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
 * the demand insert's wait is returned; see the note on the prefetch loop.
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
	b->ops.insert_entry(b, bentry, &wait);

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
			b->ops.insert_entry(b, pbentry, &prefetch_wait);
		}
		(void)prefetch_wait;
		// printf(">>> Done\n");
	}

	return wait;
}


/*
 * dirty_cnt has to follow every change to entry->dirty, so route them all
 * through here rather than assigning the flag directly.
 */
void buffer_mark_dirty(struct buffer *b, struct buffer_entry *ent, bool dirty)
{
	if (ent->dirty == dirty)
		return;

	ent->dirty = dirty;
	if (dirty)
		b->dirty_cnt++;
	else if (b->dirty_cnt)
		b->dirty_cnt--;
}

/* Cost of taking this entry's line, before it is unlinked:
 *   dirty     - not in NAND yet, so program it now and wait it out
 *   in-flight - a background program is still landing, wait out its remainder
 *   clean     - free
 */
uint64_t buffer_evict_cost(struct buffer *b, struct buffer_entry *victim)
{
	uint64_t now;

	if (victim->dirty) {
		buffer_mark_dirty(b, victim, false);
		return flush_pg(b->ssd, victim->lpn, WRITEBACK_SRC_DEMAND);
	}

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
}

/* Scan in the order the eviction policy consumes the set, oldest first, so what
 * is cleaned is what is about to be evicted and any still-in-flight line a
 * demand eviction meets has the least time left. */
static void writeback_set(struct buffer *b, struct set *set)
{
	struct buffer_entry *ent;

	if (b->way == WAY_1) {
		ent = set->entry;
		if (ent && ent->dirty)
			writeback_entry(b, ent);
		return;
	}

	QTAILQ_FOREACH(ent, &set->queue, b_entry) {
		if (b->dirty_cnt <= b->dirty_lo)
			return;
		if (ent->dirty)
			writeback_entry(b, ent);
	}
}

/* Start above dirty_hi, run until below dirty_lo, so the band width also caps
 * one pass. Deliberately unthrottled beyond that: a writeback backlog delaying
 * later NAND work is exactly the write cost this is meant to expose. */
void buffer_writeback_bg(struct buffer *b)
{
	uint64_t n_set, scanned;

	if (b->size == 0 || b->dirty_cnt <= b->dirty_hi)
		return;

	n_set = b->set_mask ? b->set_mask : 1;
	for (scanned = 0; scanned < n_set; scanned++) {
		if (b->dirty_cnt <= b->dirty_lo)
			break;
		writeback_set(b, &b->sets[b->wb_cursor]);
		b->wb_cursor = (b->wb_cursor + 1) % n_set;
	}
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
						 WRITEBACK_SRC_DEMAND);
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
					flush_pg(buffer->ssd, ent->lpn, WRITEBACK_SRC_DEMAND);
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

	/* Every entry is gone, so nothing is dirty and the scan restarts. */
	buffer->dirty_cnt = 0;
	buffer->wb_cursor = 0;

    buffer->read_hit = buffer->read_miss = 0; 
    buffer->write_hit = buffer->write_miss = 0;
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