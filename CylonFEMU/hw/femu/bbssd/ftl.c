// #define FEMU_DEBUG_FTL
#include "ftl.h"

static void *ftl_thread(void *arg);

static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, lpn_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

/*
 * Creating or destroying a mapping is the only thing that moves live_pages;
 * writebacks and GC copies remap an already-live LPN, so hooking
 * mark_page_valid() instead would over-count by every GC copy.
 */
static inline void set_maptbl_ent(struct ssd *ssd, lpn_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);

    if (mapped_ppa(&ssd->maptbl[lpn]) != mapped_ppa(ppa))
        ssd->stats.live_pages += mapped_ppa(ppa) ? 1 : -1;

    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, lpn_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

static void ssd_init_write_pointer(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = 0;
    wpp->pl = 0;
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

static void ssd_advance_write_pointer(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd);
                if (!wpp->curline) {
                    /* TODO */
                    abort();
                }
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(wpp->pl == 0);
            }
        }
    }
}

static struct ppa get_new_page(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp, FemuCtrl *n)
{
    spp->secsz = n->bb_params.secsz; // 512
    spp->secs_per_pg = n->bb_params.secs_per_pg; // 8
    spp->pgs_per_blk = n->bb_params.pgs_per_blk; //256
    spp->blks_per_pl = n->bb_params.blks_per_pl; /* 256 16GB */
    spp->pls_per_lun = n->bb_params.pls_per_lun; // 1
    spp->luns_per_ch = n->bb_params.luns_per_ch; // 8
    spp->nchs = n->bb_params.nchs; // 8

    spp->pg_rd_lat = n->bb_params.pg_rd_lat;
    spp->pg_wr_lat = n->bb_params.pg_wr_lat;
    spp->blk_er_lat = n->bb_params.blk_er_lat;
    spp->ch_xfer_lat = n->bb_params.ch_xfer_lat;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    spp->gc_thres_pcent = n->bb_params.gc_thres_pcent/100.0;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high/100.0;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;

    spp->read_hit_cnt = 0;
    spp->write_hit_cnt = 0;
    spp->read_cnt = 0;
    spp->write_cnt = 0;
    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

static void ssd_init_stats(struct ssd *ssd)
{
    struct ssd_stats *st = &ssd->stats;

    st->gc_copy_cnt = g_malloc0(sizeof(uint16_t) * ssd->sp.tt_pgs);
    st->samples = g_malloc0(sizeof(struct ssd_stats_sample) *
                            SSD_STATS_SAMPLE_MAX);
}

/* Called once per CXL request rather than per page, to keep the period check
 * off the per-page write and GC paths. */
static void ssd_stats_sample(struct ssd *ssd)
{
    struct ssd_stats *st = &ssd->stats;
    struct buffer *b = &ssd->dram_buffer;
    uint64_t w_writeback = st->w_writeback[WRITEBACK_SRC_BACKGROUND] +
                           st->w_writeback[WRITEBACK_SRC_FOREGROUND];
    uint64_t reqs = b->read_hit_trapped + b->read_miss +
                    b->write_hit_trapped + b->write_miss;

    /* Either counter making a period's worth of progress earns a sample, so a
     * phase that only reads is covered as densely as one that only writes. */
    if (w_writeback < st->next_sample_w_writeback && reqs < st->next_sample_reqs)
        return;
    st->next_sample_w_writeback = w_writeback + SSD_STATS_SAMPLE_PERIOD;
    st->next_sample_reqs = reqs + SSD_STATS_SAMPLE_REQ_PERIOD;

    if (st->nr_samples >= SSD_STATS_SAMPLE_MAX)
        return;

    st->samples[st->nr_samples++] = (struct ssd_stats_sample) {
        .time_ns              = qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
        .w_writeback          = w_writeback,
        .w_gc                 = st->w_gc,
        .gc_lines             = st->gc_lines,
        .gc_lines_forced      = st->gc_lines_forced,
        .live_pages           = st->live_pages,
        .free_lines           = ssd->lm.free_line_cnt,
        .r_hit_trapped        = b->read_hit_trapped,
        .r_miss               = b->read_miss,
        .w_hit_trapped        = b->write_hit_trapped,
        .w_miss               = b->write_miss,
        .stall_ns             = st->stall_ns,
        .w_writeback_fg       = st->w_writeback[WRITEBACK_SRC_FOREGROUND],
        .r_cache_fill         = st->r_cache_fill,
        .r_gc                 = st->r_gc,
        .stall_cache_fill_ns  = st->stall_cache_fill_ns,
        .stall_writeback_ns   = st->stall_writeback_ns,
        .evict_inflight_waits = st->evict_inflight_waits,
        .dirty_cnt            = b->dirty_cnt,
    };
}

static void ssd_stats_reset(struct ssd *ssd)
{
    struct ssd_stats *st = &ssd->stats;
    struct buffer *b = &ssd->dram_buffer;

    st->w_first_touch = st->w_gc = 0;
    memset(st->w_writeback, 0, sizeof(st->w_writeback));
    st->gc_lines = st->gc_lines_forced = st->gc_forced_novictim = 0;
    st->r_cache_fill = st->r_gc = 0;
    st->stall_ns = st->stall_cache_fill_ns = st->stall_writeback_ns = 0;
    st->evict_inflight_waits = 0;
    memset(st->trim_pages, 0, sizeof(st->trim_pages));
    memset(st->trim_cmds, 0, sizeof(st->trim_cmds));
    st->nr_samples = 0;
    st->next_sample_w_writeback = 0;
    st->next_sample_reqs = 0;
    memset(st->gc_copy_cnt, 0, sizeof(uint16_t) * ssd->sp.tt_pgs);

    /* The buffer owns its hit/miss counters; zero them here so every counter
     * covers the same window. Cache contents are left alone -- this resets the
     * measurement, not the device. */
    b->read_hit_trapped = b->read_miss = 0;
    b->write_hit_trapped = b->write_miss = 0;

    /* live_pages is current device state, not an event count, so it survives */
}

/*
 * Time series to <base>.csv, per-LPN GC copy counts to <base>.gc_copy_cnt.bin
 * (raw uint16 array indexed by LPN). Base path defaults to /tmp/cylon_ssd_stats,
 * override with $CYLON_STATS_PATH. The guest issues this at the end of a
 * measurement window, so the file I/O here perturbs nothing that is measured.
 */
static void ssd_stats_dump(struct ssd *ssd)
{
    struct ssd_stats *st = &ssd->stats;
    struct buffer *b = &ssd->dram_buffer;
    uint64_t w_writeback = st->w_writeback[WRITEBACK_SRC_BACKGROUND] +
                           st->w_writeback[WRITEBACK_SRC_FOREGROUND];
    uint64_t w_host = st->w_first_touch + w_writeback;
    uint64_t reads = b->read_hit_trapped + b->read_miss;
    uint64_t writes = b->write_hit_trapped + b->write_miss;
    const char *base = getenv("CYLON_STATS_PATH");
    char path[256];
    FILE *f;

    if (!base)
        base = "/tmp/cylon_ssd_stats";

    ftl_log("stats w_first_touch=%lu w_writeback=%lu w_gc=%lu nand_write=%lu "
            "waf=%.4f gc_lines=%lu gc_lines_forced=%lu gc_forced_novictim=%lu "
            "live_pages=%lu tt_pgs=%d u=%.4f\n",
            st->w_first_touch, w_writeback, st->w_gc, w_host + st->w_gc,
            w_host ? (double)(w_host + st->w_gc) / w_host : 0.0,
            st->gc_lines, st->gc_lines_forced, st->gc_forced_novictim,
            st->live_pages, ssd->sp.tt_pgs,
            (double)st->live_pages / ssd->sp.tt_pgs);

    /* A foreground writeback is one a request had to wait through, so its share of
     * the total says how much of the write cost the guest actually saw. */
    ftl_log("stats w_writeback_bg=%lu w_writeback_fg=%lu "
            "evict_inflight_waits=%lu dirty=%lu/%lu (hi=%lu lo=%lu)\n",
            st->w_writeback[WRITEBACK_SRC_BACKGROUND],
            st->w_writeback[WRITEBACK_SRC_FOREGROUND],
            st->evict_inflight_waits,
            b->dirty_cnt, b->size, b->dirty_hi, b->dirty_lo);

    /* No hit rate is printed: hits that never trap are not counted, so any ratio
     * built from these would understate the real one by an unknown amount. */
    ftl_log("stats r_hit_trapped=%lu r_miss=%lu w_hit_trapped=%lu w_miss=%lu "
            "entries=%lu/%lu way=%lu\n",
            b->read_hit_trapped, b->read_miss,
            b->write_hit_trapped, b->write_miss,
            b->entry_cnt, b->size,
            /* Mirrors buffer_init_set(): WAY_FULL means one set holding
             * everything, not 1 << WAY_FULL ways. */
            (b->way == WAY_FULL) ? b->size : (uint64_t)(1 << b->way));

    /* stall_s is directly comparable to the benchmark's reported kernel time:
     * whatever it does not cover came from VM exits, the FTL round trip, host
     * NUMA, or the guest itself, not from the emulated NAND. */
    ftl_log("stats r_cache_fill=%lu r_gc=%lu stall_ns=%lu stall_s=%.3f "
            "mean_stall_ns=%.1f\n",
            st->r_cache_fill, st->r_gc, st->stall_ns, st->stall_ns / 1e9,
            (reads + writes) ? (double)st->stall_ns / (reads + writes) : 0.0);

    /* writeback_share is what this whole change exists to measure: before the
     * foreground-wait path existed it was 0 by construction. */
    ftl_log("stats stall_cache_fill_ns=%lu stall_writeback_ns=%lu "
            "writeback_share=%.4f\n",
            st->stall_cache_fill_ns, st->stall_writeback_ns,
            st->stall_ns ? (double)st->stall_writeback_ns / st->stall_ns : 0.0);

    ftl_log("stats trim_pages report=%lu hook=%lu manual=%lu "
            "trim_cmds report=%lu hook=%lu manual=%lu\n",
            st->trim_pages[CYLON_TRIM_SRC_REPORT],
            st->trim_pages[CYLON_TRIM_SRC_HOOK],
            st->trim_pages[CYLON_TRIM_SRC_MANUAL],
            st->trim_cmds[CYLON_TRIM_SRC_REPORT],
            st->trim_cmds[CYLON_TRIM_SRC_HOOK],
            st->trim_cmds[CYLON_TRIM_SRC_MANUAL]);

    if (st->nr_samples >= SSD_STATS_SAMPLE_MAX)
        ftl_err("stats dump: sample buffer full, time series truncated at %d\n",
                st->nr_samples);

    snprintf(path, sizeof(path), "%s.csv", base);
    f = fopen(path, "w");
    if (!f) {
        ftl_err("stats dump: cannot open %s\n", path);
        return;
    }
    /* New columns are appended, so column positions the analysis scripts
     * already use do not move. */
    fprintf(f, "time_ns,w_writeback,w_gc,gc_lines,gc_lines_forced,live_pages,"
               "free_lines,r_hit_trapped,r_miss,w_hit_trapped,w_miss,stall_ns,"
               "w_writeback_fg,r_cache_fill,r_gc,stall_cache_fill_ns,"
               "stall_writeback_ns,evict_inflight_waits,dirty_cnt\n");
    for (int i = 0; i < st->nr_samples; i++) {
        struct ssd_stats_sample *s = &st->samples[i];

        fprintf(f, "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
                   "%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
                s->time_ns, s->w_writeback, s->w_gc, s->gc_lines,
                s->gc_lines_forced, s->live_pages, s->free_lines,
                s->r_hit_trapped, s->r_miss, s->w_hit_trapped, s->w_miss,
                s->stall_ns,
                s->w_writeback_fg, s->r_cache_fill, s->r_gc,
                s->stall_cache_fill_ns, s->stall_writeback_ns,
                s->evict_inflight_waits, s->dirty_cnt);
    }
    fclose(f);

    snprintf(path, sizeof(path), "%s.gc_copy_cnt.bin", base);
    f = fopen(path, "w");
    if (!f) {
        ftl_err("stats dump: cannot open %s\n", path);
        return;
    }
    fwrite(st->gc_copy_cnt, sizeof(uint16_t), ssd->sp.tt_pgs, f);
    fclose(f);

    ftl_log("stats dumped to %s.csv / %s.gc_copy_cnt.bin (%d samples)\n",
            base, base, st->nr_samples);
}

static int comp_buffer(const void *a, const void *b){
	return ((struct buffer_entry*)a)->lpn - ((struct buffer_entry*)b)->lpn;
}

static void buffer_init(struct ssd *ssd)
{
	struct buffer *buffer = &ssd->dram_buffer;
	struct ssdparams *spp = &ssd->sp;

    buffer->ssd = ssd;
    buffer->ssdbackend = ssd->b;
	buffer->size = spp->buffer_size;
	buffer->thres_pcent = 0.9999;
    buffer->force_pcent = 0.99999;
	buffer->entry_cnt = 0;

    /* Background writeback watermarks, converted from percent to entries once.
     * The band width (hi - lo) is also how much a single pass writes back, so
     * a narrow band means frequent small passes. Guard against a low >= high
     * that would make the hysteresis meaningless. */
    buffer->dirty_cnt = 0;
    buffer->wb_cursor = 0;
    buffer->dirty_hi = buffer->size * ssd->wb_thres_pcent / 100;
    buffer->dirty_lo = buffer->size * ssd->wb_thres_pcent_low / 100;
    if (buffer->dirty_lo >= buffer->dirty_hi)
        buffer->dirty_lo = buffer->dirty_hi / 2;

    buffer->policy = spp->policy;
    buffer->degree = spp->degree;
    buffer->stride = 1;

    buffer->way = ssd->buffer_way;
    	
    if (buffer->size == 0)
        return;
    
    buffer_init_set(buffer);

	
	buffer->tree = g_tree_new(comp_buffer);
    buffer->ghost_tree = g_tree_new(comp_buffer);
	 
	buffer->bitmap = bitmap_new(spp->buffer_size);

    buffer->read_hit_trapped = buffer->read_miss = 0;
    buffer->write_hit_trapped = buffer->write_miss = 0;

	switch (buffer->policy)
	{
	case LIFO:
		buffer->ops.evict_victim = lifo_evict_victim;
		buffer->ops.insert_entry = lifo_insert_entry;
		buffer->ops.remove_entry = lifo_remove_entry;
		break;
	case FIFO:
		buffer->ops.evict_victim = fifo_evict_victim;
		buffer->ops.insert_entry = fifo_insert_entry;
		buffer->ops.remove_entry = fifo_remove_entry;
		break;
    case CLOCK:
		buffer->ops.evict_victim = clock_evict_victim;
		buffer->ops.insert_entry = clock_insert_entry;
		buffer->ops.remove_entry = NULL;    /* TRIM unsupported */
		break;
    case S3FIFO:
        buffer->ops.evict_victim = s3fifo_evict_victim;
		buffer->ops.insert_entry = s3fifo_insert_entry;
		buffer->ops.remove_entry = NULL;    /* TRIM unsupported */
		break;
	default:
		femu_err("unknown replacement policy\n");
        assert(0);
		break;
	} 
}


static void ssd_init_buffer(struct FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    uint32_t size_mb = n->bufsz;

    spp->buffer_size = (size_mb*MiB) >> 12;
    spp->policy = n->rep;
    spp->degree = n->prefetch_degree;
    buffer_init(ssd);
}

bool ioctl_flag;
void ssd_init(FemuCtrl *n)
{
    ioctl_flag = true;

    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    ssd->b = n->mbe;
    ssd->buffer_way = n->buffer_way;
    ssd->wb_thres_pcent = n->wb_thres_pcent;
    ssd->wb_thres_pcent_low = n->wb_thres_pcent_low;

    ftl_assert(ssd);

    

    ssd_init_params(spp, n);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);

    ssd_init_stats(ssd);

    if (n->bufsz)
        ssd_init_buffer(n);
    printf("\n###########\b, read_lat: %d\n", ssd->sp.pg_rd_lat);
    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

void ssd_reset(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    buffer_clear(&ssd->dram_buffer);

    /* Todo: Free malloced ssd internals*/
}

static bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }

    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
        ssd->stats.r_gc++;
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    lpn_t lpn = get_rmap_ent(ssd, old_ppa);

    ftl_assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    ssd->stats.w_gc++;
    if (ssd->stats.gc_copy_cnt[lpn] < UINT16_MAX)
        ssd->stats.gc_copy_cnt[lpn]++;

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

static struct line *select_victim_line(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa);
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
        if (force)
            ssd->stats.gc_forced_novictim++;
        return -1;
    }

    ssd->stats.gc_lines++;
    if (force)
        ssd->stats.gc_lines_forced++;

    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(ssd, &ppa);
            clean_one_block(ssd, &ppa);
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }

    /* update line status */
    mark_line_free(ssd, &ppa);

    return 0;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    lpn_t lpn;
    uint64_t sublat, maxlat = 0;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}

static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    lpn_t lpn;
    // uint64_t victim_lpn=0;
    uint64_t maxlat = 0;
    uint64_t sublat;
    int r;
    struct buffer *buffer = &ssd->dram_buffer;
    struct buffer_entry *bentry;
    // struct nand_lun *new_lun;
    bool hitcheck = true;
    // bool dirty;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    while (should_gc_high(ssd)) {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true);
        if (r == -1)
            break;
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        bentry = buffer_lookup_entry(buffer, lpn);

        if (bentry) {//buffer hit must not be happened
            // printf("[W_Buffer]: lpn(%" PRId64 ") cache hit!! %s. THIS MUST NOT BE HAPPENED!!\n", lpn, ssd->ssdname);
            
            buffer_mark_dirty(buffer, bentry, true);
            buffer_insert_entry(buffer, bentry, INSERT_NO_PREFETCH);
        }
        else {//buffer miss
            hitcheck = false;
            // while (buffer_full(buffer)) {
            //     victim_entry = buffer_select_victim(buffer);
            //     victim_lpn = victim_entry->lpn;
            //     dirty = victim_entry->dirty;

            //     buffer_evict_victim(buffer, victim_entry);

            //     if (!dirty)
            //         continue;
                
			//     ppa = get_maptbl_ent(ssd, victim_lpn);
            //     if (mapped_ppa(&ppa)) {
            //         /* update old page information first */
            //         mark_page_invalid(ssd, &ppa);
            //         set_rmap_ent(ssd, INVALID_LPN, &ppa);
            //     }

            //     /* new write */
            //     ppa = get_new_page(ssd);
            //     /* update maptbl */
            //     set_maptbl_ent(ssd, victim_lpn, &ppa);
            //     /* update rmap */
            //     set_rmap_ent(ssd, victim_lpn, &ppa);

            //     mark_page_valid(ssd, &ppa);

            //     /* need to advance the write pointer here */
            //     ssd_advance_write_pointer(ssd);

            //     struct nand_cmd swr;
            //     swr.type = USER_IO;
            //     swr.cmd = NAND_WRITE;
            //     swr.stime = req->stime;
            //     /* get latency statistics */
            //     curlat = ssd_advance_status(ssd, &ppa, &swr);
            //     maxlat = (curlat > maxlat) ? curlat : maxlat;

            //     new_lun = get_lun(ssd, &ppa);
            //     new_lun->evict_endtime = new_lun->next_lun_avail_time;

            //     maxlat = (curlat > maxlat) ? curlat : maxlat;
            // }

            /* Fetch page from NAND to DRAM buffer*/
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
                //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
                //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
                continue;
            }

            struct nand_cmd srd;
            srd.type = USER_IO;
            srd.cmd = NAND_READ;
            srd.stime = req->stime;
            sublat = ssd_advance_status(ssd, &ppa, &srd);
            maxlat = (sublat > maxlat) ? sublat : maxlat;

            bentry = buffer_entry_init(buffer, lpn);
            buffer_insert_entry(buffer, bentry, INSERT_PREFETCH);
        }

        /* Copy to BUFFER backend for consistency */
        // ppa = get_maptbl_ent(ssd, lpn);
        // buf_idx = bentry->idx;
        
        // if (mapped_ppa(&ppa) && valid_ppa(ssd, &ppa)) {
        //     backend_memcpy(ssd, ppa, buf_idx, NAND_TO_BUF);
        // }
    }
    
    if(hitcheck == true){
		ssd->sp.write_hit_cnt++;
	}

    return maxlat;
}

// static uint64_t evict_victim(struct ssd *ssd)
// {
//     struct buffer *buffer = &ssd->dram_buffer;
//     uint64_t victim_lpn=0;
//     struct ppa ppa;
//     uint64_t curlat=0, maxlat=0;
//     struct nand_lun *new_lun;
//     struct buffer_entry *victim_entry;
//     bool dirty;
    
//     while (buffer_full(buffer)) {

//         victim_entry = buffer_select_victim(buffer);
//         assert(victim_entry);

//         victim_lpn = victim_entry->lpn;
//         dirty = victim_entry->dirty;
//         buffer_evict_victim(buffer, victim_entry);

//         if (!dirty) 
//             continue;

//         ppa = get_maptbl_ent(ssd, victim_lpn);

//         if (mapped_ppa(&ppa)) {
//             /* update old page information first */
//             /* To do: skip invalidaion for clean page */
//             mark_page_invalid(ssd, &ppa);
//             set_rmap_ent(ssd, INVALID_LPN, &ppa);
//         }

//         /* new write */
//         ppa = get_new_page(ssd);
//         /* update maptbl */
//         set_maptbl_ent(ssd, victim_lpn, &ppa);
//         /* update rmap */
//         set_rmap_ent(ssd, victim_lpn, &ppa);

//         mark_page_valid(ssd, &ppa);
        
//         /* need to advance the write pointer here */
//         ssd_advance_write_pointer(ssd);

//         struct nand_cmd swr;
//         swr.type = USER_IO;
//         swr.cmd = NAND_WRITE;
//         swr.stime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
//         /* get latency statistics */
//         curlat = ssd_advance_status(ssd, &ppa, &swr);
//         maxlat = (curlat > maxlat) ? curlat : maxlat;

//         new_lun = get_lun(ssd, &ppa);
//         new_lun->evict_endtime = new_lun->next_lun_avail_time;

//         maxlat = (curlat > maxlat) ? curlat : maxlat;
//     }
    
//     return maxlat;
// }


/*
 * TRIM: invalidate an LPN the host has told us it no longer uses.
 * mark_page_invalid() bumps the invalid-page count (ipc) and moves the line
 * onto the victim queue, so the invalid page created here becomes something
 * GC can reclaim.
 * Touches FTL state, so this must only ever be called from the FTL thread.
 */
static void ftl_trim(struct ssd *ssd, lpn_t lpn)
{
    struct ppa ppa = get_maptbl_ent(ssd, lpn);

    if (mapped_ppa(&ppa)) {
        struct ppa unmapped;

        mark_page_invalid(ssd, &ppa);
        set_rmap_ent(ssd, INVALID_LPN, &ppa);

        unmapped.ppa = UNMAPPED_PPA;
        set_maptbl_ent(ssd, lpn, &unmapped);
    }

    buffer_remove_entry(&ssd->dram_buffer, lpn);
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    ssd->cxl_req = n->cxl_req;
    ssd->cxl_resp = n->cxl_resp;

    struct buffer *buffer = &ssd->dram_buffer;
#ifdef LSA_TROLL    
    bool skip = false;
#endif
    bool read = false;
    // FILE *f = fopen("/home/necsst/cxlssd_io.log", "a");
    while (1) {
        if (n->femu_mode == FEMU_CXLSSD_MODE) {
            /* Clean ahead of the foreground path so most evictions find a reusable line.
             * How lazy this is set to is what decides how often a request ends
             * up waiting on a writeback, which is the effect being measured. */
            buffer_writeback_bg(buffer);

            if (ssd->cxl_req && femu_ring_count(ssd->cxl_req)) {
                struct cxl_req *creq = NULL;
                struct ppa ppa;
                struct buffer_entry *bentry;
                lpn_t lpn;
                uint64_t writeback_wait = 0;

                rc = femu_ring_dequeue(ssd->cxl_req, (void *)&creq, 1);
                if (rc != 1) {
                    printf("FEMU: FTL cxl_req dequeue failed\n");
                }
                assert(creq != NULL);

                /* Must work regardless of LSA_TROLL, so this lives outside the switch below */
                if (creq->ncmd->cmd == CXL_TRIM) {
                    int src = creq->trim_src;

                    for (int ti = 0; ti < creq->trim_cnt; ti++) {
                        const struct cylon_trim_ent *e = &creq->trim_ents[ti];

                        for (uint32_t k = 0; k < e->nr_pages; k++)
                            ftl_trim(ssd, e->start_lpn + k);
                        ssd->stats.trim_pages[src] += e->nr_pages;
                    }
                    ssd->stats.trim_cmds[src]++;

                    rc = femu_ring_enqueue(ssd->cxl_resp, (void *)&creq, 1);
                    if (rc != 1) {
                        ftl_err("FTL cxl_resp enqueue failed (TRIM)\n");
                    }
                    continue;
                }

                /* Counters are FTL-thread-owned, so reset/dump run here too */
                if (creq->ncmd->cmd == CXL_STATS_RESET ||
                    creq->ncmd->cmd == CXL_STATS_DUMP) {
                    if (creq->ncmd->cmd == CXL_STATS_RESET)
                        ssd_stats_reset(ssd);
                    else
                        ssd_stats_dump(ssd);

                    rc = femu_ring_enqueue(ssd->cxl_resp, (void *)&creq, 1);
                    if (rc != 1) {
                        ftl_err("FTL cxl_resp enqueue failed (STATS)\n");
                    }
                    continue;
                }
#ifdef LSA_TROLL
                switch (creq->ncmd->cmd) {
                case BUF_PRINT_STAT:
                    skip = true;
                    break;
                case BUF_CLEAR:
                    buffer_clear(buffer);
                    skip = true;
                    break;
                case SSD_INIT:
                    skip = true;
                    break;
                case INC_PREFETCH_DEGREE:
                    n->prefetch_degree = (n->prefetch_degree+1)%4;
                    buffer->degree = n->prefetch_degree;
                    // ssd_init_buffer(n);
                    skip = true;
                    break;
                }

                if (skip) {
                    skip = false;
                    rc = femu_ring_enqueue(ssd->cxl_resp, (void *)&creq, 1);
                    continue;
                }
#endif
                lpn = creq->lpn;
                lat = 0;
                read = (creq->ncmd->cmd == CXL_READ);
                // printf("[CXL]: lpn(0x%lx) %s\n", lpn, read?"READ":"WRITE");

                bentry = buffer_lookup_entry(buffer, lpn);
                if (bentry) {//buffer hit
                    rc = femu_ring_enqueue(ssd->cxl_resp, (void *)&creq, 1);
                    buffer_mark_dirty(buffer, bentry,
                                      (bentry->dirty==false && read)?false:true);

                    if (read)   buffer->read_hit_trapped++;
                    else        buffer->write_hit_trapped++;
                    /* An entry already in the cache is never evicted by its own
                     * reinsertion, so this cannot wait. */
                    buffer_insert_entry(buffer, bentry, INSERT_NO_PREFETCH);
                }
                else {//buffer miss: fetch from NAND
                    bentry = buffer_entry_init(buffer, lpn);
                    buffer_mark_dirty(buffer, bentry, read?false:true);

                    if (read)   buffer->read_miss++;
                    else        buffer->write_miss++;

                    /* Take the line first. If its victim is still dirty, or a
                     * background writeback of it is still in flight, the line is
                     * not reusable yet and the requester waits it out. This has
                     * to happen before the response is enqueued below, otherwise
                     * there is nothing left to charge the wait to. */
                    writeback_wait = buffer_insert_entry(buffer, bentry,
                                                         INSERT_PREFETCH);

                    ppa = get_maptbl_ent(ssd, lpn);
                    if (mapped_ppa(&ppa) && valid_ppa(ssd, &ppa)) {
                        creq->ncmd->cmd = NAND_READ;
                        lat += ssd_advance_status(ssd, &ppa, creq->ncmd);
                        ssd->stats.r_cache_fill++;
                        // backend_memcpy(ssd, ppa, bentry->idx, NAND_TO_BUF);
                    }
                    /* Unmapped LPN (no NAND page backs it yet). The original
                     * Cylon code (disabled below) programmed a NAND page here at
                     * once (first_touch). But that page is then held dirty in the
                     * cache and programmed AGAIN on eviction (writeback): one new
                     * page written to NAND twice, the first copy left as GC
                     * garbage.
                     *
                     * A real DRAM-cached SSD (CMM-H) programs a new page
                     * only once, on eviction. So skip it here -- a write stays
                     * dirty and flush_pg does that single write on eviction (it
                     * handles the unmapped case); a read returns the zero backing
                     * with no NAND. */
                    // else {
                    //     struct ppa new_ppa;
                    //     ftl_assert(valid_lpn(ssd, lpn));
                    //     new_ppa = get_new_page(ssd);
                    //     set_maptbl_ent(ssd, lpn, &new_ppa);
                    //     set_rmap_ent(ssd, lpn, &new_ppa);
                    //     mark_page_valid(ssd, &new_ppa);
                    //     ssd_advance_write_pointer(ssd);
                    //     ssd->stats.w_first_touch++;
                    //     creq->ncmd->cmd = NAND_WRITE;
                    //     lat += ssd_advance_status(ssd, &new_ppa, creq->ncmd);
                    // }
                    /* Hits never touch expire_time, so accumulating only here
                     * keeps the fast path clear and still totals every ns the
                     * guest was made to wait. cxlssd.c spins until expire_time,
                     * so whatever is added here is real guest stall. */
                    creq->expire_time += lat + writeback_wait;
                    ssd->stats.stall_cache_fill_ns += lat;
                    ssd->stats.stall_writeback_ns += writeback_wait;
                    ssd->stats.stall_ns += lat + writeback_wait;
                    rc = femu_ring_enqueue(ssd->cxl_resp, (void *)&creq, 1);
                }
                /* clean one line if needed (in the background) */
                if (should_gc(ssd)) {
                    do_gc(ssd, false);
                }
                /* Background GC may skip low-ipc lines and reclaim nothing, so free lines
                 * can hit 0 and abort. Force-reclaim below the high threshold. */
                while (should_gc_high(ssd)) {
                    /* perform GC here until !should_gc(ssd) */
                    if (do_gc(ssd, true) == -1)
                        break;
                }

                ssd_stats_sample(ssd);
            }
        }

        for (i = 1; i <= n->nr_pollers; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            // if (n->io_logfile)
            //     fprintf(n->io_logfile, "%ld, %s, %lu, %u\n", req->stime, req->is_write?"W":"R", req->slba, req->nlb);

            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                lat = ssd_write(ssd, req);
                break;
            case NVME_CMD_READ:
                lat = ssd_read(ssd, req);
                break;
            case NVME_CMD_DSM:
                lat = 0;
                break;
            default:
                //ftl_err("FTL received unkown request type, ERROR\n");
                ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            if (should_gc(ssd)) {
                do_gc(ssd, false);
            }
        }
    }

    return NULL;
}


 /*
 * Program one dirty page to NAND.
 * Overwriting here is the only way a PPA gets invalidated in Cylon,
 * so this is the only source of GC victim lines (raises line->ipc).
 *
 * Returns how long from now until the program completes, which includes any
 * queueing already on the target LUN because swr.stime = 0 makes
 * ssd_advance_status() measure from the current clock. src only selects a
 * counter; the work done is identical either way, and it is the caller that
 * decides whether to charge that time to a request (foreground eviction) or to
 * record it as a completion time (background writeback).
 */
uint64_t flush_pg(struct ssd* ssd, lpn_t lpn, int src)
{
    struct ppa ppa;
    uint64_t curlat = 0, maxlat = 0;
    struct nand_lun *new_lun;

    ssd->stats.w_writeback[src]++;

    ppa = get_maptbl_ent(ssd, lpn);
    if (mapped_ppa(&ppa)) {
        /* update old page information first */
        mark_page_invalid(ssd, &ppa);
        set_rmap_ent(ssd, INVALID_LPN, &ppa);
    }

    /* new write */
    ppa = get_new_page(ssd);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &ppa);

    mark_page_valid(ssd, &ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd);

    struct nand_cmd swr;
    swr.type = USER_IO;
    swr.cmd = NAND_WRITE;
    swr.stime = 0;
    /* get latency statistics */
    curlat = ssd_advance_status(ssd, &ppa, &swr);
    maxlat = (curlat > maxlat) ? curlat : maxlat;

    new_lun = get_lun(ssd, &ppa);
    new_lun->evict_endtime = new_lun->next_lun_avail_time;

    return maxlat;
}
