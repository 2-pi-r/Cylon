#include "buffer.h"
#include "ftl.h"
#include "../kvm_ext.h"

// static uint64_t ssd_lpn2hva(struct buffer *b, lpn_t lpn)
// {
// 	return (uint64_t)b->ssdbackend->buf_space + (lpn<<12);
// }

static uint64_t get_gfn(struct buffer *b, lpn_t lpn)
{
	uint64_t base_gpa = b->ssdbackend->base_gpa;
	uint64_t base_gfn = base_gpa >> 12;
	return base_gfn + lpn;
}

void direct_mr_add(struct buffer *b, lpn_t lpn)
{
	// if (ioctl_flag) {
		// printf("update lpn: %ld\n", lpn);
		femu_kvm_spte_clear_mmio_flag(get_gfn(b, lpn), lpn);
	// }
		
}

void direct_mr_del(struct buffer *b, lpn_t lpn)
{
	// if (ioctl_flag) {
		// printf("evict lpn: %ld\n", lpn);
		femu_kvm_spte_set_mmio_flag(get_gfn(b, lpn), lpn);
	// }
		
}

struct buffer_entry *buffer_entry_init(struct buffer *b, lpn_t lpn)
{
    struct buffer_entry *new;
	if (b->policy == S3FIFO) {
		struct buffer_entry target;
		target.lpn = lpn;
		
		//check in ghost queue
		new = g_tree_lookup(b->ghost_tree, &target);
		if(new != NULL){
			return new;
		}
	}

	// char name[64];
    new = malloc(sizeof(struct buffer_entry));
    if (!new){
        femu_err("alloc new buffer_entry failed\n");
        return NULL;
    }
        
    new->lpn = lpn;
    new->dirty = false;
    new->dirtied_by_ept_scan = false;
    new->writeback_endtime = 0;

	return new;
}

bool buffer_full(struct buffer *b)
{
    return (b->size !=0) && (b->size * b->thres_pcent <= b->entry_cnt);
}

struct buffer_entry* buffer_lookup_entry(struct buffer *b, lpn_t lpn)
{
	struct buffer_entry target;
	target.lpn = lpn;

	if (b->size == 0)
		return NULL;

	// if (b->policy == S3FIFO) {
	// 	//check in main and small queue
	// 	struct buffer_entry *ent = g_tree_lookup(b->tree, &target);
	// 	if(ent != NULL){
	// 		return ent;
	// 	}
	// 	//check in ghost queue
	// 	ent = g_tree_lookup(b->ghost_tree, &target);
	// 	if(ent != NULL){
	// 		return ent;
	// 	}
	// 	return NULL;
	// }

	return g_tree_lookup(b->tree, &target);
}

static inline int get_set_idx(struct buffer *b, lpn_t lpn)
{
	return lpn % (b->set_mask);
}

struct set* buffer_get_set(struct buffer *b, lpn_t lpn)
{
	return &b->sets[get_set_idx(b,lpn)];
}

// bool buffer_hit(struct buffer *b, lpn_t lpn)
//  {
// 	//Buffer lookup for 'read'
//     struct buffer_entry *buffer_entry = NULL;
// 	struct buffer_entry target;
// 	target.lpn = lpn;

// 	// return false;

// 	if (b->size == 0)
// 		return false;

// 	assert(b->tree);
// 	buffer_entry = g_tree_lookup(b->tree, &target);
// 	if(buffer_entry == NULL){
// 		return false;
// 	}
// 	else{
// 		return true;
// 	}
// }

// struct buffer_entry* buffer_select_victim(struct buffer *b)
// {
// 	if (b->size == 0)
// 		return NULL;

// 	return b->ops.select_victim(b);
// }

// bool buffer_evict_victim(struct buffer *b, struct buffer_entry *victim_entry)
// {
// 	if (!victim_entry)
// 		return false;
	
// 	// bitmap_clear(b->bitmap, victim_entry->idx, 1);
// 	QTAILQ_REMOVE(&b->queue, victim_entry, b_entry);	//remove from queue
// 	g_tree_remove(b->tree, victim_entry);	//remove from avl tree

// 	direct_mr_del(b, victim_entry->lpn);

// 	free(victim_entry);
// 	b->entry_cnt--;

// 	return true;
// }

// bool buffer_force_eviction(struct buffer *b)
// {
// 	return (b->size !=0) && (b->entry_cnt > (b->size * b->force_pcent));
// }

/*
 * Returns how long the requester must wait for the line this insert took. Only
 * the foreground insert's wait is returned; see the note on the prefetch loop.
 */
uint64_t buffer_insert_entry(struct buffer *b, struct buffer_entry *bentry, int prefetch)
{
	assert(bentry != NULL);
	uint64_t wait = 0;
	lpn_t lpn;
	uint64_t start_lpn = bentry->lpn + b->stride;
	uint64_t end_lpn = start_lpn + b->degree;
	struct buffer_entry *pbentry = NULL;

	if (b->size == 0) {
		// free(b);
		return 0;
	}

	// return false;
	// printf("[FEMU CXL] Insert entry: lpn: %lx\n", bentry->lpn);
	if (b->ops.insert_entry(b, bentry, &wait) > 0)
		bentry->insert_cnt_when_inserted = ++b->insert_cnt;

	/* prefetch */
	/* TODO: prefetch not considered. */
	if (prefetch == INSERT_PREFETCH) {
		uint64_t prefetch_wait = 0;	/* discarded, see TODO above */

		// printf("[FEMU CXL] Prefetch start: lpn: %lx, prefetch: %lx - %lx\n",(*bentry)->lpn, start_lpn, end_lpn-1);
		for(lpn = start_lpn; lpn < end_lpn; lpn++){
			if (!valid_user_lpn(b->ssd, lpn))
				continue;

			pbentry = buffer_lookup_entry(b, lpn);
			if (!pbentry) {
				pbentry = buffer_entry_init(b, lpn);
			}
			if (b->ops.insert_entry(b, pbentry, &prefetch_wait) > 0)
				pbentry->insert_cnt_when_inserted = ++b->insert_cnt;
		}
		(void)prefetch_wait;
		// printf(">>> Done\n");
	}

	return wait;
}


int buffer_comp_insert_order(const void *a, const void *b)
{
	uint64_t x = ((const struct buffer_entry *)a)->insert_cnt_when_inserted;
	uint64_t y = ((const struct buffer_entry *)b)->insert_cnt_when_inserted;

	return (x > y) - (x < y);
}

static gboolean take_first_entry(gpointer key, gpointer value, gpointer first)
{
	*(struct buffer_entry **)first = value;
	return TRUE;	/* stop at the first, the smallest key */
}

/*
 * dirty_cnt and dirty_tree have to follow every change to entry->dirty, so
 * route them all through here rather than assigning the flag directly. A line
 * dirtied again keeps its insert_cnt_when_inserted, so it goes back to its
 * eviction-order place. The entry must already be inserted: that sets its key.
 */
void buffer_mark_dirty(struct buffer *b, struct buffer_entry *ent, bool dirty)
{
	uint64_t start;

	if (ent->dirty == dirty)
		return;

	ent->dirty = dirty;
	start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
	if (dirty) {
		b->dirty_cnt++;
		g_tree_insert(b->dirty_tree, ent, ent);
		if (!b->dirty_tree_first ||
		    buffer_comp_insert_order(ent, b->dirty_tree_first) < 0)
			b->dirty_tree_first = ent;
	} else {
		ent->dirtied_by_ept_scan = false;
		g_tree_remove(b->dirty_tree, ent);
		if (b->dirty_tree_first == ent) {
			b->dirty_tree_first = NULL;
			g_tree_foreach(b->dirty_tree, take_first_entry,
				       &b->dirty_tree_first);
		}
		if (b->dirty_cnt)
			b->dirty_cnt--;
	}
	b->ssd->stats.dirty_index_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - start;
}

/* Cost of taking this entry's line, before it is unlinked:
 *   dirty     - not in NAND yet, so program it now and wait it out
 *   in-flight - a background program is still landing, wait out its remainder
 *   clean     - free
 */
uint64_t buffer_evict_cost(struct buffer *b, struct buffer_entry *victim)
{
	uint64_t now;

	/* Taking hit writes from the EPT at eviction, left off on purpose: first
	 * seen here, the line is always programmed in the foreground, while a real
	 * device sees hit writes as they happen and writes it back in the
	 * background. buffer_scan_ept_dirty() takes them before eviction instead. */
	// if (!victim->dirty && femu_kvm_spte_is_dirty(victim->lpn))
	// 	buffer_mark_dirty(b, victim, true);

	if (victim->dirty) {
		if (victim->dirtied_by_ept_scan)
			b->ssd->stats.w_writeback_fg_by_ept_scan++;
		buffer_mark_dirty(b, victim, false);
		return flush_pg(b->ssd, victim->lpn, WRITEBACK_SRC_FOREGROUND);
	}

	/* Counted only, for the reason above: hit writes the scan had not reached
	 * yet, so they are never written back. */
	if (femu_kvm_spte_is_dirty(victim->lpn))
		b->ssd->stats.ept_dirty_missed_at_evict++;

	now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
	if (victim->writeback_endtime > now) {
		b->ssd->stats.evict_inflight_waits++;
		return victim->writeback_endtime - now;
	}

	return 0;
}

/* Write one dirty entry out but leave it cached, so the line turns clean in
 * place instead of being evicted. It is not reusable until the program lands,
 * which writeback_endtime records for buffer_evict_cost(). */
static void writeback_entry(struct buffer *b, struct buffer_entry *ent)
{
	uint64_t lat = flush_pg(b->ssd, ent->lpn, WRITEBACK_SRC_BACKGROUND);

	buffer_mark_dirty(b, ent, false);
	ent->writeback_endtime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + lat;
	/* The program just issued holds the hit writes recorded so far; left set,
	 * the next EPT scan would dirty the line again for nothing. Some later hit
	 * writes may go unrecorded, see femu_kvm_spte_clear_dirty(). */
	femu_kvm_spte_clear_dirty(ent->lpn);
}

/* Age = lines inserted since this line was, i.e. how far it has moved toward
 * eviction under FIFO. */
static bool is_dirty_line_too_old(struct buffer *b, struct buffer_entry *ent)
{
	return b->age_limit &&
	       b->insert_cnt - ent->insert_cnt_when_inserted >= b->age_limit;
}

/* Two triggers, as in the kernel flusher: by watermark, start above high and
 * run until below low; by age, write every line that reached age_limit,
 * stopping at the first that has not, as move_expired_inodes() does.
 * Both take the dirty line nearest eviction first, and each program waits for
 * room on its die -- see writeback_die_has_room(). */
void buffer_writeback_bg(struct buffer *b)
{
	struct buffer_entry *ent;
	uint64_t now;
	bool over_high_watermark;

	if (b->size == 0)
		return;

	ent = b->dirty_tree_first;
	over_high_watermark = b->dirty_cnt > b->dirty_lines_high;
	if (!ent || (!over_high_watermark && !is_dirty_line_too_old(b, ent))) {
		b->writeback_blocked_since = 0;
		return;
	}

	now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
	if (!writeback_die_has_room(b->ssd, now)) {
		/* Something is due but the die is full: time the refusal once per
		 * blocked span, see stats.writeback_bg_blocked_ns. */
		if (!b->writeback_blocked_since)
			b->writeback_blocked_since = now;
		return;
	}
	if (b->writeback_blocked_since) {
		b->ssd->stats.writeback_bg_blocked_ns +=
			now - b->writeback_blocked_since;
		b->writeback_blocked_since = 0;
	}

	while ((ent = b->dirty_tree_first)) {
		bool by_watermark = over_high_watermark &&
				    b->dirty_cnt > b->dirty_lines_low;

		if (!by_watermark && !is_dirty_line_too_old(b, ent))
			break;
		if (!writeback_die_has_room(b->ssd,
					    qemu_clock_get_ns(QEMU_CLOCK_REALTIME)))
			break;
		if (!by_watermark)
			b->ssd->stats.w_writeback_bg_by_age++;
		writeback_entry(b, ent);
	}
}

/* Mark dirty the cached lines the guest wrote through a cache hit.
 * Such writes never trap, so the EPT Dirty flag set by hardware is their only
 * trace. Checks a batch of LPNs per call so background writeback can take the
 * lines before eviction. Reads only; writeback_entry() clears the flag. */
void buffer_scan_ept_dirty(struct buffer *b)
{
	struct buffer_entry *ent;
	lpn_t lpn;
	uint64_t i;

	if (b->size == 0 || b->ept_dirty_scan_batch == 0)
		return;

	b->ept_dirty_scan_last_start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
	for (i = 0; i < b->ept_dirty_scan_batch; i++) {
		lpn = b->ept_dirty_scan_next_lpn;
		if (++b->ept_dirty_scan_next_lpn == b->ept_dirty_scan_lpn_cnt) {
			b->ept_dirty_scan_next_lpn = 0;
			b->ssd->stats.ept_dirty_scan_full_sweep_cnt++;
		}

		if (!femu_kvm_spte_is_dirty(lpn))
			continue;
		ent = buffer_lookup_entry(b, lpn);
		if (ent && !ent->dirty) {
			buffer_mark_dirty(b, ent, true);
			ent->dirtied_by_ept_scan = true;
			b->ssd->stats.ept_dirty_found_by_scan++;
		}
	}
	b->ept_dirty_scan_last_end_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

/*
 * Remove the entry for this LPN from the cache, if present (TRIM only).
 * If left in place, the next access would be treated as a cache hit (zero
 * latency), and a later eviction would remap the LPN via flush_pg(),
 * effectively undoing the TRIM.
 */
bool buffer_remove_entry(struct buffer *b, lpn_t lpn)
{
	struct buffer_entry *ent;

	if (b->size == 0 || !b->ops.remove_entry)
		return false;

	ent = buffer_lookup_entry(b, lpn);
	if (!ent)
		return false;

	return b->ops.remove_entry(b, ent) > 0;
}


static int comp_buffer(const void *a, const void *b){
	return ((struct buffer_entry*)a)->lpn - ((struct buffer_entry*)b)->lpn;
}

void buffer_clear(struct buffer *buffer)
{
	int n_way = 1 << buffer->way;
    uint64_t n_set;
    if (buffer->way == WAY_FULL) {
        n_set = 1;
    }
    else {
        n_set = buffer->size / n_way;
    }
    buffer->set_mask = n_set;
	
    
    if (buffer->way == WAY_1) {
        for (int i = 0; i < n_set; i++) {
			if (buffer->sets[i].entry) {
				g_tree_remove(buffer->tree, buffer->sets[i].entry);
				if (buffer->sets[i].entry->dirty)
					flush_pg(buffer->ssd, buffer->sets[i].entry->lpn,
						 WRITEBACK_SRC_FOREGROUND);
				direct_mr_del(buffer, buffer->sets[i].entry->lpn);
				free(buffer->sets[i].entry);
			}
				
			buffer->sets[i].cnt = 0;
			buffer->sets[i].hand = NULL;
		}
    }
	else {
		for (int i = 0; i < n_set; i++) {
			struct set *set = &buffer->sets[i];
			while(!QTAILQ_EMPTY(&set->queue)) {
				struct buffer_entry *ent = QTAILQ_FIRST(&set->queue);
				QTAILQ_REMOVE(&set->queue, ent, b_entry);
				
				if (ent->dirty)
					flush_pg(buffer->ssd, ent->lpn, WRITEBACK_SRC_FOREGROUND);
				g_tree_remove(buffer->tree, ent);	//remove from avl tree
				direct_mr_del(buffer, ent->lpn);

				free(ent);
			}
			buffer->sets[i].cnt = 0;
			buffer->sets[i].hand = NULL;
			QTAILQ_INIT(&set->queue);
			QTAILQ_INIT(&set->tmp);
		}
	}

	
	buffer->tree = g_tree_new(comp_buffer);
	// b->bitmap = bitmap_new(b->size);

	/* Every entry is gone, so nothing is dirty. The entries were freed without
	 * buffer_mark_dirty(), so the tree is rebuilt here rather than emptied. */
	buffer->dirty_cnt = 0;
	g_tree_destroy(buffer->dirty_tree);
	buffer->dirty_tree = g_tree_new(buffer_comp_insert_order);
	buffer->dirty_tree_first = NULL;
	buffer->writeback_blocked_since = 0;

    buffer->load_hit_trapped = buffer->load_miss = 0;
    buffer->store_hit_trapped = buffer->store_miss = 0;
	buffer->entry_cnt = 0;

	buffer->ins_cnt = 0;
	buffer->evict_cnt = 0;
}

void buffer_init_set(struct buffer* buffer)
{
    int n_way = 1 << buffer->way;
    uint64_t n_set;
    if (buffer->way == WAY_FULL) {
		fprintf(stderr, "Fully-associative\n");
        n_set = 1;
    }
    else {
		fprintf(stderr, "Set-associative: %d-way\n", n_way);
        n_set = buffer->size / n_way;
    }
    buffer->set_mask = n_set;
	printf("Buffer size: %ld, n_set: %ld, n_way: %d, set_mask: 0x%lx\n", buffer->size, n_set, n_way, buffer->set_mask);
    
    buffer->sets = (struct set*)malloc(sizeof(struct set) * n_set);
    
	for (int i = 0; i < n_set; i++) {
		if (buffer->way == WAY_1) {
			buffer->sets[i].entry = NULL;
    	}
		else {
			QTAILQ_INIT(&buffer->sets[i].queue);

			if (buffer->policy == LIFO) {
				QTAILQ_INIT(&buffer->sets[i].tmp);
			}
			
			else if (buffer->policy == S3FIFO) {
				QTAILQ_INIT(&buffer->sets[i].small);
				QTAILQ_INIT(&buffer->sets[i].ghost);
				// buffer->sets[i].size_small = n_way / 4; // 25% for small queue
				// buffer->sets[i].size_main = n_way - buffer->sets[i].size_small; // 75% for main queue
				// buffer->sets[i].size_ghost = n_way; // 100% for ghost queue

				buffer->sets[i].cnt_small = 0;
				buffer->sets[i].cnt_main = 0;
				buffer->sets[i].cnt_ghost = 0;
			}
		}

		buffer->sets[i].cnt = 0;
		buffer->sets[i].hand = NULL;
	}


    printf("Initializing buffer done. WAY: %d, n_set: %ld\n", n_way, n_set);
    
}

void buffer_destroy_set(struct buffer *buffer)
{
	int n_way = 1 << buffer->way;
    uint64_t n_set;
    if (buffer->way == WAY_FULL) {
        n_set = 1;
    }
    else {
        n_set = buffer->size / n_way;
    }
    buffer->set_mask = n_set;

    free(buffer->sets);
    printf("Destroying buffer done. WAY: %d, n_set: %ld\n", n_way, n_set);
}