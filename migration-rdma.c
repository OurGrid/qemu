/*
 * RDMA protocol and interfaces
 *
 * Copyright IBM, Corp. 2010-2013
 *
 * Authors:
 *  Michael R. Hines <mrhines@us.ibm.com>
 *  Jiuxing Liu <jl@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 *
 */
#include "qemu-common.h"
#include "migration/migration.h"
#include "migration/qemu-file.h"
#include "exec/cpu-common.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "qemu/bitmap.h"
#include "block/coroutine.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <rdma/rdma_cma.h>

#define DEBUG_RDMA
//#define DEBUG_RDMA_VERBOSE
//#define DEBUG_RDMA_REALLY_VERBOSE

#ifdef DEBUG_RDMA
#define DPRINTF(fmt, ...) \
    do { printf("rdma: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#ifdef DEBUG_RDMA_VERBOSE
#define DDPRINTF(fmt, ...) \
    do { printf("rdma: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DDPRINTF(fmt, ...) \
    do { } while (0)
#endif

#ifdef DEBUG_RDMA_REALLY_VERBOSE
#define DDDPRINTF(fmt, ...) \
    do { printf("rdma: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DDDPRINTF(fmt, ...) \
    do { } while (0)
#endif

/*
 * Print and error on both the Monitor and the Log file.
 */
#define ERROR(errp, fmt, ...) \
    do { \
        fprintf(stderr, "RDMA ERROR: " fmt, ## __VA_ARGS__); \
        if (errp && (*(errp) == NULL)) { \
            error_setg(errp, "RDMA ERROR: " fmt, ## __VA_ARGS__); \
        } \
    } while (0)

#define RDMA_RESOLVE_TIMEOUT_MS 10000

/* Do not merge data if larger than this. */
#define RDMA_MERGE_MAX (2 * 1024 * 1024)
#define RDMA_SIGNALED_SEND_MAX (RDMA_MERGE_MAX / 4096)

#define RDMA_REG_CHUNK_SHIFT 20 /* 1 MB */

/*
 * This is only for non-live state being migrated.
 * Instead of RDMA_WRITE messages, we use RDMA_SEND
 * messages for that state, which requires a different
 * delivery design than main memory.
 */
#define RDMA_SEND_INCREMENT 32768

/*
 * Maximum size infiniband SEND message
 */
#define RDMA_CONTROL_MAX_BUFFER (512 * 1024)
#define RDMA_CONTROL_MAX_COMMANDS_PER_MESSAGE 4096

#define RDMA_CONTROL_VERSION_CURRENT 1
/*
 * Capabilities for negotiation.
 */
#define RDMA_CAPABILITY_PIN_ALL 0x01

/*
 * Add the other flags above to this list of known capabilities
 * as they are introduced.
 */
static uint32_t known_capabilities = RDMA_CAPABILITY_PIN_ALL;

#define CHECK_ERROR_STATE() \
    do { \
        if (rdma->error_state) { \
            if (!rdma->error_reported) { \
                fprintf(stderr, "RDMA is in an error state waiting migration" \
                                " to abort!\n"); \
                rdma->error_reported = 1; \
            } \
            return rdma->error_state; \
        } \
    } while (0);

/*
 * A work request ID is 64-bits and we split up these bits
 * into 3 parts:
 *
 * bits 0-15 : type of control message, 2^16
 * bits 16-29: ram block index, 2^14
 * bits 30-63: ram block chunk number, 2^34
 *
 * The last two bit ranges are only used for RDMA writes,
 * in order to track their completion and potentially
 * also track unregistration status of the message.
 */
#define RDMA_WRID_TYPE_SHIFT  0UL
#define RDMA_WRID_BLOCK_SHIFT 16UL
#define RDMA_WRID_CHUNK_SHIFT 30UL

#define RDMA_WRID_TYPE_MASK \
    ((1UL << RDMA_WRID_BLOCK_SHIFT) - 1UL)

#define RDMA_WRID_BLOCK_MASK \
    (~RDMA_WRID_TYPE_MASK & ((1UL << RDMA_WRID_CHUNK_SHIFT) - 1UL))

#define RDMA_WRID_CHUNK_MASK (~RDMA_WRID_BLOCK_MASK & ~RDMA_WRID_TYPE_MASK)

/*
 * RDMA migration protocol:
 * 1. RDMA Writes (data messages, i.e. RAM)
 * 2. IB Send/Recv (control channel messages)
 */
enum {
    RDMA_WRID_NONE = 0,
    RDMA_WRID_RDMA_WRITE = 1,
    RDMA_WRID_SEND_CONTROL = 2000,
    RDMA_WRID_RECV_CONTROL = 4000,
};

const char *wrid_desc[] = {
    [RDMA_WRID_NONE] = "NONE",
    [RDMA_WRID_RDMA_WRITE] = "WRITE RDMA",
    [RDMA_WRID_SEND_CONTROL] = "CONTROL SEND",
    [RDMA_WRID_RECV_CONTROL] = "CONTROL RECV",
};

/*
 * Work request IDs for IB SEND messages only (not RDMA writes).
 * This is used by the migration protocol to transmit
 * control messages (such as device state and registration commands)
 *
 * We could use more WRs, but we have enough for now.
 */
enum {
    RDMA_WRID_READY = 0,
    RDMA_WRID_DATA,
    RDMA_WRID_CONTROL,
    RDMA_WRID_MAX,
};

/*
 * SEND/RECV IB Control Messages.
 */
enum {
    RDMA_CONTROL_NONE = 0,
    RDMA_CONTROL_ERROR,
    RDMA_CONTROL_READY,               /* ready to receive */
    RDMA_CONTROL_QEMU_FILE,           /* QEMUFile-transmitted bytes */
    RDMA_CONTROL_RAM_BLOCKS_REQUEST,  /* RAMBlock synchronization */
    RDMA_CONTROL_RAM_BLOCKS_RESULT,   /* RAMBlock synchronization */
    RDMA_CONTROL_COMPRESS,            /* page contains repeat values */
    RDMA_CONTROL_REGISTER_REQUEST,    /* dynamic page registration */
    RDMA_CONTROL_REGISTER_RESULT,     /* key to use after registration */
    RDMA_CONTROL_REGISTER_FINISHED,   /* current iteration finished */
    RDMA_CONTROL_UNREGISTER_REQUEST,  /* dynamic UN-registration */
    RDMA_CONTROL_UNREGISTER_FINISHED, /* unpinning finished */
};

const char *control_desc[] = {
    [RDMA_CONTROL_NONE] = "NONE",
    [RDMA_CONTROL_ERROR] = "ERROR",
    [RDMA_CONTROL_READY] = "READY",
    [RDMA_CONTROL_QEMU_FILE] = "QEMU FILE",
    [RDMA_CONTROL_RAM_BLOCKS_REQUEST] = "RAM BLOCKS REQUEST",
    [RDMA_CONTROL_RAM_BLOCKS_RESULT] = "RAM BLOCKS RESULT",
    [RDMA_CONTROL_COMPRESS] = "COMPRESS",
    [RDMA_CONTROL_REGISTER_REQUEST] = "REGISTER REQUEST",
    [RDMA_CONTROL_REGISTER_RESULT] = "REGISTER RESULT",
    [RDMA_CONTROL_REGISTER_FINISHED] = "REGISTER FINISHED",
    [RDMA_CONTROL_UNREGISTER_REQUEST] = "UNREGISTER REQUEST",
    [RDMA_CONTROL_UNREGISTER_FINISHED] = "UNREGISTER FINISHED",
};

/*
 * Memory and MR structures used to represent an IB Send/Recv work request.
 * This is *not* used for RDMA writes, only IB Send/Recv.
 */
typedef struct {
    uint8_t  control[RDMA_CONTROL_MAX_BUFFER]; /* actual buffer to register */
    struct   ibv_mr *control_mr;               /* registration metadata */
    size_t   control_len;                      /* length of the message */
    uint8_t *control_curr;                     /* start of unconsumed bytes */
} RDMAWorkRequestData;

/*
 * Negotiate RDMA capabilities during connection-setup time.
 */
typedef struct {
    uint32_t version;
    uint32_t flags;
} RDMACapabilities;

static void caps_to_network(RDMACapabilities *cap)
{
    cap->version = htonl(cap->version);
    cap->flags = htonl(cap->flags);
}

static void network_to_caps(RDMACapabilities *cap)
{
    cap->version = ntohl(cap->version);
    cap->flags = ntohl(cap->flags);
}

/*
 * Representation of a RAMBlock from an RDMA perspective.
 * This is not transmitted, only local.
 * This and subsequent structures cannot be linked lists
 * because we're using a single IB message to transmit
 * the information. It's small anyway, so a list is overkill.
 */
typedef struct RDMALocalBlock {
    uint8_t  *local_host_addr; /* local virtual address */
    uint64_t remote_host_addr; /* remote virtual address */
    uint64_t offset;
    uint64_t length;
    struct   ibv_mr **pmr;     /* MRs for chunk-level registration */
    struct   ibv_mr *mr;       /* MR for non-chunk-level registration */
    uint32_t *remote_keys;     /* rkeys for chunk-level registration */
    uint32_t remote_rkey;      /* rkeys for non-chunk-level registration */
    int      index;            /* which block are we */
    bool     is_ram_block;
    int      nb_chunks;
    unsigned long *transit_bitmap;
    unsigned long *unregister_bitmap;
} RDMALocalBlock;

/*
 * Also represents a RAMblock, but only on the dest.
 * This gets transmitted by the dest during connection-time
 * to the source VM and then is used to populate the
 * corresponding RDMALocalBlock with
 * the information needed to perform the actual RDMA.
 */
typedef struct QEMU_PACKED RDMARemoteBlock {
    uint64_t remote_host_addr;
    uint64_t offset;
    uint64_t length;
    uint32_t remote_rkey;
    uint32_t padding;
} RDMARemoteBlock;

static uint64_t htonll(uint64_t v)
{
    union { uint32_t lv[2]; uint64_t llv; } u;
    u.lv[0] = htonl(v >> 32);
    u.lv[1] = htonl(v & 0xFFFFFFFFULL);
    return u.llv;
}

static uint64_t ntohll(uint64_t v) {
    union { uint32_t lv[2]; uint64_t llv; } u;
    u.llv = v;
    return ((uint64_t)ntohl(u.lv[0]) << 32) | (uint64_t) ntohl(u.lv[1]);
}

static void remote_block_to_network(RDMARemoteBlock *rb)
{
    rb->remote_host_addr = htonll(rb->remote_host_addr);
    rb->offset = htonll(rb->offset);
    rb->length = htonll(rb->length);
    rb->remote_rkey = htonl(rb->remote_rkey);
}

static void network_to_remote_block(RDMARemoteBlock *rb)
{
    rb->remote_host_addr = ntohll(rb->remote_host_addr);
    rb->offset = ntohll(rb->offset);
    rb->length = ntohll(rb->length);
    rb->remote_rkey = ntohl(rb->remote_rkey);
}

/*
 * Virtual address of the above structures used for transmitting
 * the RAMBlock descriptions at connection-time.
 * This structure is *not* transmitted.
 */
typedef struct RDMALocalBlocks {
    int nb_blocks;
    bool     init;             /* main memory init complete */
    RDMALocalBlock *block;
} RDMALocalBlocks;

/*
 * Main data structure for RDMA state.
 * While there is only one copy of this structure being allocated right now,
 * this is the place where one would start if you wanted to consider
 * having more than one RDMA connection open at the same time.
 */
typedef struct RDMAContext {
    char *host;
    int port;

    RDMAWorkRequestData wr_data[RDMA_WRID_MAX + 1];

    /*
     * This is used by *_exchange_send() to figure out whether or not
     * the initial "READY" message has already been received or not.
     * This is because other functions may potentially poll() and detect
     * the READY message before send() does, in which case we need to
     * know if it completed.
     */
    int control_ready_expected;

    /* number of outstanding writes */
    int nb_sent;

    /* store info about current buffer so that we can
       merge it with future sends */
    uint64_t current_addr;
    uint64_t current_length;
    /* index of ram block the current buffer belongs to */
    int current_index;
    /* index of the chunk in the current ram block */
    int current_chunk;

    bool pin_all;

    /*
     * infiniband-specific variables for opening the device
     * and maintaining connection state and so forth.
     *
     * cm_id also has ibv_context, rdma_event_channel, and ibv_qp in
     * cm_id->verbs, cm_id->channel, and cm_id->qp.
     */
    struct rdma_cm_id *cm_id;               /* connection manager ID */
    struct rdma_cm_id *listen_id;

    struct ibv_context          *verbs;
    struct rdma_event_channel   *channel;
    struct ibv_qp *qp;                      /* queue pair */
    struct ibv_comp_channel *comp_channel;  /* completion channel */
    struct ibv_pd *pd;                      /* protection domain */
    struct ibv_cq *cq;                      /* completion queue */

    /*
     * If a previous write failed (perhaps because of a failed
     * memory registration, then do not attempt any future work
     * and remember the error state.
     */
    int error_state;
    int error_reported;

    /*
     * Description of ram blocks used throughout the code.
     */
    RDMALocalBlocks local_ram_blocks;
    RDMARemoteBlock *block;

    /*
     * Migration on *destination* started.
     * Then use coroutine yield function.
     * Source runs in a thread, so we don't care.
     */
    int migration_started_on_destination;

    int total_registrations;
    int total_writes;

    int unregister_current, unregister_next;
    uint64_t unregistrations[RDMA_SIGNALED_SEND_MAX];

    GHashTable *blockmap;
} RDMAContext;

/*
 * Interface to the rest of the migration call stack.
 */
typedef struct QEMUFileRDMA {
    RDMAContext *rdma;
    size_t len;
    void *file;
} QEMUFileRDMA;

/*
 * Main structure for IB Send/Recv control messages.
 * This gets prepended at the beginning of every Send/Recv.
 */
typedef struct QEMU_PACKED {
    uint32_t len;     /* Total length of data portion */
    uint32_t type;    /* which control command to perform */
    uint32_t repeat;  /* number of commands in data portion of same type */
    uint32_t padding;
} RDMAControlHeader;

static void control_to_network(RDMAControlHeader *control)
{
    control->type = htonl(control->type);
    control->len = htonl(control->len);
    control->repeat = htonl(control->repeat);
}

static void network_to_control(RDMAControlHeader *control)
{
    control->type = ntohl(control->type);
    control->len = ntohl(control->len);
    control->repeat = ntohl(control->repeat);
}

/*
 * Register a single Chunk.
 * Information sent by the source VM to inform the dest
 * to register an single chunk of memory before we can perform
 * the actual RDMA operation.
 */
typedef struct QEMU_PACKED {
    union QEMU_PACKED {
        uint64_t current_addr;  /* offset into the ramblock of the chunk */
        uint64_t chunk;         /* chunk to lookup if unregistering */
    } key;
    uint32_t current_index; /* which ramblock the chunk belongs to */
    uint32_t padding;
    uint64_t chunks;            /* how many sequential chunks to register */
} RDMARegister;

static void register_to_network(RDMARegister *reg)
{
    reg->key.current_addr = htonll(reg->key.current_addr);
    reg->current_index = htonl(reg->current_index);
    reg->chunks = htonll(reg->chunks);
}

static void network_to_register(RDMARegister *reg)
{
    reg->key.current_addr = ntohll(reg->key.current_addr);
    reg->current_index = ntohl(reg->current_index);
    reg->chunks = ntohll(reg->chunks);
}

typedef struct QEMU_PACKED {
    uint32_t value;     /* if zero, we will madvise() */
    uint32_t block_idx; /* which ram block index */
    uint64_t offset;    /* where in the remote ramblock this chunk */
    uint64_t length;    /* length of the chunk */
} RDMACompress;

static void compress_to_network(RDMACompress *comp)
{
    comp->value = htonl(comp->value);
    comp->block_idx = htonl(comp->block_idx);
    comp->offset = htonll(comp->offset);
    comp->length = htonll(comp->length);
}

static void network_to_compress(RDMACompress *comp)
{
    comp->value = ntohl(comp->value);
    comp->block_idx = ntohl(comp->block_idx);
    comp->offset = ntohll(comp->offset);
    comp->length = ntohll(comp->length);
}

/*
 * The result of the dest's memory registration produces an "rkey"
 * which the source VM must reference in order to perform
 * the RDMA operation.
 */
typedef struct QEMU_PACKED {
    uint32_t rkey;
    uint32_t padding;
    uint64_t host_addr;
} RDMARegisterResult;

static void result_to_network(RDMARegisterResult *result)
{
    result->rkey = htonl(result->rkey);
    result->host_addr = htonll(result->host_addr);
};

static void network_to_result(RDMARegisterResult *result)
{
    result->rkey = ntohl(result->rkey);
    result->host_addr = ntohll(result->host_addr);
};

const char *print_wrid(int wrid);
static int qemu_rdma_exchange_send(RDMAContext *rdma, RDMAControlHeader *head,
                                   uint8_t *data, RDMAControlHeader *resp,
                                   int *resp_idx,
                                   int (*callback)(RDMAContext *rdma));

static inline uint64_t ram_chunk_index(uint8_t *start, uint8_t *host)
{
    return ((uintptr_t) host - (uintptr_t) start) >> RDMA_REG_CHUNK_SHIFT;
}

static inline uint8_t *ram_chunk_start(RDMALocalBlock *rdma_ram_block,
                                       uint64_t i)
{
    return (uint8_t *) (((uintptr_t) rdma_ram_block->local_host_addr)
                                    + (i << RDMA_REG_CHUNK_SHIFT));
}

static inline uint8_t *ram_chunk_end(RDMALocalBlock *rdma_ram_block, uint64_t i)
{
    uint8_t *result = ram_chunk_start(rdma_ram_block, i) +
                                         (1UL << RDMA_REG_CHUNK_SHIFT);

    if (result > (rdma_ram_block->local_host_addr + rdma_ram_block->length)) {
        result = rdma_ram_block->local_host_addr + rdma_ram_block->length;
    }

    return result;
}

static int __qemu_rdma_add_block(RDMAContext *rdma, void *host_addr,
                         ram_addr_t block_offset, uint64_t length)
{
    RDMALocalBlocks *local = &rdma->local_ram_blocks;
    RDMALocalBlock *block = g_hash_table_lookup(rdma->blockmap,
        (void *) block_offset);
    RDMALocalBlock *old = local->block;

    assert(block == NULL);

    local->block = g_malloc0(sizeof(RDMALocalBlock) * (local->nb_blocks + 1));

    if (local->nb_blocks) {
        int x;

        for (x = 0; x < local->nb_blocks; x++) {
            g_hash_table_remove(rdma->blockmap, (void *)old[x].offset);
            g_hash_table_insert(rdma->blockmap, (void *)old[x].offset,
                                                &local->block[x]);
        }
        memcpy(local->block, old, sizeof(RDMALocalBlock) * local->nb_blocks);
        g_free(old);
    }

    block = &local->block[local->nb_blocks];

    block->local_host_addr = host_addr;
    block->offset = block_offset;
    block->length = length;
    block->index = local->nb_blocks;
    block->nb_chunks = ram_chunk_index(host_addr, host_addr + length) + 1UL;
    block->transit_bitmap = bitmap_new(block->nb_chunks);
    bitmap_clear(block->transit_bitmap, 0, block->nb_chunks);
    block->unregister_bitmap = bitmap_new(block->nb_chunks);
    bitmap_clear(block->unregister_bitmap, 0, block->nb_chunks);
    block->remote_keys = g_malloc0(block->nb_chunks * sizeof(uint32_t));

    block->is_ram_block = local->init ? false : true;

    g_hash_table_insert(rdma->blockmap, (void *) block_offset, block);

    DDPRINTF("Added Block: %d, addr: %" PRIu64 ", offset: %" PRIu64
           " length: %" PRIu64 " end: %" PRIu64 " bits %" PRIu64 " chunks %d\n",
            local->nb_blocks, (uint64_t) block->local_host_addr, block->offset,
            block->length, (uint64_t) (block->local_host_addr + block->length),
                BITS_TO_LONGS(block->nb_chunks) *
                    sizeof(unsigned long) * 8, block->nb_chunks);

    local->nb_blocks++;

    return 0;
}

/*
 * Memory regions need to be registered with the device and queue pairs setup
 * in advanced before the migration starts. This tells us where the RAM blocks
 * are so that we can register them individually.
 */
static void qemu_rdma_init_one_block(void *host_addr,
    ram_addr_t block_offset, ram_addr_t length, void *opaque)
{
    __qemu_rdma_add_block(opaque, host_addr, block_offset, length);
}

/*
 * Identify the RAMBlocks and their quantity. They will be references to
 * identify chunk boundaries inside each RAMBlock and also be referenced
 * during dynamic page registration.
 */
static int qemu_rdma_init_ram_blocks(RDMAContext *rdma)
{
    RDMALocalBlocks *local = &rdma->local_ram_blocks;

    assert(rdma->blockmap == NULL);
    rdma->blockmap = g_hash_table_new(g_direct_hash, g_direct_equal);
    memset(local, 0, sizeof *local);
    qemu_ram_foreach_block(qemu_rdma_init_one_block, rdma);
    DPRINTF("Allocated %d local ram block structures\n", local->nb_blocks);
    rdma->block = (RDMARemoteBlock *) g_malloc0(sizeof(RDMARemoteBlock) *
                        rdma->local_ram_blocks.nb_blocks);
    local->init = true;
    return 0;
}

static int __qemu_rdma_delete_block(RDMAContext *rdma, ram_addr_t block_offset)
{
    RDMALocalBlocks *local = &rdma->local_ram_blocks;
    RDMALocalBlock *block = g_hash_table_lookup(rdma->blockmap,
        (void *) block_offset);
    RDMALocalBlock *old = local->block;
    int x;

    assert(block);

    if (block->pmr) {
        int j;

        for (j = 0; j < block->nb_chunks; j++) {
            if (!block->pmr[j]) {
                continue;
            }
            ibv_dereg_mr(block->pmr[j]);
            rdma->total_registrations--;
        }
        g_free(block->pmr);
        block->pmr = NULL;
    }

    if (block->mr) {
        ibv_dereg_mr(block->mr);
        rdma->total_registrations--;
        block->mr = NULL;
    }

    g_free(block->transit_bitmap);
    block->transit_bitmap = NULL;

    g_free(block->unregister_bitmap);
    block->unregister_bitmap = NULL;

    g_free(block->remote_keys);
    block->remote_keys = NULL;

    for (x = 0; x < local->nb_blocks; x++) {
        g_hash_table_remove(rdma->blockmap, (void *)old[x].offset);
    }

    if (local->nb_blocks > 1) {

        local->block = g_malloc0(sizeof(RDMALocalBlock) *
                                    (local->nb_blocks - 1));

        if (block->index) {
            memcpy(local->block, old, sizeof(RDMALocalBlock) * block->index);
        }

        if (block->index < (local->nb_blocks - 1)) {
            memcpy(local->block + block->index, old + (block->index + 1),
                sizeof(RDMALocalBlock) *
                    (local->nb_blocks - (block->index + 1)));
        }
    } else {
        assert(block == local->block);
        local->block = NULL;
    }

    DDPRINTF("Deleted Block: %d, addr: %" PRIu64 ", offset: %" PRIu64
           " length: %" PRIu64 " end: %" PRIu64 " bits %" PRIu64 " chunks %d\n",
            local->nb_blocks, (uint64_t) block->local_host_addr, block->offset,
            block->length, (uint64_t) (block->local_host_addr + block->length),
                BITS_TO_LONGS(block->nb_chunks) *
                    sizeof(unsigned long) * 8, block->nb_chunks);

    g_free(old);

    local->nb_blocks--;

    if (local->nb_blocks) {
        for (x = 0; x < local->nb_blocks; x++) {
            g_hash_table_insert(rdma->blockmap, (void *)local->block[x].offset,
                                                &local->block[x]);
        }
    }

    return 0;
}

/*
 * Put in the log file which RDMA device was opened and the details
 * associated with that device.
 */
static void qemu_rdma_dump_id(const char *who, struct ibv_context *verbs)
{
    printf("%s RDMA Device opened: kernel name %s "
           "uverbs device name %s, "
           "infiniband_verbs class device path %s,"
           " infiniband class device path %s\n",
                who,
                verbs->device->name,
                verbs->device->dev_name,
                verbs->device->dev_path,
                verbs->device->ibdev_path);
}

/*
 * Put in the log file the RDMA gid addressing information,
 * useful for folks who have trouble understanding the
 * RDMA device hierarchy in the kernel.
 */
static void qemu_rdma_dump_gid(const char *who, struct rdma_cm_id *id)
{
    char sgid[33];
    char dgid[33];
    inet_ntop(AF_INET6, &id->route.addr.addr.ibaddr.sgid, sgid, sizeof sgid);
    inet_ntop(AF_INET6, &id->route.addr.addr.ibaddr.dgid, dgid, sizeof dgid);
    DPRINTF("%s Source GID: %s, Dest GID: %s\n", who, sgid, dgid);
}

/*
 * Figure out which RDMA device corresponds to the requested IP hostname
 * Also create the initial connection manager identifiers for opening
 * the connection.
 */
static int qemu_rdma_resolve_host(RDMAContext *rdma, Error **errp)
{
    int ret;
    struct addrinfo *res;
    char port_str[16];
    struct rdma_cm_event *cm_event;
    char ip[40] = "unknown";

    if (rdma->host == NULL || !strcmp(rdma->host, "")) {
        ERROR(errp, "RDMA hostname has not been set\n");
        return -1;
    }

    /* create CM channel */
    rdma->channel = rdma_create_event_channel();
    if (!rdma->channel) {
        ERROR(errp, "could not create CM channel\n");
        return -1;
    }

    /* create CM id */
    ret = rdma_create_id(rdma->channel, &rdma->cm_id, NULL, RDMA_PS_TCP);
    if (ret) {
        ERROR(errp, "could not create channel id\n");
        goto err_resolve_create_id;
    }

    snprintf(port_str, 16, "%d", rdma->port);
    port_str[15] = '\0';

    ret = getaddrinfo(rdma->host, port_str, NULL, &res);
    if (ret < 0) {
        ERROR(errp, "could not getaddrinfo address %s\n", rdma->host);
        goto err_resolve_get_addr;
    }

    inet_ntop(AF_INET, &((struct sockaddr_in *) res->ai_addr)->sin_addr,
                                ip, sizeof ip);
    DPRINTF("%s => %s\n", rdma->host, ip);

    /* resolve the first address */
    ret = rdma_resolve_addr(rdma->cm_id, NULL, res->ai_addr,
            RDMA_RESOLVE_TIMEOUT_MS);
    if (ret) {
        ERROR(errp, "could not resolve address %s\n", rdma->host);
        goto err_resolve_get_addr;
    }

    qemu_rdma_dump_gid("source_resolve_addr", rdma->cm_id);

    ret = rdma_get_cm_event(rdma->channel, &cm_event);
    if (ret) {
        ERROR(errp, "could not perform event_addr_resolved\n");
        goto err_resolve_get_addr;
    }

    if (cm_event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
        ERROR(errp, "result not equal to event_addr_resolved %s\n",
                rdma_event_str(cm_event->event));
        perror("rdma_resolve_addr");
        goto err_resolve_get_addr;
    }
    rdma_ack_cm_event(cm_event);

    /* resolve route */
    ret = rdma_resolve_route(rdma->cm_id, RDMA_RESOLVE_TIMEOUT_MS);
    if (ret) {
        ERROR(errp, "could not resolve rdma route\n");
        goto err_resolve_get_addr;
    }

    ret = rdma_get_cm_event(rdma->channel, &cm_event);
    if (ret) {
        ERROR(errp, "could not perform event_route_resolved\n");
        goto err_resolve_get_addr;
    }
    if (cm_event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
        ERROR(errp, "result not equal to event_route_resolved: %s\n",
                        rdma_event_str(cm_event->event));
        rdma_ack_cm_event(cm_event);
        goto err_resolve_get_addr;
    }
    rdma_ack_cm_event(cm_event);
    rdma->verbs = rdma->cm_id->verbs;
    qemu_rdma_dump_id("source_resolve_host", rdma->cm_id->verbs);
    qemu_rdma_dump_gid("source_resolve_host", rdma->cm_id);
    return 0;

err_resolve_get_addr:
    rdma_destroy_id(rdma->cm_id);
    rdma->cm_id = NULL;
err_resolve_create_id:
    rdma_destroy_event_channel(rdma->channel);
    rdma->channel = NULL;

    return -1;
}

/*
 * Create protection domain and completion queues
 */
static int qemu_rdma_alloc_pd_cq(RDMAContext *rdma)
{
    /* allocate pd */
    rdma->pd = ibv_alloc_pd(rdma->verbs);
    if (!rdma->pd) {
        fprintf(stderr, "failed to allocate protection domain\n");
        return -1;
    }

    /* create completion channel */
    rdma->comp_channel = ibv_create_comp_channel(rdma->verbs);
    if (!rdma->comp_channel) {
        fprintf(stderr, "failed to allocate completion channel\n");
        goto err_alloc_pd_cq;
    }

    /*
     * Completion queue can be filled by both read and write work requests,
     * so must reflect the sum of both possible queue sizes.
     */
    rdma->cq = ibv_create_cq(rdma->verbs, (RDMA_SIGNALED_SEND_MAX * 3),
            NULL, rdma->comp_channel, 0);
    if (!rdma->cq) {
        fprintf(stderr, "failed to allocate completion queue\n");
        goto err_alloc_pd_cq;
    }

    return 0;

err_alloc_pd_cq:
    if (rdma->pd) {
        ibv_dealloc_pd(rdma->pd);
    }
    if (rdma->comp_channel) {
        ibv_destroy_comp_channel(rdma->comp_channel);
    }
    rdma->pd = NULL;
    rdma->comp_channel = NULL;
    return -1;

}

/*
 * Create queue pairs.
 */
static int qemu_rdma_alloc_qp(RDMAContext *rdma)
{
    struct ibv_qp_init_attr attr = { 0 };
    int ret;

    attr.cap.max_send_wr = RDMA_SIGNALED_SEND_MAX;
    attr.cap.max_recv_wr = 3;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;
    attr.send_cq = rdma->cq;
    attr.recv_cq = rdma->cq;
    attr.qp_type = IBV_QPT_RC;

    ret = rdma_create_qp(rdma->cm_id, rdma->pd, &attr);
    if (ret) {
        return -1;
    }

    rdma->qp = rdma->cm_id->qp;
    return 0;
}

static int qemu_rdma_reg_whole_ram_blocks(RDMAContext *rdma)
{
    int i;
    RDMALocalBlocks *local = &rdma->local_ram_blocks;

    for (i = 0; i < local->nb_blocks; i++) {
        local->block[i].mr =
            ibv_reg_mr(rdma->pd,
                    local->block[i].local_host_addr,
                    local->block[i].length,
                    IBV_ACCESS_LOCAL_WRITE |
                    IBV_ACCESS_REMOTE_WRITE
                    );
        if (!local->block[i].mr) {
            perror("Failed to register local dest ram block!\n");
            break;
        }
        rdma->total_registrations++;
    }

    if (i >= local->nb_blocks) {
        return 0;
    }

    for (i--; i >= 0; i--) {
        ibv_dereg_mr(local->block[i].mr);
        rdma->total_registrations--;
    }

    return -1;

}

/*
 * Find the ram block that corresponds to the page requested to be
 * transmitted by QEMU.
 *
 * Once the block is found, also identify which 'chunk' within that
 * block that the page belongs to.
 *
 * This search cannot fail or the migration will fail.
 */
static int qemu_rdma_search_ram_block(RDMAContext *rdma,
                                      uint64_t block_offset,
                                      uint64_t offset,
                                      uint64_t length,
                                      uint64_t *block_index,
                                      uint64_t *chunk_index)
{
    uint64_t current_addr = block_offset + offset;
    RDMALocalBlock *block = g_hash_table_lookup(rdma->blockmap,
                                                (void *) block_offset);
    assert(block);
    assert(current_addr >= block->offset);
    assert((current_addr + length) <= (block->offset + block->length));

    *block_index = block->index;
    *chunk_index = ram_chunk_index(block->local_host_addr,
                block->local_host_addr + (current_addr - block->offset));

    return 0;
}

/*
 * Register a chunk with IB. If the chunk was already registered
 * previously, then skip.
 *
 * Also return the keys associated with the registration needed
 * to perform the actual RDMA operation.
 */
static int qemu_rdma_register_and_get_keys(RDMAContext *rdma,
        RDMALocalBlock *block, uint8_t *host_addr,
        uint32_t *lkey, uint32_t *rkey, int chunk,
        uint8_t *chunk_start, uint8_t *chunk_end)
{
    if (block->mr) {
        if (lkey) {
            *lkey = block->mr->lkey;
        }
        if (rkey) {
            *rkey = block->mr->rkey;
        }
        return 0;
    }

    /* allocate memory to store chunk MRs */
    if (!block->pmr) {
        block->pmr = g_malloc0(block->nb_chunks * sizeof(struct ibv_mr *));
        if (!block->pmr) {
            return -1;
        }
    }

    /*
     * If 'rkey', then we're the destination, so grant access to the source.
     *
     * If 'lkey', then we're the source VM, so grant access only to ourselves.
     */
    if (!block->pmr[chunk]) {
        uint64_t len = chunk_end - chunk_start;

        DDPRINTF("Registering %" PRIu64 " bytes @ %p\n",
                 len, chunk_start);

        block->pmr[chunk] = ibv_reg_mr(rdma->pd,
                chunk_start, len,
                (rkey ? (IBV_ACCESS_LOCAL_WRITE |
                        IBV_ACCESS_REMOTE_WRITE) : 0));

        if (!block->pmr[chunk]) {
            perror("Failed to register chunk!");
            fprintf(stderr, "Chunk details: block: %d chunk index %d"
                            " start %" PRIu64 " end %" PRIu64 " host %" PRIu64
                            " local %" PRIu64 " registrations: %d\n",
                            block->index, chunk, (uint64_t) chunk_start,
                            (uint64_t) chunk_end, (uint64_t) host_addr,
                            (uint64_t) block->local_host_addr,
                            rdma->total_registrations);
            return -1;
        }
        rdma->total_registrations++;
    }

    if (lkey) {
        *lkey = block->pmr[chunk]->lkey;
    }
    if (rkey) {
        *rkey = block->pmr[chunk]->rkey;
    }
    return 0;
}

/*
 * Register (at connection time) the memory used for control
 * channel messages.
 */
static int qemu_rdma_reg_control(RDMAContext *rdma, int idx)
{
    rdma->wr_data[idx].control_mr = ibv_reg_mr(rdma->pd,
            rdma->wr_data[idx].control, RDMA_CONTROL_MAX_BUFFER,
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (rdma->wr_data[idx].control_mr) {
        rdma->total_registrations++;
        return 0;
    }
    fprintf(stderr, "qemu_rdma_reg_control failed!\n");
    return -1;
}

const char *print_wrid(int wrid)
{
    if (wrid >= RDMA_WRID_RECV_CONTROL) {
        return wrid_desc[RDMA_WRID_RECV_CONTROL];
    }
    return wrid_desc[wrid];
}

/*
 * RDMA requires memory registration (mlock/pinning), but this is not good for
 * overcommitment.
 *
 * In preparation for the future where LRU information or workload-specific
 * writable writable working set memory access behavior is available to QEMU
 * it would be nice to have in place the ability to UN-register/UN-pin
 * particular memory regions from the RDMA hardware when it is determine that
 * those regions of memory will likely not be accessed again in the near future.
 *
 * While we do not yet have such information right now, the following
 * compile-time option allows us to perform a non-optimized version of this
 * behavior.
 *
 * By uncommenting this option, you will cause *all* RDMA transfers to be
 * unregistered immediately after the transfer completes on both sides of the
 * connection. This has no effect in 'rdma-pin-all' mode, only regular mode.
 *
 * This will have a terrible impact on migration performance, so until future
 * workload information or LRU information is available, do not attempt to use
 * this feature except for basic testing.
 */
//#define RDMA_UNREGISTRATION_EXAMPLE

/*
 * Perform a non-optimized memory unregistration after every transfer
 * for demonsration purposes, only if pin-all is not requested.
 *
 * Potential optimizations:
 * 1. Start a new thread to run this function continuously
        - for bit clearing
        - and for receipt of unregister messages
 * 2. Use an LRU.
 * 3. Use workload hints.
 */
static int qemu_rdma_unregister_waiting(RDMAContext *rdma)
{
    while (rdma->unregistrations[rdma->unregister_current]) {
        int ret;
        uint64_t wr_id = rdma->unregistrations[rdma->unregister_current];
        uint64_t chunk =
            (wr_id & RDMA_WRID_CHUNK_MASK) >> RDMA_WRID_CHUNK_SHIFT;
        uint64_t index =
            (wr_id & RDMA_WRID_BLOCK_MASK) >> RDMA_WRID_BLOCK_SHIFT;
        RDMALocalBlock *block =
            &(rdma->local_ram_blocks.block[index]);
        RDMARegister reg = { .current_index = index };
        RDMAControlHeader resp = { .type = RDMA_CONTROL_UNREGISTER_FINISHED,
                                 };
        RDMAControlHeader head = { .len = sizeof(RDMARegister),
                                   .type = RDMA_CONTROL_UNREGISTER_REQUEST,
                                   .repeat = 1,
                                 };

        DDPRINTF("Processing unregister for chunk: %" PRIu64
                 " at position %d\n", chunk, rdma->unregister_current);

        rdma->unregistrations[rdma->unregister_current] = 0;
        rdma->unregister_current++;

        if (rdma->unregister_current == RDMA_SIGNALED_SEND_MAX) {
            rdma->unregister_current = 0;
        }


        /*
         * Unregistration is speculative (because migration is single-threaded
         * and we cannot break the protocol's inifinband message ordering).
         * Thus, if the memory is currently being used for transmission,
         * then abort the attempt to unregister and try again
         * later the next time a completion is received for this memory.
         */
        clear_bit(chunk, block->unregister_bitmap);

        if (test_bit(chunk, block->transit_bitmap)) {
            DDPRINTF("Cannot unregister inflight chunk: %" PRIu64 "\n", chunk);
            continue;
        }

        DDPRINTF("Sending unregister for chunk: %" PRIu64 "\n", chunk);

        ret = ibv_dereg_mr(block->pmr[chunk]);
        block->pmr[chunk] = NULL;
        block->remote_keys[chunk] = 0;

        if (ret != 0) {
            perror("unregistration chunk failed");
            return -ret;
        }
        rdma->total_registrations--;

        reg.key.chunk = chunk;
        register_to_network(&reg);
        ret = qemu_rdma_exchange_send(rdma, &head, (uint8_t *) &reg,
                                &resp, NULL, NULL);
        if (ret < 0) {
            return ret;
        }

        DDPRINTF("Unregister for chunk: %" PRIu64 " complete.\n", chunk);
    }

    return 0;
}

static uint64_t qemu_rdma_make_wrid(uint64_t wr_id, uint64_t index,
                                         uint64_t chunk)
{
    uint64_t result = wr_id & RDMA_WRID_TYPE_MASK;

    result |= (index << RDMA_WRID_BLOCK_SHIFT);
    result |= (chunk << RDMA_WRID_CHUNK_SHIFT);

    return result;
}

/*
 * Set bit for unregistration in the next iteration.
 * We cannot transmit right here, but will unpin later.
 */
static void qemu_rdma_signal_unregister(RDMAContext *rdma, uint64_t index,
                                        uint64_t chunk, uint64_t wr_id)
{
    if (rdma->unregistrations[rdma->unregister_next] != 0) {
        fprintf(stderr, "rdma migration: queue is full!\n");
    } else {
        RDMALocalBlock *block = &(rdma->local_ram_blocks.block[index]);

        if (!test_and_set_bit(chunk, block->unregister_bitmap)) {
            DDPRINTF("Appending unregister chunk %" PRIu64
                    " at position %d\n", chunk, rdma->unregister_next);

            rdma->unregistrations[rdma->unregister_next++] =
                    qemu_rdma_make_wrid(wr_id, index, chunk);

            if (rdma->unregister_next == RDMA_SIGNALED_SEND_MAX) {
                rdma->unregister_next = 0;
            }
        } else {
            DDPRINTF("Unregister chunk %" PRIu64 " already in queue.\n",
                    chunk);
        }
    }
}

/*
 * Consult the connection manager to see a work request
 * (of any kind) has completed.
 * Return the work request ID that completed.
 */
static uint64_t qemu_rdma_poll(RDMAContext *rdma, uint64_t *wr_id_out)
{
    int ret;
    struct ibv_wc wc;
    uint64_t wr_id;

    ret = ibv_poll_cq(rdma->cq, 1, &wc);

    if (!ret) {
        *wr_id_out = RDMA_WRID_NONE;
        return 0;
    }

    if (ret < 0) {
        fprintf(stderr, "ibv_poll_cq return %d!\n", ret);
        return ret;
    }

    wr_id = wc.wr_id & RDMA_WRID_TYPE_MASK;

    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "ibv_poll_cq wc.status=%d %s!\n",
                        wc.status, ibv_wc_status_str(wc.status));
        fprintf(stderr, "ibv_poll_cq wrid=%s!\n", wrid_desc[wr_id]);

        return -1;
    }

    if (rdma->control_ready_expected &&
        (wr_id >= RDMA_WRID_RECV_CONTROL)) {
        DDDPRINTF("completion %s #%" PRId64 " received (%" PRId64 ")"
                  " left %d\n", wrid_desc[RDMA_WRID_RECV_CONTROL],
                  wr_id - RDMA_WRID_RECV_CONTROL, wr_id, rdma->nb_sent);
        rdma->control_ready_expected = 0;
    }

    if (wr_id == RDMA_WRID_RDMA_WRITE) {
        uint64_t chunk =
            (wc.wr_id & RDMA_WRID_CHUNK_MASK) >> RDMA_WRID_CHUNK_SHIFT;
        uint64_t index =
            (wc.wr_id & RDMA_WRID_BLOCK_MASK) >> RDMA_WRID_BLOCK_SHIFT;
        RDMALocalBlock *block = &(rdma->local_ram_blocks.block[index]);

        DDDPRINTF("completions %s (%" PRId64 ") left %d, "
                 "block %" PRIu64 ", chunk: %" PRIu64 " %p %p\n",
                 print_wrid(wr_id), wr_id, rdma->nb_sent, index, chunk,
                 block->local_host_addr, (void *)block->remote_host_addr);

        clear_bit(chunk, block->transit_bitmap);

        if (rdma->nb_sent > 0) {
            rdma->nb_sent--;
        }

        if (!rdma->pin_all) {
            /*
             * FYI: If one wanted to signal a specific chunk to be unregistered
             * using LRU or workload-specific information, this is the function
             * you would call to do so. That chunk would then get asynchronously
             * unregistered later.
             */
#ifdef RDMA_UNREGISTRATION_EXAMPLE
            qemu_rdma_signal_unregister(rdma, index, chunk, wc.wr_id);
#endif
        }
    } else {
        DDDPRINTF("other completion %s (%" PRId64 ") received left %d\n",
            print_wrid(wr_id), wr_id, rdma->nb_sent);
    }

    *wr_id_out = wc.wr_id;

    return  0;
}

/*
 * Block until the next work request has completed.
 *
 * First poll to see if a work request has already completed,
 * otherwise block.
 *
 * If we encounter completed work requests for IDs other than
 * the one we're interested in, then that's generally an error.
 *
 * The only exception is actual RDMA Write completions. These
 * completions only need to be recorded, but do not actually
 * need further processing.
 */
static int qemu_rdma_block_for_wrid(RDMAContext *rdma, int wrid_requested)
{
    int num_cq_events = 0, ret = 0;
    struct ibv_cq *cq;
    void *cq_ctx;
    uint64_t wr_id = RDMA_WRID_NONE, wr_id_in;

    if (ibv_req_notify_cq(rdma->cq, 0)) {
        return -1;
    }
    /* poll cq first */
    while (wr_id != wrid_requested) {
        ret = qemu_rdma_poll(rdma, &wr_id_in);
        if (ret < 0) {
            return ret;
        }

        wr_id = wr_id_in & RDMA_WRID_TYPE_MASK;

        if (wr_id == RDMA_WRID_NONE) {
            break;
        }
        if (wr_id != wrid_requested) {
            DDDPRINTF("A Wanted wrid %s (%d) but got %s (%" PRIu64 ")\n",
                print_wrid(wrid_requested),
                wrid_requested, print_wrid(wr_id), wr_id);
        }
    }

    if (wr_id == wrid_requested) {
        return 0;
    }

    while (1) {
        /*
         * Coroutine doesn't start until process_incoming_migration()
         * so don't yield unless we know we're running inside of a coroutine.
         */
        if (rdma->migration_started_on_destination) {
            yield_until_fd_readable(rdma->comp_channel->fd);
        }

        if (ibv_get_cq_event(rdma->comp_channel, &cq, &cq_ctx)) {
            perror("ibv_get_cq_event");
            goto err_block_for_wrid;
        }

        num_cq_events++;

        if (ibv_req_notify_cq(cq, 0)) {
            goto err_block_for_wrid;
        }

        while (wr_id != wrid_requested) {
            ret = qemu_rdma_poll(rdma, &wr_id_in);
            if (ret < 0) {
                goto err_block_for_wrid;
            }

            wr_id = wr_id_in & RDMA_WRID_TYPE_MASK;

            if (wr_id == RDMA_WRID_NONE) {
                break;
            }
            if (wr_id != wrid_requested) {
                DDDPRINTF("B Wanted wrid %s (%d) but got %s (%" PRIu64 ")\n",
                    print_wrid(wrid_requested), wrid_requested,
                    print_wrid(wr_id), wr_id);
            }
        }

        if (wr_id == wrid_requested) {
            goto success_block_for_wrid;
        }
    }

success_block_for_wrid:
    if (num_cq_events) {
        ibv_ack_cq_events(cq, num_cq_events);
    }
    return 0;

err_block_for_wrid:
    if (num_cq_events) {
        ibv_ack_cq_events(cq, num_cq_events);
    }
    return ret;
}

/*
 * Post a SEND message work request for the control channel
 * containing some data and block until the post completes.
 */
static int qemu_rdma_post_send_control(RDMAContext *rdma, uint8_t *buf,
                                       RDMAControlHeader *head)
{
    int ret = 0;
    RDMAWorkRequestData *wr = &rdma->wr_data[RDMA_WRID_MAX];
    struct ibv_send_wr *bad_wr;
    struct ibv_sge sge = {
                           .addr = (uint64_t)(wr->control),
                           .length = head->len + sizeof(RDMAControlHeader),
                           .lkey = wr->control_mr->lkey,
                         };
    struct ibv_send_wr send_wr = {
                                   .wr_id = RDMA_WRID_SEND_CONTROL,
                                   .opcode = IBV_WR_SEND,
                                   .send_flags = IBV_SEND_SIGNALED,
                                   .sg_list = &sge,
                                   .num_sge = 1,
                                };

    DDDPRINTF("CONTROL: sending %s..\n", control_desc[head->type]);

    /*
     * We don't actually need to do a memcpy() in here if we used
     * the "sge" properly, but since we're only sending control messages
     * (not RAM in a performance-critical path), then its OK for now.
     *
     * The copy makes the RDMAControlHeader simpler to manipulate
     * for the time being.
     */
    memcpy(wr->control, head, sizeof(RDMAControlHeader));
    control_to_network((void *) wr->control);

    if (buf) {
        memcpy(wr->control + sizeof(RDMAControlHeader), buf, head->len);
    }


    if (ibv_post_send(rdma->qp, &send_wr, &bad_wr)) {
        return -1;
    }

    if (ret < 0) {
        fprintf(stderr, "Failed to use post IB SEND for control!\n");
        return ret;
    }

    ret = qemu_rdma_block_for_wrid(rdma, RDMA_WRID_SEND_CONTROL);
    if (ret < 0) {
        fprintf(stderr, "rdma migration: send polling control error!\n");
    }

    return ret;
}

/*
 * Post a RECV work request in anticipation of some future receipt
 * of data on the control channel.
 */
static int qemu_rdma_post_recv_control(RDMAContext *rdma, int idx)
{
    struct ibv_recv_wr *bad_wr;
    struct ibv_sge sge = {
                            .addr = (uint64_t)(rdma->wr_data[idx].control),
                            .length = RDMA_CONTROL_MAX_BUFFER,
                            .lkey = rdma->wr_data[idx].control_mr->lkey,
                         };

    struct ibv_recv_wr recv_wr = {
                                    .wr_id = RDMA_WRID_RECV_CONTROL + idx,
                                    .sg_list = &sge,
                                    .num_sge = 1,
                                 };


    if (ibv_post_recv(rdma->qp, &recv_wr, &bad_wr)) {
        return -1;
    }

    return 0;
}

/*
 * Block and wait for a RECV control channel message to arrive.
 */
static int qemu_rdma_exchange_get_response(RDMAContext *rdma,
                RDMAControlHeader *head, int expecting, int idx)
{
    int ret = qemu_rdma_block_for_wrid(rdma, RDMA_WRID_RECV_CONTROL + idx);

    if (ret < 0) {
        fprintf(stderr, "rdma migration: recv polling control error!\n");
        return ret;
    }

    network_to_control((void *) rdma->wr_data[idx].control);
    memcpy(head, rdma->wr_data[idx].control, sizeof(RDMAControlHeader));

    DDDPRINTF("CONTROL: %s receiving...\n", control_desc[expecting]);

    if (expecting == RDMA_CONTROL_NONE) {
        DDDPRINTF("Surprise: got %s (%d)\n",
                  control_desc[head->type], head->type);
    } else if (head->type != expecting || head->type == RDMA_CONTROL_ERROR) {
        fprintf(stderr, "Was expecting a %s (%d) control message"
                ", but got: %s (%d), length: %d\n",
                control_desc[expecting], expecting,
                control_desc[head->type], head->type, head->len);
        return -EIO;
    }

    return 0;
}

/*
 * When a RECV work request has completed, the work request's
 * buffer is pointed at the header.
 *
 * This will advance the pointer to the data portion
 * of the control message of the work request's buffer that
 * was populated after the work request finished.
 */
static void qemu_rdma_move_header(RDMAContext *rdma, int idx,
                                  RDMAControlHeader *head)
{
    rdma->wr_data[idx].control_len = head->len;
    rdma->wr_data[idx].control_curr =
        rdma->wr_data[idx].control + sizeof(RDMAControlHeader);
}

/*
 * This is an 'atomic' high-level operation to deliver a single, unified
 * control-channel message.
 *
 * Additionally, if the user is expecting some kind of reply to this message,
 * they can request a 'resp' response message be filled in by posting an
 * additional work request on behalf of the user and waiting for an additional
 * completion.
 *
 * The extra (optional) response is used during registration to us from having
 * to perform an *additional* exchange of message just to provide a response by
 * instead piggy-backing on the acknowledgement.
 */
static int qemu_rdma_exchange_send(RDMAContext *rdma, RDMAControlHeader *head,
                                   uint8_t *data, RDMAControlHeader *resp,
                                   int *resp_idx,
                                   int (*callback)(RDMAContext *rdma))
{
    int ret = 0;

    /*
     * Wait until the dest is ready before attempting to deliver the message
     * by waiting for a READY message.
     */
    if (rdma->control_ready_expected) {
        RDMAControlHeader resp;
        ret = qemu_rdma_exchange_get_response(rdma,
                                    &resp, RDMA_CONTROL_READY, RDMA_WRID_READY);
        if (ret < 0) {
            return ret;
        }
    }

    /*
     * If the user is expecting a response, post a WR in anticipation of it.
     */
    if (resp) {
        ret = qemu_rdma_post_recv_control(rdma, RDMA_WRID_DATA);
        if (ret) {
            fprintf(stderr, "rdma migration: error posting"
                    " extra control recv for anticipated result!");
            return ret;
        }
    }

    /*
     * Post a WR to replace the one we just consumed for the READY message.
     */
    ret = qemu_rdma_post_recv_control(rdma, RDMA_WRID_READY);
    if (ret) {
        fprintf(stderr, "rdma migration: error posting first control recv!");
        return ret;
    }

    /*
     * Deliver the control message that was requested.
     */
    ret = qemu_rdma_post_send_control(rdma, data, head);

    if (ret < 0) {
        fprintf(stderr, "Failed to send control buffer!\n");
        return ret;
    }

    /*
     * If we're expecting a response, block and wait for it.
     */
    if (resp) {
        if (callback) {
            DDPRINTF("Issuing callback before receiving response...\n");
            ret = callback(rdma);
            if (ret < 0) {
                return ret;
            }
        }

        DDPRINTF("Waiting for response %s\n", control_desc[resp->type]);
        ret = qemu_rdma_exchange_get_response(rdma, resp,
                                              resp->type, RDMA_WRID_DATA);

        if (ret < 0) {
            return ret;
        }

        qemu_rdma_move_header(rdma, RDMA_WRID_DATA, resp);
        if (resp_idx) {
            *resp_idx = RDMA_WRID_DATA;
        }
        DDPRINTF("Response %s received.\n", control_desc[resp->type]);
    }

    rdma->control_ready_expected = 1;

    return 0;
}

/*
 * This is an 'atomic' high-level operation to receive a single, unified
 * control-channel message.
 */
static int qemu_rdma_exchange_recv(RDMAContext *rdma, RDMAControlHeader *head,
                                int expecting)
{
    RDMAControlHeader ready = {
                                .len = 0,
                                .type = RDMA_CONTROL_READY,
                                .repeat = 1,
                              };
    int ret;

    /*
     * Inform the source that we're ready to receive a message.
     */
    ret = qemu_rdma_post_send_control(rdma, NULL, &ready);

    if (ret < 0) {
        fprintf(stderr, "Failed to send control buffer!\n");
        return ret;
    }

    /*
     * Block and wait for the message.
     */
    ret = qemu_rdma_exchange_get_response(rdma, head,
                                          expecting, RDMA_WRID_READY);

    if (ret < 0) {
        return ret;
    }

    qemu_rdma_move_header(rdma, RDMA_WRID_READY, head);

    /*
     * Post a new RECV work request to replace the one we just consumed.
     */
    ret = qemu_rdma_post_recv_control(rdma, RDMA_WRID_READY);
    if (ret) {
        fprintf(stderr, "rdma migration: error posting second control recv!");
        return ret;
    }

    return 0;
}

/*
 * Write an actual chunk of memory using RDMA.
 *
 * If we're using dynamic registration on the dest-side, we have to
 * send a registration command first.
 */
static int qemu_rdma_write_one(QEMUFile *f, RDMAContext *rdma,
                               int current_index, uint64_t current_addr,
                               uint64_t length)
{
    struct ibv_sge sge;
    struct ibv_send_wr send_wr = { 0 };
    struct ibv_send_wr *bad_wr;
    int reg_result_idx, ret, count = 0;
    uint64_t chunk, chunks;
    uint8_t *chunk_start, *chunk_end;
    RDMALocalBlock *block = &(rdma->local_ram_blocks.block[current_index]);
    RDMARegister reg;
    RDMARegisterResult *reg_result;
    RDMAControlHeader resp = { .type = RDMA_CONTROL_REGISTER_RESULT };
    RDMAControlHeader head = { .len = sizeof(RDMARegister),
                               .type = RDMA_CONTROL_REGISTER_REQUEST,
                               .repeat = 1,
                             };

retry:
    sge.addr = (uint64_t)(block->local_host_addr +
                            (current_addr - block->offset));
    sge.length = length;

    chunk = ram_chunk_index(block->local_host_addr, (uint8_t *) sge.addr);
    chunk_start = ram_chunk_start(block, chunk);

    if (block->is_ram_block) {
        chunks = length / (1UL << RDMA_REG_CHUNK_SHIFT);

        if (chunks && ((length % (1UL << RDMA_REG_CHUNK_SHIFT)) == 0)) {
            chunks--;
        }
    } else {
        chunks = block->length / (1UL << RDMA_REG_CHUNK_SHIFT);

        if (chunks && ((block->length % (1UL << RDMA_REG_CHUNK_SHIFT)) == 0)) {
            chunks--;
        }
    }

    DDPRINTF("Writing %" PRIu64 " chunks, (%" PRIu64 " MB)\n",
        chunks + 1, (chunks + 1) * (1UL << RDMA_REG_CHUNK_SHIFT) / 1024 / 1024);

    chunk_end = ram_chunk_end(block, chunk + chunks);

    if (!rdma->pin_all) {
#ifdef RDMA_UNREGISTRATION_EXAMPLE
        qemu_rdma_unregister_waiting(rdma);
#endif
    }

    while (test_bit(chunk, block->transit_bitmap)) {
        (void)count;
        DDPRINTF("(%d) Not clobbering: block: %d chunk %" PRIu64
                " current %" PRIu64 " len %" PRIu64 " %d %d\n",
                count++, current_index, chunk,
                sge.addr, length, rdma->nb_sent, block->nb_chunks);

        ret = qemu_rdma_block_for_wrid(rdma, RDMA_WRID_RDMA_WRITE);

        if (ret < 0) {
            fprintf(stderr, "Failed to Wait for previous write to complete "
                    "block %d chunk %" PRIu64
                    " current %" PRIu64 " len %" PRIu64 " %d\n",
                    current_index, chunk, sge.addr, length, rdma->nb_sent);
            return ret;
        }
    }

    if (!rdma->pin_all || !block->is_ram_block) {
        if (!block->remote_keys[chunk]) {
            /*
             * This chunk has not yet been registered, so first check to see
             * if the entire chunk is zero. If so, tell the other size to
             * memset() + madvise() the entire chunk without RDMA.
             */

            if (can_use_buffer_find_nonzero_offset((void *)sge.addr, length)
                   && buffer_find_nonzero_offset((void *)sge.addr,
                                                    length) == length) {
                RDMACompress comp = {
                                        .offset = current_addr,
                                        .value = 0,
                                        .block_idx = current_index,
                                        .length = length,
                                    };

                head.len = sizeof(comp);
                head.type = RDMA_CONTROL_COMPRESS;

                DDPRINTF("Entire chunk is zero, sending compress: %"
                    PRIu64 " for %d "
                    "bytes, index: %d, offset: %" PRId64 "...\n",
                    chunk, sge.length, current_index, current_addr);

                compress_to_network(&comp);
                ret = qemu_rdma_exchange_send(rdma, &head,
                                (uint8_t *) &comp, NULL, NULL, NULL);

                if (ret < 0) {
                    return -EIO;
                }

                acct_update_position(f, sge.length, true);

                return 1;
            }

            /*
             * Otherwise, tell other side to register.
             */
            reg.current_index = current_index;
            if (block->is_ram_block) {
                reg.key.current_addr = current_addr;
            } else {
                reg.key.chunk = chunk;
            }
            reg.chunks = chunks;

            DDPRINTF("Sending registration request chunk %" PRIu64 " for %d "
                    "bytes, index: %d, offset: %" PRId64 "...\n",
                    chunk, sge.length, current_index, current_addr);

            register_to_network(&reg);
            ret = qemu_rdma_exchange_send(rdma, &head, (uint8_t *) &reg,
                                    &resp, &reg_result_idx, NULL);
            if (ret < 0) {
                return ret;
            }

            /* try to overlap this single registration with the one we sent. */
            if (qemu_rdma_register_and_get_keys(rdma, block,
                                                (uint8_t *) sge.addr,
                                                &sge.lkey, NULL, chunk,
                                                chunk_start, chunk_end)) {
                fprintf(stderr, "cannot get lkey!\n");
                return -EINVAL;
            }

            reg_result = (RDMARegisterResult *)
                    rdma->wr_data[reg_result_idx].control_curr;

            network_to_result(reg_result);

            DDPRINTF("Received registration result:"
                    " my key: %x their key %x, chunk %" PRIu64 "\n",
                    block->remote_keys[chunk], reg_result->rkey, chunk);

            block->remote_keys[chunk] = reg_result->rkey;
            block->remote_host_addr = reg_result->host_addr;
        } else {
            /* already registered before */
            if (qemu_rdma_register_and_get_keys(rdma, block,
                                                (uint8_t *)sge.addr,
                                                &sge.lkey, NULL, chunk,
                                                chunk_start, chunk_end)) {
                fprintf(stderr, "cannot get lkey!\n");
                return -EINVAL;
            }
        }

        send_wr.wr.rdma.rkey = block->remote_keys[chunk];
    } else {
        send_wr.wr.rdma.rkey = block->remote_rkey;

        if (qemu_rdma_register_and_get_keys(rdma, block, (uint8_t *)sge.addr,
                                                     &sge.lkey, NULL, chunk,
                                                     chunk_start, chunk_end)) {
            fprintf(stderr, "cannot get lkey!\n");
            return -EINVAL;
        }
    }

    /*
     * Encode the ram block index and chunk within this wrid.
     * We will use this information at the time of completion
     * to figure out which bitmap to check against and then which
     * chunk in the bitmap to look for.
     */
    send_wr.wr_id = qemu_rdma_make_wrid(RDMA_WRID_RDMA_WRITE,
                                        current_index, chunk);

    send_wr.opcode = IBV_WR_RDMA_WRITE;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.wr.rdma.remote_addr = block->remote_host_addr +
                                (current_addr - block->offset);

    DDDPRINTF("Posting chunk: %" PRIu64 ", addr: %lx"
              " remote: %lx, bytes %" PRIu32 "\n",
              chunk, sge.addr, send_wr.wr.rdma.remote_addr,
              sge.length);

    /*
     * ibv_post_send() does not return negative error numbers,
     * per the specification they are positive - no idea why.
     */
    ret = ibv_post_send(rdma->qp, &send_wr, &bad_wr);

    if (ret == ENOMEM) {
        DDPRINTF("send queue is full. wait a little....\n");
        ret = qemu_rdma_block_for_wrid(rdma, RDMA_WRID_RDMA_WRITE);
        if (ret < 0) {
            fprintf(stderr, "rdma migration: failed to make "
                            "room in full send queue! %d\n", ret);
            return ret;
        }

        goto retry;

    } else if (ret > 0) {
        perror("rdma migration: post rdma write failed");
        return -ret;
    }

    set_bit(chunk, block->transit_bitmap);
    acct_update_position(f, sge.length, false);
    rdma->total_writes++;

    return 0;
}

/*
 * Push out any unwritten RDMA operations.
 *
 * We support sending out multiple chunks at the same time.
 * Not all of them need to get signaled in the completion queue.
 */
static int qemu_rdma_write_flush(QEMUFile *f, RDMAContext *rdma)
{
    int ret;

    if (!rdma->current_length) {
        return 0;
    }

    ret = qemu_rdma_write_one(f, rdma,
            rdma->current_index, rdma->current_addr, rdma->current_length);

    if (ret < 0) {
        return ret;
    }

    if (ret == 0) {
        rdma->nb_sent++;
        DDDPRINTF("sent total: %d\n", rdma->nb_sent);
    }

    rdma->current_length = 0;
    rdma->current_addr = 0;

    return 0;
}

static inline int qemu_rdma_buffer_mergable(RDMAContext *rdma,
                    uint64_t offset, uint64_t len)
{
    RDMALocalBlock *block =
        &(rdma->local_ram_blocks.block[rdma->current_index]);
    uint8_t *host_addr = block->local_host_addr + (offset - block->offset);
    uint8_t *chunk_end = ram_chunk_end(block, rdma->current_chunk);

    if (rdma->current_length == 0) {
        return 0;
    }

    /*
     * Only merge into chunk sequentially.
     */
    if (offset != (rdma->current_addr + rdma->current_length)) {
        return 0;
    }

    if (rdma->current_index < 0) {
        return 0;
    }

    if (offset < block->offset) {
        return 0;
    }

    if ((offset + len) > (block->offset + block->length)) {
        return 0;
    }

    if (rdma->current_chunk < 0) {
        return 0;
    }

    if ((host_addr + len) > chunk_end) {
        return 0;
    }

    return 1;
}

/*
 * We're not actually writing here, but doing three things:
 *
 * 1. Identify the chunk the buffer belongs to.
 * 2. If the chunk is full or the buffer doesn't belong to the current
 *    chunk, then start a new chunk and flush() the old chunk.
 * 3. To keep the hardware busy, we also group chunks into batches
 *    and only require that a batch gets acknowledged in the completion
 *    qeueue instead of each individual chunk.
 */
static int qemu_rdma_write(QEMUFile *f, RDMAContext *rdma,
                           uint64_t block_offset, uint64_t offset,
                           uint64_t len)
{
    uint64_t current_addr = block_offset + offset;
    uint64_t index = rdma->current_index;
    uint64_t chunk = rdma->current_chunk;
    int ret;

    /* If we cannot merge it, we flush the current buffer first. */
    if (!qemu_rdma_buffer_mergable(rdma, current_addr, len)) {
        ret = qemu_rdma_write_flush(f, rdma);
        if (ret) {
            return ret;
        }
        rdma->current_length = 0;
        rdma->current_addr = current_addr;

        ret = qemu_rdma_search_ram_block(rdma, block_offset,
                                         offset, len, &index, &chunk);
        if (ret) {
            fprintf(stderr, "ram block search failed\n");
            return ret;
        }
        rdma->current_index = index;
        rdma->current_chunk = chunk;
    }

    /* merge it */
    rdma->current_length += len;

    /* flush it if buffer is too large */
    if (rdma->current_length >= RDMA_MERGE_MAX) {
        return qemu_rdma_write_flush(f, rdma);
    }

    return 0;
}

static void qemu_rdma_cleanup(RDMAContext *rdma)
{
    struct rdma_cm_event *cm_event;
    int ret, idx;

    if (rdma->cm_id) {
        if (rdma->error_state) {
            RDMAControlHeader head = { .len = 0,
                                       .type = RDMA_CONTROL_ERROR,
                                       .repeat = 1,
                                     };
            fprintf(stderr, "Early error. Sending error.\n");
            qemu_rdma_post_send_control(rdma, NULL, &head);
        }

        ret = rdma_disconnect(rdma->cm_id);
        if (!ret) {
            DDPRINTF("waiting for disconnect\n");
            ret = rdma_get_cm_event(rdma->channel, &cm_event);
            if (!ret) {
                rdma_ack_cm_event(cm_event);
            }
        }
        DDPRINTF("Disconnected.\n");
        rdma->cm_id = NULL;
    }

    g_free(rdma->block);
    rdma->block = NULL;

    for (idx = 0; idx <= RDMA_WRID_MAX; idx++) {
        if (rdma->wr_data[idx].control_mr) {
            rdma->total_registrations--;
            ibv_dereg_mr(rdma->wr_data[idx].control_mr);
        }
        rdma->wr_data[idx].control_mr = NULL;
    }

    if (rdma->local_ram_blocks.block) {
        while (rdma->local_ram_blocks.nb_blocks) {
            __qemu_rdma_delete_block(rdma,
                    rdma->local_ram_blocks.block->offset);
        }
    }

    if (rdma->qp) {
        ibv_destroy_qp(rdma->qp);
        rdma->qp = NULL;
    }
    if (rdma->cq) {
        ibv_destroy_cq(rdma->cq);
        rdma->cq = NULL;
    }
    if (rdma->comp_channel) {
        ibv_destroy_comp_channel(rdma->comp_channel);
        rdma->comp_channel = NULL;
    }
    if (rdma->pd) {
        ibv_dealloc_pd(rdma->pd);
        rdma->pd = NULL;
    }
    if (rdma->listen_id) {
        rdma_destroy_id(rdma->listen_id);
        rdma->listen_id = NULL;
    }
    if (rdma->cm_id) {
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = NULL;
    }
    if (rdma->channel) {
        rdma_destroy_event_channel(rdma->channel);
        rdma->channel = NULL;
    }
}


static int qemu_rdma_source_init(RDMAContext *rdma, Error **errp, bool pin_all)
{
    int ret, idx;
    Error *local_err = NULL, **temp = &local_err;

    /*
     * Will be validated against destination's actual capabilities
     * after the connect() completes.
     */
    rdma->pin_all = pin_all;

    ret = qemu_rdma_resolve_host(rdma, temp);
    if (ret) {
        goto err_rdma_source_init;
    }

    ret = qemu_rdma_alloc_pd_cq(rdma);
    if (ret) {
        ERROR(temp, "rdma migration: error allocating pd and cq! Your mlock()"
                    " limits may be too low. Please check $ ulimit -a # and "
                    "search for 'ulimit -l' in the output\n");
        goto err_rdma_source_init;
    }

    ret = qemu_rdma_alloc_qp(rdma);
    if (ret) {
        ERROR(temp, "rdma migration: error allocating qp!\n");
        goto err_rdma_source_init;
    }

    ret = qemu_rdma_init_ram_blocks(rdma);
    if (ret) {
        ERROR(temp, "rdma migration: error initializing ram blocks!\n");
        goto err_rdma_source_init;
    }

    for (idx = 0; idx <= RDMA_WRID_MAX; idx++) {
        ret = qemu_rdma_reg_control(rdma, idx);
        if (ret) {
            ERROR(temp, "rdma migration: error registering %d control!\n",
                                                            idx);
            goto err_rdma_source_init;
        }
    }

    return 0;

err_rdma_source_init:
    error_propagate(errp, local_err);
    qemu_rdma_cleanup(rdma);
    return -1;
}

static int qemu_rdma_connect(RDMAContext *rdma, Error **errp)
{
    RDMACapabilities cap = {
                                .version = RDMA_CONTROL_VERSION_CURRENT,
                                .flags = 0,
                           };
    struct rdma_conn_param conn_param = { .initiator_depth = 2,
                                          .retry_count = 5,
                                          .private_data = &cap,
                                          .private_data_len = sizeof(cap),
                                        };
    struct rdma_cm_event *cm_event;
    int ret;

    /*
     * Only negotiate the capability with destination if the user
     * on the source first requested the capability.
     */
    if (rdma->pin_all) {
        DPRINTF("Server pin-all memory requested.\n");
        cap.flags |= RDMA_CAPABILITY_PIN_ALL;
    }

    caps_to_network(&cap);

    ret = rdma_connect(rdma->cm_id, &conn_param);
    if (ret) {
        perror("rdma_connect");
        ERROR(errp, "connecting to destination!\n");
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = NULL;
        goto err_rdma_source_connect;
    }

    ret = rdma_get_cm_event(rdma->channel, &cm_event);
    if (ret) {
        perror("rdma_get_cm_event after rdma_connect");
        ERROR(errp, "connecting to destination!\n");
        rdma_ack_cm_event(cm_event);
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = NULL;
        goto err_rdma_source_connect;
    }

    if (cm_event->event != RDMA_CM_EVENT_ESTABLISHED) {
        perror("rdma_get_cm_event != EVENT_ESTABLISHED after rdma_connect");
        ERROR(errp, "connecting to destination!\n");
        rdma_ack_cm_event(cm_event);
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = NULL;
        goto err_rdma_source_connect;
    }

    memcpy(&cap, cm_event->param.conn.private_data, sizeof(cap));
    network_to_caps(&cap);

    /*
     * Verify that the *requested* capabilities are supported by the destination
     * and disable them otherwise.
     */
    if (rdma->pin_all && !(cap.flags & RDMA_CAPABILITY_PIN_ALL)) {
        ERROR(errp, "Server cannot support pinning all memory. "
                        "Will register memory dynamically.\n");
        rdma->pin_all = false;
    }

    DPRINTF("Pin all memory: %s\n", rdma->pin_all ? "enabled" : "disabled");

    rdma_ack_cm_event(cm_event);

    ret = qemu_rdma_post_recv_control(rdma, 0);
    if (ret) {
        ERROR(errp, "posting second control recv!\n");
        goto err_rdma_source_connect;
    }

    rdma->control_ready_expected = 1;
    rdma->nb_sent = 0;
    return 0;

err_rdma_source_connect:
    qemu_rdma_cleanup(rdma);
    return -1;
}

static int qemu_rdma_dest_init(RDMAContext *rdma, Error **errp)
{
    int ret = -EINVAL, idx;
    struct sockaddr_in sin;
    struct rdma_cm_id *listen_id;
    char ip[40] = "unknown";

    for (idx = 0; idx <= RDMA_WRID_MAX; idx++) {
        rdma->wr_data[idx].control_len = 0;
        rdma->wr_data[idx].control_curr = NULL;
    }

    if (rdma->host == NULL) {
        ERROR(errp, "RDMA host is not set!\n");
        rdma->error_state = -EINVAL;
        return -1;
    }
    /* create CM channel */
    rdma->channel = rdma_create_event_channel();
    if (!rdma->channel) {
        ERROR(errp, "could not create rdma event channel\n");
        rdma->error_state = -EINVAL;
        return -1;
    }

    /* create CM id */
    ret = rdma_create_id(rdma->channel, &listen_id, NULL, RDMA_PS_TCP);
    if (ret) {
        ERROR(errp, "could not create cm_id!\n");
        goto err_dest_init_create_listen_id;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(rdma->port);

    if (rdma->host && strcmp("", rdma->host)) {
        struct hostent *dest_addr;
        dest_addr = gethostbyname(rdma->host);
        if (!dest_addr) {
            ERROR(errp, "migration could not gethostbyname!\n");
            ret = -EINVAL;
            goto err_dest_init_bind_addr;
        }
        memcpy(&sin.sin_addr.s_addr, dest_addr->h_addr,
                dest_addr->h_length);
        inet_ntop(AF_INET, dest_addr->h_addr, ip, sizeof ip);
    } else {
        sin.sin_addr.s_addr = INADDR_ANY;
    }

    DPRINTF("%s => %s\n", rdma->host, ip);

    ret = rdma_bind_addr(listen_id, (struct sockaddr *)&sin);
    if (ret) {
        ERROR(errp, "Error: could not rdma_bind_addr!\n");
        goto err_dest_init_bind_addr;
    }

    rdma->listen_id = listen_id;
    qemu_rdma_dump_gid("dest_init", listen_id);
    return 0;

err_dest_init_bind_addr:
    rdma_destroy_id(listen_id);
err_dest_init_create_listen_id:
    rdma_destroy_event_channel(rdma->channel);
    rdma->channel = NULL;
    rdma->error_state = ret;
    return ret;

}

static void *qemu_rdma_data_init(const char *host_port, Error **errp)
{
    RDMAContext *rdma = NULL;
    InetSocketAddress *addr;

    if (host_port) {
        rdma = g_malloc0(sizeof(RDMAContext));
        memset(rdma, 0, sizeof(RDMAContext));
        rdma->current_index = -1;
        rdma->current_chunk = -1;

        addr = inet_parse(host_port, NULL);
        if (addr != NULL) {
            rdma->port = atoi(addr->port);
            rdma->host = g_strdup(addr->host);
        } else {
            ERROR(errp, "bad RDMA migration address '%s'", host_port);
            g_free(rdma);
            return NULL;
        }
    }

    return rdma;
}

/*
 * QEMUFile interface to the control channel.
 * SEND messages for control only.
 * pc.ram is handled with regular RDMA messages.
 */
static int qemu_rdma_put_buffer(void *opaque, const uint8_t *buf,
                                int64_t pos, int size)
{
    QEMUFileRDMA *r = opaque;
    QEMUFile *f = r->file;
    RDMAContext *rdma = r->rdma;
    size_t remaining = size;
    uint8_t * data = (void *) buf;
    int ret;

    CHECK_ERROR_STATE();

    /*
     * Push out any writes that
     * we're queued up for pc.ram.
     */
    ret = qemu_rdma_write_flush(f, rdma);
    if (ret < 0) {
        rdma->error_state = ret;
        return ret;
    }

    while (remaining) {
        RDMAControlHeader head;

        r->len = MIN(remaining, RDMA_SEND_INCREMENT);
        remaining -= r->len;

        head.len = r->len;
        head.type = RDMA_CONTROL_QEMU_FILE;

        ret = qemu_rdma_exchange_send(rdma, &head, data, NULL, NULL, NULL);

        if (ret < 0) {
            rdma->error_state = ret;
            return ret;
        }

        data += r->len;
    }

    return size;
}

static size_t qemu_rdma_fill(RDMAContext *rdma, uint8_t *buf,
                             int size, int idx)
{
    size_t len = 0;

    if (rdma->wr_data[idx].control_len) {
        DDDPRINTF("RDMA %" PRId64 " of %d bytes already in buffer\n",
                    rdma->wr_data[idx].control_len, size);

        len = MIN(size, rdma->wr_data[idx].control_len);
        memcpy(buf, rdma->wr_data[idx].control_curr, len);
        rdma->wr_data[idx].control_curr += len;
        rdma->wr_data[idx].control_len -= len;
    }

    return len;
}

/*
 * QEMUFile interface to the control channel.
 * RDMA links don't use bytestreams, so we have to
 * return bytes to QEMUFile opportunistically.
 */
static int qemu_rdma_get_buffer(void *opaque, uint8_t *buf,
                                int64_t pos, int size)
{
    QEMUFileRDMA *r = opaque;
    RDMAContext *rdma = r->rdma;
    RDMAControlHeader head;
    int ret = 0;

    CHECK_ERROR_STATE();

    /*
     * First, we hold on to the last SEND message we
     * were given and dish out the bytes until we run
     * out of bytes.
     */
    r->len = qemu_rdma_fill(r->rdma, buf, size, 0);
    if (r->len) {
        return r->len;
    }

    /*
     * Once we run out, we block and wait for another
     * SEND message to arrive.
     */
    ret = qemu_rdma_exchange_recv(rdma, &head, RDMA_CONTROL_QEMU_FILE);

    if (ret < 0) {
        rdma->error_state = ret;
        return ret;
    }

    /*
     * SEND was received with new bytes, now try again.
     */
    return qemu_rdma_fill(r->rdma, buf, size, 0);
}

/*
 * Block until all the outstanding chunks have been delivered by the hardware.
 */
static int qemu_rdma_drain_cq(QEMUFile *f, RDMAContext *rdma)
{
    int ret;

    if (qemu_rdma_write_flush(f, rdma) < 0) {
        return -EIO;
    }

    while (rdma->nb_sent) {
        ret = qemu_rdma_block_for_wrid(rdma, RDMA_WRID_RDMA_WRITE);
        if (ret < 0) {
            fprintf(stderr, "rdma migration: complete polling error!\n");
            return -EIO;
        }
    }

    qemu_rdma_unregister_waiting(rdma);

    return 0;
}

static int qemu_rdma_close(void *opaque)
{
    DPRINTF("Shutting down connection.\n");
    QEMUFileRDMA *r = opaque;
    if (r->rdma) {
        qemu_rdma_cleanup(r->rdma);
        g_free(r->rdma);
    }
    g_free(r);
    return 0;
}

/*
 * Parameters:
 *    @offset == 0 :
 *        This means that 'block_offset' is a full virtual address that does not
 *        belong to a RAMBlock of the virtual machine and instead
 *        represents a private malloc'd memory area that the caller wishes to
 *        transfer.
 *
 *    @offset != 0 :
 *        Offset is an offset to be added to block_offset and used
 *        to also lookup the corresponding RAMBlock.
 *
 *    @size > 0 :
 *        Initiate an transfer this size.
 *
 *    @size == 0 :
 *        A 'hint' or 'advice' that means that we wish to speculatively
 *        and asynchronously unregister this memory. In this case, there is no
 *        guarantee that the unregister will actually happen, for example,
 *        if the memory is being actively transmitted. Additionally, the memory
 *        may be re-registered at any future time if a write within the same
 *        chunk was requested again, even if you attempted to unregister it
 *        here.
 *
 *    @size < 0 : TODO, not yet supported
 *        Unregister the memory NOW. This means that the caller does not
 *        expect there to be any future RDMA transfers and we just want to clean
 *        things up. This is used in case the upper layer owns the memory and
 *        cannot wait for qemu_fclose() to occur.
 *
 *    @bytes_sent : User-specificed pointer to indicate how many bytes were
 *                  sent. Usually, this will not be more than a few bytes of
 *                  the protocol because most transfers are sent asynchronously.
 */
static size_t qemu_rdma_save_page(QEMUFile *f, void *opaque,
                                  ram_addr_t block_offset, ram_addr_t offset,
                                  size_t size, int *bytes_sent)
{
    QEMUFileRDMA *rfile = opaque;
    RDMAContext *rdma = rfile->rdma;
    int ret;

    CHECK_ERROR_STATE();

    qemu_fflush(f);

    if (size > 0) {
        /*
         * Add this page to the current 'chunk'. If the chunk
         * is full, or the page doen't belong to the current chunk,
         * an actual RDMA write will occur and a new chunk will be formed.
         */
        ret = qemu_rdma_write(f, rdma, block_offset, offset, size);
        if (ret < 0) {
            fprintf(stderr, "rdma migration: write error! %d\n", ret);
            goto err;
        }

        /*
         * We always return 1 bytes because the RDMA
         * protocol is completely asynchronous. We do not yet know
         * whether an  identified chunk is zero or not because we're
         * waiting for other pages to potentially be merged with
         * the current chunk. So, we have to call qemu_update_position()
         * later on when the actual write occurs.
         */
        if (bytes_sent) {
            *bytes_sent = 1;
        }
    } else {
        uint64_t index, chunk;

        /* TODO: Change QEMUFileOps prototype to be signed: size_t => long
        if (size < 0) {
            ret = qemu_rdma_drain_cq(f, rdma);
            if (ret < 0) {
                fprintf(stderr, "rdma: failed to synchronously drain"
                                " completion queue before unregistration.\n");
                goto err;
            }
        }
        */

        ret = qemu_rdma_search_ram_block(rdma, block_offset,
                                         offset, size, &index, &chunk);

        if (ret) {
            fprintf(stderr, "ram block search failed\n");
            goto err;
        }

        qemu_rdma_signal_unregister(rdma, index, chunk, 0);

        /*
         * TODO: Synchronous, guaranteed unregistration (should not occur during
         * fast-path). Otherwise, unregisters will process on the next call to
         * qemu_rdma_drain_cq()
        if (size < 0) {
            qemu_rdma_unregister_waiting(rdma);
        }
        */
    }

    /*
     * Drain the Completion Queue if possible, but do not block,
     * just poll.
     *
     * If nothing to poll, the end of the iteration will do this
     * again to make sure we don't overflow the request queue.
     */
    while (1) {
        uint64_t wr_id, wr_id_in;
        int ret = qemu_rdma_poll(rdma, &wr_id_in);
        if (ret < 0) {
            fprintf(stderr, "rdma migration: polling error! %d\n", ret);
            goto err;
        }

        wr_id = wr_id_in & RDMA_WRID_TYPE_MASK;

        if (wr_id == RDMA_WRID_NONE) {
            break;
        }
    }

    return RAM_SAVE_CONTROL_DELAYED;
err:
    rdma->error_state = ret;
    return ret;
}

static int qemu_rdma_accept(RDMAContext *rdma)
{
    RDMACapabilities cap;
    struct rdma_conn_param conn_param = {
                                            .responder_resources = 2,
                                            .private_data = &cap,
                                            .private_data_len = sizeof(cap),
                                         };
    struct rdma_cm_event *cm_event;
    struct ibv_context *verbs;
    int ret = -EINVAL;
    int idx;

    ret = rdma_get_cm_event(rdma->channel, &cm_event);
    if (ret) {
        goto err_rdma_dest_wait;
    }

    if (cm_event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
        rdma_ack_cm_event(cm_event);
        goto err_rdma_dest_wait;
    }

    memcpy(&cap, cm_event->param.conn.private_data, sizeof(cap));

    network_to_caps(&cap);

    if (cap.version < 1 || cap.version > RDMA_CONTROL_VERSION_CURRENT) {
            fprintf(stderr, "Unknown source RDMA version: %d, bailing...\n",
                            cap.version);
            rdma_ack_cm_event(cm_event);
            goto err_rdma_dest_wait;
    }

    /*
     * Respond with only the capabilities this version of QEMU knows about.
     */
    cap.flags &= known_capabilities;

    /*
     * Enable the ones that we do know about.
     * Add other checks here as new ones are introduced.
     */
    if (cap.flags & RDMA_CAPABILITY_PIN_ALL) {
        rdma->pin_all = true;
    }

    rdma->cm_id = cm_event->id;
    verbs = cm_event->id->verbs;

    rdma_ack_cm_event(cm_event);

    DPRINTF("Memory pin all: %s\n", rdma->pin_all ? "enabled" : "disabled");

    caps_to_network(&cap);

    DPRINTF("verbs context after listen: %p\n", verbs);

    if (!rdma->verbs) {
        rdma->verbs = verbs;
    } else if (rdma->verbs != verbs) {
            fprintf(stderr, "ibv context not matching %p, %p!\n",
                    rdma->verbs, verbs);
            goto err_rdma_dest_wait;
    }

    qemu_rdma_dump_id("dest_init", verbs);

    ret = qemu_rdma_alloc_pd_cq(rdma);
    if (ret) {
        fprintf(stderr, "rdma migration: error allocating pd and cq!\n");
        goto err_rdma_dest_wait;
    }

    ret = qemu_rdma_alloc_qp(rdma);
    if (ret) {
        fprintf(stderr, "rdma migration: error allocating qp!\n");
        goto err_rdma_dest_wait;
    }

    ret = qemu_rdma_init_ram_blocks(rdma);
    if (ret) {
        fprintf(stderr, "rdma migration: error initializing ram blocks!\n");
        goto err_rdma_dest_wait;
    }

    for (idx = 0; idx <= RDMA_WRID_MAX; idx++) {
        ret = qemu_rdma_reg_control(rdma, idx);
        if (ret) {
            fprintf(stderr, "rdma: error registering %d control!\n", idx);
            goto err_rdma_dest_wait;
        }
    }

    qemu_set_fd_handler2(rdma->channel->fd, NULL, NULL, NULL, NULL);

    ret = rdma_accept(rdma->cm_id, &conn_param);
    if (ret) {
        fprintf(stderr, "rdma_accept returns %d!\n", ret);
        goto err_rdma_dest_wait;
    }

    ret = rdma_get_cm_event(rdma->channel, &cm_event);
    if (ret) {
        fprintf(stderr, "rdma_accept get_cm_event failed %d!\n", ret);
        goto err_rdma_dest_wait;
    }

    if (cm_event->event != RDMA_CM_EVENT_ESTABLISHED) {
        fprintf(stderr, "rdma_accept not event established!\n");
        rdma_ack_cm_event(cm_event);
        goto err_rdma_dest_wait;
    }

    rdma_ack_cm_event(cm_event);

    ret = qemu_rdma_post_recv_control(rdma, 0);
    if (ret) {
        fprintf(stderr, "rdma migration: error posting second control recv!\n");
        goto err_rdma_dest_wait;
    }

    qemu_rdma_dump_gid("dest_connect", rdma->cm_id);

    return 0;

err_rdma_dest_wait:
    rdma->error_state = ret;
    qemu_rdma_cleanup(rdma);
    return ret;
}

/*
 * During each iteration of the migration, we listen for instructions
 * by the source VM to perform dynamic page registrations before they
 * can perform RDMA operations.
 *
 * We respond with the 'rkey'.
 *
 * Keep doing this until the source tells us to stop.
 */
static int qemu_rdma_registration_handle(QEMUFile *f, void *opaque,
                                         uint64_t flags)
{
    RDMAControlHeader reg_resp = { .len = sizeof(RDMARegisterResult),
                               .type = RDMA_CONTROL_REGISTER_RESULT,
                               .repeat = 0,
                             };
    RDMAControlHeader unreg_resp = { .len = 0,
                               .type = RDMA_CONTROL_UNREGISTER_FINISHED,
                               .repeat = 0,
                             };
    RDMAControlHeader blocks = { .type = RDMA_CONTROL_RAM_BLOCKS_RESULT,
                                 .repeat = 1 };
    QEMUFileRDMA *rfile = opaque;
    RDMAContext *rdma = rfile->rdma;
    RDMALocalBlocks *local = &rdma->local_ram_blocks;
    RDMAControlHeader head;
    RDMARegister *reg, *registers;
    RDMACompress *comp;
    RDMARegisterResult *reg_result;
    static RDMARegisterResult results[RDMA_CONTROL_MAX_COMMANDS_PER_MESSAGE];
    RDMALocalBlock *block;
    void *host_addr;
    int ret = 0;
    int idx = 0;
    int count = 0;
    int i = 0;

    CHECK_ERROR_STATE();

    do {
        DDDPRINTF("Waiting for next request %" PRIu64 "...\n", flags);

        ret = qemu_rdma_exchange_recv(rdma, &head, RDMA_CONTROL_NONE);

        if (ret < 0) {
            break;
        }

        if (head.repeat > RDMA_CONTROL_MAX_COMMANDS_PER_MESSAGE) {
            fprintf(stderr, "rdma: Too many requests in this message (%d)."
                            "Bailing.\n", head.repeat);
            ret = -EIO;
            break;
        }

        switch (head.type) {
        case RDMA_CONTROL_COMPRESS:
            comp = (RDMACompress *) rdma->wr_data[idx].control_curr;
            network_to_compress(comp);

            DDPRINTF("Zapping zero chunk: %" PRId64
                    " bytes, index %d, offset %" PRId64 "\n",
                    comp->length, comp->block_idx, comp->offset);
            block = &(rdma->local_ram_blocks.block[comp->block_idx]);

            host_addr = block->local_host_addr +
                            (comp->offset - block->offset);

            ram_handle_compressed(host_addr, comp->value, comp->length);
            break;

        case RDMA_CONTROL_REGISTER_FINISHED:
            DDDPRINTF("Current registrations complete.\n");
            goto out;

        case RDMA_CONTROL_RAM_BLOCKS_REQUEST:
            DPRINTF("Initial setup info requested.\n");

            if (rdma->pin_all) {
                ret = qemu_rdma_reg_whole_ram_blocks(rdma);
                if (ret) {
                    fprintf(stderr, "rdma migration: error dest "
                                    "registering ram blocks!\n");
                    goto out;
                }
            }

            /*
             * Dest uses this to prepare to transmit the RAMBlock descriptions
             * to the source VM after connection setup.
             * Both sides use the "remote" structure to communicate and update
             * their "local" descriptions with what was sent.
             */
            for (i = 0; i < local->nb_blocks; i++) {
                rdma->block[i].remote_host_addr =
                    (uint64_t)(local->block[i].local_host_addr);

                if (rdma->pin_all) {
                    rdma->block[i].remote_rkey = local->block[i].mr->rkey;
                }

                rdma->block[i].offset = local->block[i].offset;
                rdma->block[i].length = local->block[i].length;

                remote_block_to_network(&rdma->block[i]);
            }

            blocks.len = rdma->local_ram_blocks.nb_blocks
                                                * sizeof(RDMARemoteBlock);


            ret = qemu_rdma_post_send_control(rdma,
                                        (uint8_t *) rdma->block, &blocks);

            if (ret < 0) {
                fprintf(stderr, "rdma migration: error sending remote info!\n");
                goto out;
            }

            break;
        case RDMA_CONTROL_REGISTER_REQUEST:
            DDPRINTF("There are %d registration requests\n", head.repeat);

            reg_resp.repeat = head.repeat;
            registers = (RDMARegister *) rdma->wr_data[idx].control_curr;

            for (count = 0; count < head.repeat; count++) {
                uint64_t chunk;
                uint8_t *chunk_start, *chunk_end;

                reg = &registers[count];
                network_to_register(reg);

                reg_result = &results[count];

                DDPRINTF("Registration request (%d): index %d, current_addr %"
                         PRIu64 " chunks: %" PRIu64 "\n", count,
                         reg->current_index, reg->key.current_addr, reg->chunks);

                block = &(rdma->local_ram_blocks.block[reg->current_index]);
                if (block->is_ram_block) {
                    host_addr = (block->local_host_addr +
                                (reg->key.current_addr - block->offset));
                    chunk = ram_chunk_index(block->local_host_addr,
                                            (uint8_t *) host_addr);
                } else {
                    chunk = reg->key.chunk;
                    host_addr = block->local_host_addr +
                        (reg->key.chunk * (1UL << RDMA_REG_CHUNK_SHIFT));
                }
                chunk_start = ram_chunk_start(block, chunk);
                chunk_end = ram_chunk_end(block, chunk + reg->chunks);
                if (qemu_rdma_register_and_get_keys(rdma, block,
                            (uint8_t *)host_addr, NULL, &reg_result->rkey,
                            chunk, chunk_start, chunk_end)) {
                    fprintf(stderr, "cannot get rkey!\n");
                    ret = -EINVAL;
                    goto out;
                }

                reg_result->host_addr = (uint64_t) block->local_host_addr;

                DDPRINTF("Registered rkey for this request: %x\n",
                                reg_result->rkey);

                result_to_network(reg_result);
            }

            ret = qemu_rdma_post_send_control(rdma,
                            (uint8_t *) results, &reg_resp);

            if (ret < 0) {
                fprintf(stderr, "Failed to send control buffer!\n");
                goto out;
            }
            break;
        case RDMA_CONTROL_UNREGISTER_REQUEST:
            DDPRINTF("There are %d unregistration requests\n", head.repeat);
            unreg_resp.repeat = head.repeat;
            registers = (RDMARegister *) rdma->wr_data[idx].control_curr;

            for (count = 0; count < head.repeat; count++) {
                reg = &registers[count];
                network_to_register(reg);

                DDPRINTF("Unregistration request (%d): "
                         " index %d, chunk %" PRIu64 "\n",
                         count, reg->current_index, reg->key.chunk);

                block = &(rdma->local_ram_blocks.block[reg->current_index]);

                ret = ibv_dereg_mr(block->pmr[reg->key.chunk]);
                block->pmr[reg->key.chunk] = NULL;

                if (ret != 0) {
                    perror("rdma unregistration chunk failed");
                    ret = -ret;
                    goto out;
                }

                rdma->total_registrations--;

                DDPRINTF("Unregistered chunk %" PRIu64 " successfully.\n",
                            reg->key.chunk);
            }

            ret = qemu_rdma_post_send_control(rdma, NULL, &unreg_resp);

            if (ret < 0) {
                fprintf(stderr, "Failed to send control buffer!\n");
                goto out;
            }
            break;
        case RDMA_CONTROL_REGISTER_RESULT:
            fprintf(stderr, "Invalid RESULT message at dest.\n");
            ret = -EIO;
            goto out;
        default:
            fprintf(stderr, "Unknown control message %s\n",
                                control_desc[head.type]);
            ret = -EIO;
            goto out;
        }
    } while (1);
out:
    if (ret < 0) {
        rdma->error_state = ret;
    }
    return ret;
}

static int qemu_rdma_registration_start(QEMUFile *f, void *opaque,
                                        uint64_t flags)
{
    QEMUFileRDMA *rfile = opaque;
    RDMAContext *rdma = rfile->rdma;

    CHECK_ERROR_STATE();

    DDDPRINTF("start section: %" PRIu64 "\n", flags);
    qemu_put_be64(f, RAM_SAVE_FLAG_HOOK);
    qemu_fflush(f);

    return 0;
}

/*
 * Inform dest that dynamic registrations are done for now.
 * First, flush writes, if any.
 */
static int qemu_rdma_registration_stop(QEMUFile *f, void *opaque,
                                       uint64_t flags)
{
    Error *local_err = NULL, **errp = &local_err;
    QEMUFileRDMA *rfile = opaque;
    RDMAContext *rdma = rfile->rdma;
    RDMAControlHeader head = { .len = 0, .repeat = 1 };
    int ret = 0;

    CHECK_ERROR_STATE();

    qemu_fflush(f);
    ret = qemu_rdma_drain_cq(f, rdma);

    if (ret < 0) {
        goto err;
    }

    if (flags == RAM_CONTROL_SETUP) {
        RDMAControlHeader resp = {.type = RDMA_CONTROL_RAM_BLOCKS_RESULT };
        RDMALocalBlocks *local = &rdma->local_ram_blocks;
        int reg_result_idx, i, j, nb_remote_blocks;

        head.type = RDMA_CONTROL_RAM_BLOCKS_REQUEST;
        DPRINTF("Sending registration setup for ram blocks...\n");

        /*
         * Make sure that we parallelize the pinning on both sides.
         * For very large guests, doing this serially takes a really
         * long time, so we have to 'interleave' the pinning locally
         * with the control messages by performing the pinning on this
         * side before we receive the control response from the other
         * side that the pinning has completed.
         */
        ret = qemu_rdma_exchange_send(rdma, &head, NULL, &resp,
                    &reg_result_idx, rdma->pin_all ?
                    qemu_rdma_reg_whole_ram_blocks : NULL);
        if (ret < 0) {
            ERROR(errp, "receiving remote info!\n");
            return ret;
        }

        qemu_rdma_move_header(rdma, reg_result_idx, &resp);
        memcpy(rdma->block,
            rdma->wr_data[reg_result_idx].control_curr, resp.len);

        nb_remote_blocks = resp.len / sizeof(RDMARemoteBlock);

        /*
         * The protocol uses two different sets of rkeys (mutually exclusive):
         * 1. One key to represent the virtual address of the entire ram block.
         *    (dynamic chunk registration disabled - pin everything with one rkey.)
         * 2. One to represent individual chunks within a ram block.
         *    (dynamic chunk registration enabled - pin individual chunks.)
         *
         * Once the capability is successfully negotiated, the destination transmits
         * the keys to use (or sends them later) including the virtual addresses
         * and then propagates the remote ram block descriptions to his local copy.
         */

        if (local->nb_blocks != nb_remote_blocks) {
            ERROR(errp, "ram blocks mismatch #1! "
                        "Your QEMU command line parameters are probably "
                        "not identical on both the source and destination.\n");
            return -EINVAL;
        }

        for (i = 0; i < nb_remote_blocks; i++) {
            network_to_remote_block(&rdma->block[i]);

            /* search local ram blocks */
            for (j = 0; j < local->nb_blocks; j++) {
                if (rdma->block[i].offset != local->block[j].offset) {
                    continue;
                }

                if (rdma->block[i].length != local->block[j].length) {
                    ERROR(errp, "ram blocks mismatch #2! "
                        "Your QEMU command line parameters are probably "
                        "not identical on both the source and destination.\n");
                    return -EINVAL;
                }
                local->block[j].remote_host_addr =
                        rdma->block[i].remote_host_addr;
                local->block[j].remote_rkey = rdma->block[i].remote_rkey;
                break;
            }

            if (j >= local->nb_blocks) {
                ERROR(errp, "ram blocks mismatch #3! "
                        "Your QEMU command line parameters are probably "
                        "not identical on both the source and destination.\n");
                return -EINVAL;
            }
        }
    }

    DDDPRINTF("Sending registration finish %" PRIu64 "...\n", flags);

    head.type = RDMA_CONTROL_REGISTER_FINISHED;
    ret = qemu_rdma_exchange_send(rdma, &head, NULL, NULL, NULL, NULL);

    if (ret < 0) {
        goto err;
    }

    return 0;
err:
    rdma->error_state = ret;
    return ret;
}

static int qemu_rdma_get_fd(void *opaque)
{
    QEMUFileRDMA *rfile = opaque;
    RDMAContext *rdma = rfile->rdma;

    return rdma->comp_channel->fd;
}

const QEMUFileOps rdma_read_ops = {
    .get_buffer    = qemu_rdma_get_buffer,
    .get_fd        = qemu_rdma_get_fd,
    .close         = qemu_rdma_close,
    .hook_ram_load = qemu_rdma_registration_handle,
};

const QEMUFileOps rdma_write_ops = {
    .put_buffer         = qemu_rdma_put_buffer,
    .close              = qemu_rdma_close,
    .before_ram_iterate = qemu_rdma_registration_start,
    .after_ram_iterate  = qemu_rdma_registration_stop,
    .save_page          = qemu_rdma_save_page,
};

static void *qemu_fopen_rdma(RDMAContext *rdma, const char *mode)
{
    QEMUFileRDMA *r = g_malloc0(sizeof(QEMUFileRDMA));

    if (qemu_file_mode_is_not_valid(mode)) {
        return NULL;
    }

    r->rdma = rdma;

    if (mode[0] == 'w') {
        r->file = qemu_fopen_ops(r, &rdma_write_ops);
    } else {
        r->file = qemu_fopen_ops(r, &rdma_read_ops);
    }

    return r->file;
}

static void rdma_accept_incoming_migration(void *opaque)
{
    RDMAContext *rdma = opaque;
    int ret;
    QEMUFile *f;
    Error *local_err = NULL, **errp = &local_err;

    DPRINTF("Accepting rdma connection...\n");
    ret = qemu_rdma_accept(rdma);

    if (ret) {
        ERROR(errp, "RDMA Migration initialization failed!\n");
        return;
    }

    DPRINTF("Accepted migration\n");

    f = qemu_fopen_rdma(rdma, "rb");
    if (f == NULL) {
        ERROR(errp, "could not qemu_fopen_rdma!\n");
        qemu_rdma_cleanup(rdma);
        return;
    }

    rdma->migration_started_on_destination = 1;
    process_incoming_migration(f);
}

void rdma_start_incoming_migration(const char *host_port, Error **errp)
{
    int ret;
    RDMAContext *rdma;
    Error *local_err = NULL;

    DPRINTF("Starting RDMA-based incoming migration\n");
    rdma = qemu_rdma_data_init(host_port, &local_err);

    if (rdma == NULL) {
        goto err;
    }

    ret = qemu_rdma_dest_init(rdma, &local_err);

    if (ret) {
        goto err;
    }

    DPRINTF("qemu_rdma_dest_init success\n");

    ret = rdma_listen(rdma->listen_id, 5);

    if (ret) {
        ERROR(errp, "listening on socket!\n");
        goto err;
    }

    DPRINTF("rdma_listen success\n");

    qemu_set_fd_handler2(rdma->channel->fd, NULL,
                         rdma_accept_incoming_migration, NULL,
                            (void *)(intptr_t) rdma);
    return;
err:
    error_propagate(errp, local_err);
    g_free(rdma);
}

void rdma_start_outgoing_migration(void *opaque,
                            const char *host_port, Error **errp)
{
    MigrationState *s = opaque;
    Error *local_err = NULL, **temp = &local_err;
    RDMAContext *rdma = qemu_rdma_data_init(host_port, &local_err);
    int ret = 0;

    if (rdma == NULL) {
        ERROR(temp, "Failed to initialize RDMA data structures! %d\n", ret);
        goto err;
    }

    ret = qemu_rdma_source_init(rdma, &local_err,
        s->enabled_capabilities[MIGRATION_CAPABILITY_X_RDMA_PIN_ALL]);

    if (ret) {
        goto err;
    }

    DPRINTF("qemu_rdma_source_init success\n");
    ret = qemu_rdma_connect(rdma, &local_err);

    if (ret) {
        goto err;
    }

    DPRINTF("qemu_rdma_source_connect success\n");

    s->file = qemu_fopen_rdma(rdma, "wb");
    migrate_fd_connect(s);
    return;
err:
    error_propagate(errp, local_err);
    g_free(rdma);
    migrate_fd_error(s);
}
