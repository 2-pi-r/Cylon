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

/* Sampling periods, and the cap on samples held in memory. A read-dominated
 * kernel (PageRank) programs almost nothing while it runs, so sampling on host
 * writes alone leaves that phase nearly uncovered -- the series ends up dense
 * over the load and sparse over the part being measured. The request period
 * keeps it dense there too, and is larger because a miss storm produces
 * requests far faster than page programs. */
#define SSD_STATS_SAMPLE_PERIOD     (1 << 12)
#define SSD_STATS_SAMPLE_REQ_PERIOD (1 << 14)
#define SSD_STATS_SAMPLE_MAX        (1 << 18)

struct ssd_stats_sample {
    uint64_t time_ns;
    /* w_host (w_first_touch + w_writeback) used to sit in this slot. It read as
     * "requests the host sent", which it is not, so it is gone. w_first_touch is
     * 0 on the CXL path, so the writeback total below carries the same numbers
     * and existing column positions still line up. */
    uint64_t w_writeback;   /* both sources summed */
    uint64_t w_gc;
    uint64_t gc_lines;
    uint64_t gc_lines_forced;
    uint64_t live_pages;
    uint64_t free_lines;
    /* Buffer hit/miss, split by direction. Misses are complete and are what a
     * request actually waits on; the hit counts only cover hits that trapped
     * into the device, so they are not a hit rate (see struct buffer). */
    uint64_t r_hit_trapped;
    uint64_t r_miss;
    uint64_t w_hit_trapped;
    uint64_t w_miss;
    uint64_t stall_ns;
    /* Appended, so column positions above do not move. */
    uint64_t w_writeback_fg;      /* of w_writeback, the part a miss waited on */
    uint64_t r_cache_fill;
    uint64_t r_gc;
    uint64_t stall_cache_fill_ns;
    uint64_t stall_writeback_ns;
    uint64_t evict_inflight_waits;
    uint64_t dirty_cnt;           /* shows the watermark band being worked */
};

struct ssd_stats {
    /* NAND page programs by trigger. w_first_touch is the allocation done on the
     * first access to an unmapped LPN, so even a read programs a page; it should
     * stay flat after warm-up, otherwise the measured window isn't warmed up. */
    uint64_t w_first_touch;
    /* Indexed by WRITEBACK_SRC_*: background (watermark-driven, entry stays
     * cached) vs foreground (a miss needed the line and waited for the program). */
    uint64_t w_writeback[WRITEBACK_SRC_NR];
    uint64_t w_gc;             /* GC valid-page copies */

    uint64_t gc_lines;         /* lines reclaimed */
    uint64_t gc_lines_forced;  /* of those, reclaimed below the high watermark */
    /* Forced GC that found no victim: out of reclaimable space */
    uint64_t gc_forced_novictim;

    /* NAND page reads that filled the cache after a miss on a mapped LPN. Named
     * for the cause, like the w_* counters, so distinct from r_gc. Smaller than
     * read_miss + write_miss: a miss on an unmapped LPN reads nothing. Holds for
     * write misses too -- those fill the line as well. */
    uint64_t r_cache_fill;
    /* NAND page reads GC does before copying a valid page. Always equal to w_gc,
     * so derivable, but device load is r_cache_fill + r_gc + w_writeback + w_gc
     * and this half is easy to forget. */
    uint64_t r_gc;

    /* Emulated device latency actually charged to the guest, summed over CXL
     * requests. Hits and unmapped misses add zero, so this is the part of the
     * run the timing model accounts for; compare it against the benchmark's
     * wall clock to see how much of the run came from anything else.
     * stall_ns is the sum of the two below. */
    uint64_t stall_ns;
    uint64_t stall_cache_fill_ns;  /* waiting for the fill read */
    uint64_t stall_writeback_ns;   /* waiting for the victim's line to free up */

    /* Evictions that waited on an in-flight background writeback rather than
     * programming the page themselves. These call no flush_pg(), so they leave
     * no trace in w_writeback[]; count them to see whether modelling the
     * in-flight state changes anything. */
    uint64_t evict_inflight_waits;

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
    uint64_t next_sample_w_writeback;
    uint64_t next_sample_reqs;
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
    /* Background writeback watermarks as a percentage of cache lines. Runtime
     * properties rather than constants because the sweep over them is the point
     * of the experiment, and rebuilding per value is not practical. */
    int wb_thres_pcent;      /* start cleaning above this share of dirty lines */
    int wb_thres_pcent_low;  /* stop once back under this one */

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
