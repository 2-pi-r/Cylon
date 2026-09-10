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
    double thres_pcent;
    double force_pcent;

    /* Background writeback watermarks, in entries. Dirty lines rather than
     * occupied ones because the cache is always full after warm-up, so
     * entry_cnt never moves. (dirty_hi - dirty_lo) also bounds how much one
     * pass writes back, so there is no separate batch limit. */
    uint64_t dirty_cnt;
    uint64_t dirty_hi;
    uint64_t dirty_lo;
    uint64_t wb_cursor;  /* set to resume the background scan from */

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
    uint64_t read_hit_trapped;
    uint64_t write_hit_trapped;
    /* Misses always trap, so these are complete. */
    uint64_t read_miss;
    uint64_t write_miss;

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

/* Watermark-driven background writeback; see struct buffer's dirty_hi/dirty_lo. */
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
