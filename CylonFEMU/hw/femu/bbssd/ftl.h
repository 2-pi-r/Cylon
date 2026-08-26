#ifndef __FEMU_FTL_H
#define __FEMU_FTL_H

#include "../nvme.h"
#include "buffer.h"

#define INVALID_PPA     (~(0ULL))
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))

typedef uint64_t lpn_t;

enum {
    NAND_READ =  0,
    NAND_WRITE = 1,
    NAND_ERASE = 2,

    NAND_READ_LATENCY = 40000,
    NAND_PROG_LATENCY = 200000,
    NAND_ERASE_LATENCY = 2000000,
};

enum {
    USER_IO = 0,
    GC_IO = 1,
};

enum {
    SEC_FREE = 0,
    SEC_INVALID = 1,
    SEC_VALID = 2,

    PG_FREE = 0,
    PG_INVALID = 1,
    PG_VALID = 2
};

enum {
    FEMU_ENABLE_GC_DELAY = 1,
    FEMU_DISABLE_GC_DELAY = 2,

    FEMU_ENABLE_DELAY_EMU = 3,
    FEMU_DISABLE_DELAY_EMU = 4,

    FEMU_RESET_ACCT = 5,
    FEMU_ENABLE_LOG = 6,
    FEMU_DISABLE_LOG = 7,
};


#define BLK_BITS    (16)
#define PG_BITS     (16)
#define SEC_BITS    (8)
#define PL_BITS     (8)
#define LUN_BITS    (8)
#define CH_BITS     (7)

/* describe a physical page addr */
struct ppa {
    union {
        struct {
            uint64_t blk : BLK_BITS;
            uint64_t pg  : PG_BITS;
            uint64_t sec : SEC_BITS;
            uint64_t pl  : PL_BITS;
            uint64_t lun : LUN_BITS;
            uint64_t ch  : CH_BITS;
            uint64_t rsv : 1;
        } g;

        uint64_t ppa;
    };
};

typedef int nand_sec_status_t;

struct nand_page {
    nand_sec_status_t *sec;
    int nsecs;
    int status;
};

struct nand_block {
    struct nand_page *pg;
    int npgs;
    int ipc; /* invalid page count */
    int vpc; /* valid page count */
    int erase_cnt;
    int wp; /* current write pointer */
};

struct nand_plane {
    struct nand_block *blk;
    int nblks;
};

struct nand_lun {
    struct nand_plane *pl;
    int npls;
    uint64_t next_lun_avail_time;
    bool busy;
    uint64_t gc_endtime;
    uint64_t evict_endtime;
};

struct ssd_channel {
    struct nand_lun *lun;
    int nluns;
    uint64_t next_ch_avail_time;
    bool busy;
    uint64_t gc_endtime;
    uint64_t evict_endtime;
};

struct ssdparams {
    int secsz;        /* sector size in bytes */
    int secs_per_pg;  /* # of sectors per page */
    int pgs_per_blk;  /* # of NAND pages per block */
    int blks_per_pl;  /* # of blocks per plane */
    int pls_per_lun;  /* # of planes per LUN (Die) */
    int luns_per_ch;  /* # of LUNs per channel */
    int nchs;         /* # of channels in the SSD */

    int pg_rd_lat;    /* NAND page read latency in nanoseconds */
    int pg_wr_lat;    /* NAND page program latency in nanoseconds */
    int blk_er_lat;   /* NAND block erase latency in nanoseconds */
    int ch_xfer_lat;  /* channel transfer latency for one page in nanoseconds
                       * this defines the channel bandwith
                       */

    double gc_thres_pcent;
    int gc_thres_lines;
    double gc_thres_pcent_high;
    int gc_thres_lines_high;
    bool enable_gc_delay;

    /* below are all calculated values */
    int secs_per_blk; /* # of sectors per block */
    int secs_per_pl;  /* # of sectors per plane */
    int secs_per_lun; /* # of sectors per LUN */
    int secs_per_ch;  /* # of sectors per channel */
    int tt_secs;      /* # of sectors in the SSD */

    int pgs_per_pl;   /* # of pages per plane */
    int pgs_per_lun;  /* # of pages per LUN (Die) */
    int pgs_per_ch;   /* # of pages per channel */
    int tt_pgs;       /* total # of pages in the SSD */

    int blks_per_lun; /* # of blocks per LUN */
    int blks_per_ch;  /* # of blocks per channel */
    int tt_blks;      /* total # of blocks in the SSD */

    int secs_per_line;
    int pgs_per_line;
    int blks_per_line;
    int tt_lines;

    int pls_per_ch;   /* # of planes per channel */
    int tt_pls;       /* total # of planes in the SSD */

    int tt_luns;      /* total # of LUNs in the SSD */

    uint64_t buffer_size;
	double buffer_thres_pcent;
    int policy; /* Buffer replacement policy */
    int degree; /* Prefetch degree */
    int way; /* Set-associativity */

	int read_hit_cnt;
	int read_cnt;
	int write_hit_cnt;
	int write_cnt;
};

typedef struct line {
    int id;  /* line id, the same as corresponding block id */
    int ipc; /* invalid page count in this line */
    int vpc; /* valid page count in this line */
    QTAILQ_ENTRY(line) entry; /* in either {free,victim,full} list */
    /* position in the priority queue for victim lines */
    size_t                  pos;
} line;

/* wp: record next write addr */
struct write_pointer {
    struct line *curline;
    int ch;
    int lun;
    int pg;
    int blk;
    int pl;
};

struct line_mgmt {
    struct line *lines;
    /* free line list, we only need to maintain a list of blk numbers */
    QTAILQ_HEAD(free_line_list, line) free_line_list;
    pqueue_t *victim_line_pq;
    //QTAILQ_HEAD(victim_line_list, line) victim_line_list;
    QTAILQ_HEAD(full_line_list, line) full_line_list;
    int tt_lines;
    int free_line_cnt;
    int victim_line_cnt;
    int full_line_cnt;
};

struct nand_cmd {
    int type;
    int cmd;
    int64_t stime; /* Coperd: request arrival time */
};

enum {
    CXL_READ,
    CXL_WRITE,
    BUF_PRINT_STAT,
    BUF_CLEAR,
    SSD_INIT,
    INC_PREFETCH_DEGREE,
    CXL_TRIM,
    CXL_STATS_RESET,
    CXL_STATS_DUMP,
};

/* Sampling period in host writes, and the cap on samples held in memory */
#define SSD_STATS_SAMPLE_PERIOD (1 << 12)
#define SSD_STATS_SAMPLE_MAX    (1 << 18)

struct ssd_stats_sample {
    uint64_t time_ns;
    uint64_t w_host;
    uint64_t w_gc;
    uint64_t gc_lines;
    uint64_t gc_lines_forced;
    uint64_t live_pages;
    uint64_t free_lines;
};

struct ssd_stats {
    /* NAND page programs by trigger. w_first_touch is the allocation done on the
     * first access to an unmapped LPN, so even a read programs a page; it should
     * stay flat after warm-up, otherwise the measured window isn't warmed up. */
    uint64_t w_first_touch;
    uint64_t w_writeback;      /* dirty buffer eviction */
    uint64_t w_gc;             /* GC valid-page copies */

    uint64_t gc_lines;         /* lines reclaimed */
    uint64_t gc_lines_forced;  /* of those, reclaimed below the high watermark */
    /* Forced GC that found no victim: out of reclaimable space */
    uint64_t gc_forced_novictim;

    /* Mapped LPNs. U = live_pages / tt_pgs, the variable GC copy cost hinges on,
     * and the device-side cross-check for the guest's slow-tier usage. */
    uint64_t live_pages;

    /* Indexed by CYLON_TRIM_SRC_*. Split by source so a disagreement with the
     * guest's trim_pages_* in /proc/vmstat points at one path. */
    uint64_t trim_pages[3];
    uint64_t trim_cmds[3];

    /* GC copies per LPN, saturating. Joined offline against the guest's shadow
     * link/unlink trace to tell whether a few cold pages are recopied forever. */
    uint16_t *gc_copy_cnt;

    /* Held in memory and written out only on CXL_STATS_DUMP: the FTL thread
     * drives the wall-clock timing model, so file I/O here would distort it. */
    struct ssd_stats_sample *samples;
    int nr_samples;
    uint64_t next_sample_w_host;
};

/* A TRIM range. The guest builds an array of these in its own RAM. */
struct cylon_trim_ent {
    uint32_t start_lpn;
    uint32_t nr_pages;
};

/* Which guest path produced a TRIM batch. Log/accounting only. */
enum {
    CYLON_TRIM_SRC_REPORT = 0,  /* Linux free page reporting */
    CYLON_TRIM_SRC_HOOK   = 1,  /* SSP page-death hook */
    CYLON_TRIM_SRC_MANUAL = 2,  /* cxl write-labels, for debugging */
};

/*
 * Doorbell payload. Only this crosses the mailbox; the device DMAs the range
 * list out of guest RAM, so the cost no longer scales with the range count.
 * Writing the mailbox payload costs one VM exit per 4 bytes.
 */
struct cylon_trim_db {
    uint32_t list_pfn;   /* guest physical page holding the cylon_trim_ent array */
    uint16_t count;      /* ranges in that page */
    uint16_t src;        /* CYLON_TRIM_SRC_* */
};

struct cxl_req {
    /* request */
    struct nand_cmd *ncmd;
    lpn_t lpn;

    /* CXL_TRIM only. The issuing thread blocks until completion, so we point
     * at the caller's buffer instead of copying it. */
    const struct cylon_trim_ent *trim_ents;
    int trim_cnt;
    int trim_src;

    /* response */
    uint64_t expire_time;

    size_t pos;
};

struct ssd {
    char *ssdname;
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct SsdDramBackend *b;
    int buffer_way;

    struct ppa *maptbl; /* page level mapping table */
    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    struct write_pointer wp;
    struct line_mgmt lm;

    /* lockless ring for communication with NVMe IO thread */
    struct rte_ring **to_ftl;
    struct rte_ring **to_poller;

    struct rte_ring *cxl_req;
    struct rte_ring *cxl_resp;

    struct buffer dram_buffer;

    struct ssd_stats stats;

    bool *dataplane_started_ptr;
    QemuThread ftl_thread;
};

void ssd_init(FemuCtrl *n);
void ssd_reset(FemuCtrl *n);

/* valid including OP region */
static inline bool valid_lpn(struct ssd *ssd, lpn_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

/* valid within user-exposed capacity only (excludes OP) */
static inline bool valid_user_lpn(struct ssd *ssd, lpn_t lpn)
{
    return (lpn < (ssd->b->size >> 12));
}

// int flush_pg(struct ssd* ssd, lpn_t lpn);


#ifdef FEMU_DEBUG_FTL
#define ftl_debug(fmt, ...) \
    do { printf("[FEMU] FTL-Dbg: " fmt, ## __VA_ARGS__); } while (0)
#else
#define ftl_debug(fmt, ...) \
    do { } while (0)
#endif

#define ftl_err(fmt, ...) \
    do { fprintf(stderr, "[FEMU] FTL-Err: " fmt, ## __VA_ARGS__); } while (0)

#define ftl_log(fmt, ...) \
    do { printf("[FEMU] FTL-Log: " fmt, ## __VA_ARGS__); } while (0)


/* FEMU assert() */
#ifdef FEMU_DEBUG_FTL
#define ftl_assert(expression) assert(expression)
#else
#define ftl_assert(expression)
#endif


#endif
