#ifndef __FEMU_FTL_BUFFER_H
#define __FEMU_FTL_BUFFER_H

#include "../nvme.h"
// #include "ftl.h"
typedef uint64_t lpn_t;
struct ssd;

/* What caused a writeback. Log/accounting only: flush_pg() itself behaves the
 * same either way, the caller decides what to do with the latency it returns. */
enum {
    WRITEBACK_SRC_BACKGROUND = 0, /* watermark-driven, entry stays cached */
    WRITEBACK_SRC_FOREGROUND = 1, /* a miss waited for it, entry is evicted */
    WRITEBACK_SRC_NR         = 2,
};

/* Program one page to NAND. Returns how long from now until it completes,
 * queueing on the target LUN included, so the caller can either charge that to
 * a request or record it as a completion time. */
uint64_t flush_pg(struct ssd* ssd, lpn_t lpn, int src);

/* May background writeback issue one more program to the die the next write
 * would land on? Also samples stats.die_backlog_max_ns, so call it only
 * where background writeback actually wants to issue. `now` is the caller's
 * QEMU_CLOCK_REALTIME reading, passed in so one pass reads the clock once. */
bool writeback_die_has_room(struct ssd *ssd, uint64_t now);

extern bool ioctl_flag;


struct buffer;
struct set;


struct buffer_entry {
	lpn_t lpn;
    // uint32_t idx;
    bool dirty;
    /* Background writeback completion time, 0 if none outstanding. Until then
     * the data is not in NAND, so the line cannot be handed to another LPN even
     * though it is no longer dirty. Per-entry counterpart to
     * nand_lun.evict_endtime. */
    uint64_t writeback_endtime;
    /* buffer->insert_cnt when the line last turned dirty. The gap to the
     * current insert_cnt is how far it has moved toward the FIFO head; see
     * is_dirty_line_too_old(). */
    uint64_t insert_cnt_when_dirtied;
    /* prev/next pointers chaining this entry into buffer->dirty_list */
    QTAILQ_ENTRY(buffer_entry) dirty_list_entry;
    union {
        /*struct for CLOCK*/
        struct {
            uint8_t ref; 
        };
        /*struct for S3-FIFO*/
        struct {
            uint8_t freq;
            uint8_t tier; /* 0 = S, 1 = M */
        };
    };
    
	QTAILQ_ENTRY(buffer_entry) b_entry;
};

/* evict_victim/insert_entry keep their int return (-1 on failure) and report the
 * wait through *wait, which is added to rather than assigned so that one insert
 * can accumulate several evictions. */
struct buffer_ops {
    int (*evict_victim)(struct buffer *, struct set *, uint64_t *wait);
    int (*insert_entry)(struct buffer *, struct buffer_entry *, uint64_t *wait);
    /*
     * Remove one specific entry (TRIM only). evict_victim only removes whichever
     * victim the policy itself picks, so it can't target a specific LPN.
     * NULL for unsupported policies (CLOCK/S3FIFO); callers check this and reject TRIM.
     */
    int (*remove_entry)(struct buffer *, struct buffer_entry *);
};

struct set {  
    union {
        QTAILQ_HEAD(queue, buffer_entry) queue;
        struct buffer_entry* entry; /* For Direct-mapped buffer */
    };

    union {
        /* tmp for LIFO */
        struct {
            QTAILQ_HEAD(, buffer_entry) tmp;
        };

        /* for S3-FIFO */
        struct {
            QTAILQ_HEAD(, buffer_entry) small;
            QTAILQ_HEAD(, buffer_entry) ghost;

            int cnt_small;
            int cnt_main;
            int cnt_ghost;
        };
    };
    
    struct buffer_entry *hand;   // clock hand; NULL if empty
    int cnt;
};

enum {
    WAY_1,  /* Direct-mapped */
    WAY_2,
    WAY_4,
    WAY_8,
    WAY_16,
    WAY_FULL /* Fully-associate */
};

struct buffer {
    struct ssd *ssd; /* parent */

    uint64_t size; /* # of entries */
    uint64_t entry_cnt;
    /* Original Cylon. Occupancy (not dirty) ratios at which the cache was to be
     * considered full / force-evicted, both set to ~1.0 so they never fired.
     * Now dead: buffer_full() has no live caller and buffer_force_eviction() is
     * commented out. Eviction happens per-set on insert instead. Left in place
     * to keep the diff against original Cylon small. */
    double thres_pcent;
    double force_pcent;

    /* Background writeback watermarks as line counts, derived once from
     * ssd->writeback_watermark_high/_low (percentages). Dirty lines rather than
     * occupied ones because the cache is always full after warm-up, so
     * entry_cnt never moves. The gap between the two also bounds how much one
     * pass writes back, so there is no separate batch limit. */
    uint64_t dirty_cnt;
    uint64_t dirty_lines_high;
    uint64_t dirty_lines_low;
    /* Dirty entries, oldest dirtied first; background writeback takes from the
     * head instead of scanning the set queues. Under FIFO this is eviction
     * order too, since a line turns dirty almost only when it is inserted. */
    QTAILQ_HEAD(, buffer_entry) dirty_list;
    /* Lines inserted so far, the clock for a dirty line's age (insert_cnt -
     * insert_cnt_when_dirtied) that triggers background writeback by age.
     * Unlike ins_cnt, never reset it: ages are differences across time, and a
     * reset would make every dirty line look too old at once. */
    uint64_t insert_cnt;
    /* Write a dirty line back, whatever the dirty count, once this many lines
     * were inserted after it; 0 = off. Counted in insertions, not time, because
     * a FIFO line is pushed out by insertions: the watermarks alone let a line
     * reach the head dirty whenever writes are too sparse to cross high. */
    uint64_t age_limit;
    /* When the issue limit first refused, 0 while it is not refusing. Collapses
     * the many refusals one spin loop produces into a single time span for
     * stats.writeback_bg_blocked_ns. */
    uint64_t writeback_blocked_since;

	GTree *tree;
    GTree *ghost_tree;
    unsigned long *bitmap; /* bitmap for allocation */
    int way;

    uint64_t set_mask;
    struct set* sets;

    /* Eviction policy */
    int policy;
    
    /* Next-N Prefetching */
    int degree;
    /* Prefetch Stride */
    int stride;

    struct buffer_ops ops;

    /* Hits the device actually saw. Inserting an entry clears the EPT MMIO flag
     * (direct_mr_add), so a cached page is normally reached without a VM exit
     * and never counted here. What is left is accesses that trapped anyway: a
     * race with the insert, or a failed flag clear. A rising value means the
     * direct-mapping path is failing -- it is not a hit rate. */
    uint64_t load_hit_trapped;
    uint64_t store_hit_trapped;
    /* Misses always trap, so these are complete. load/store is the guest
     * instruction, not a NAND direction: a store miss on a mapped LPN also does
     * a fill read (r_cache_fill) and only marks the line dirty, so its NAND
     * program happens later, at writeback. */
    uint64_t load_miss;
    uint64_t store_miss;

    uint64_t ins_cnt;
    uint64_t evict_cnt;
    
    uint64_t gen;

    SsdDramBackend *ssdbackend;
};


enum {
    INSERT_NO_PREFETCH = 0,
    INSERT_PREFETCH = 1,
};



void direct_mr_add(struct buffer *b, lpn_t lpn);
void direct_mr_del(struct buffer *b, lpn_t lpn);

/* Common OPS */
struct buffer_entry *buffer_entry_init(struct buffer *b, lpn_t lpn);
bool buffer_full(struct buffer *b);
bool buffer_force_eviction(struct buffer *b);
struct buffer_entry* buffer_lookup_entry(struct buffer *b, lpn_t lpn);
struct set* buffer_get_set(struct buffer *b, lpn_t lpn);
// bool buffer_hit(struct buffer *b, lpn_t lpn);

// struct buffer_entry* buffer_select_victim(struct buffer *);
// bool buffer_evict_victim(struct buffer *, struct buffer_entry *);
/* Returns the time the requester must wait for the foreground insert's eviction. */
uint64_t buffer_insert_entry(struct buffer *, struct buffer_entry *, int);
bool buffer_remove_entry(struct buffer *, lpn_t);
void buffer_clear(struct buffer *);
void buffer_init_set(struct buffer *);
void buffer_destroy_set(struct buffer *);
// void buffer_print_stat(struct buffer *);

/* Keep entry->dirty and buffer->dirty_cnt in step. */
void buffer_mark_dirty(struct buffer *b, struct buffer_entry *ent, bool dirty);

/* Cost of taking this entry's line. Every policy's evict_victim() goes through
 * here, so the dirty/in-flight/clean rule lives in one place. */
uint64_t buffer_evict_cost(struct buffer *b, struct buffer_entry *victim);

/* Background writeback, by dirty count (watermarks) and by age
 * (age_limit); see struct buffer. */
void buffer_writeback_bg(struct buffer *b);


/* Todo: refactor for better modularity */
/* LIFO */
int lifo_evict_victim(struct buffer *, struct set *, uint64_t *wait);
int lifo_insert_entry(struct buffer *, struct buffer_entry *, uint64_t *wait);
int lifo_remove_entry(struct buffer *, struct buffer_entry *);

/* FIFO */
int fifo_evict_victim(struct buffer *, struct set *, uint64_t *wait);
int fifo_insert_entry(struct buffer *, struct buffer_entry *, uint64_t *wait);
int fifo_remove_entry(struct buffer *, struct buffer_entry *);

/* CLOCK (second chance)*/
int clock_evict_victim(struct buffer *, struct set *, uint64_t *wait);
int clock_insert_entry(struct buffer *, struct buffer_entry *, uint64_t *wait);

/* S3FIFO */
int s3fifo_evict_victim(struct buffer *, struct set *, uint64_t *wait);
int s3fifo_insert_entry(struct buffer *, struct buffer_entry *, uint64_t *wait);

#endif
