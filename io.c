// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "dma.h"

#if (SUPPORTED_SSD_TYPE(CONV) || SUPPORTED_SSD_TYPE(ZNS))
#include "ssd.h"
#else
struct buffer;
#endif

#undef PERF_DEBUG

#define sq_entry(entry_id) sq->sq[SQ_ENTRY_TO_PAGE_NUM(entry_id)][SQ_ENTRY_TO_PAGE_OFFSET(entry_id)]
#define cq_entry(entry_id) cq->cq[CQ_ENTRY_TO_PAGE_NUM(entry_id)][CQ_ENTRY_TO_PAGE_OFFSET(entry_id)]

extern bool io_using_dma;

static inline unsigned int __get_io_worker(struct nvmev_dispatcher_ctx *disp, int sqid)
{
#ifdef CONFIG_NVMEV_IO_WORKER_BY_SQ
	return nvmev_io_worker_id_for_queue(disp, sqid);
#else
	return disp->first_worker_id + disp->io_worker_turn;
#endif
}

static inline unsigned long long __get_wallclock(unsigned int cpu_nr)
{
	return cpu_clock(cpu_nr);
}

static inline size_t __cmd_io_offset(struct nvme_rw_command *cmd)
{
	return (cmd->slba) << LBA_BITS;
}

static inline size_t __cmd_io_size(struct nvme_rw_command *cmd)
{
	return (cmd->length + 1) << LBA_BITS;
}

static unsigned int __do_perform_io(int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	struct nvme_rw_command *cmd = &sq_entry(sq_entry).rw;
	size_t offset;
	size_t length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	u64 paddr;
	u64 *paddr_list = NULL;
	size_t nsid = cmd->nsid - 1; // 0-based
	bool is_paddr_memremap = false;

	offset = __cmd_io_offset(cmd);
	length = __cmd_io_size(cmd);
	remaining = length;

	while (remaining) {
		size_t io_size;
		void *vaddr;
		size_t mem_offs = 0;
		bool is_vaddr_memremap = false;

		prp_offs++;
		if (prp_offs == 1) {
			paddr = cmd->prp1;
		} else if (prp_offs == 2) {
			paddr = cmd->prp2;
			if (remaining > PAGE_SIZE) {
				if (pfn_valid(paddr >> PAGE_SHIFT)) {
					paddr_list = kmap_atomic_pfn(PRP_PFN(paddr)) +
						(paddr & PAGE_OFFSET_MASK);
				} else {
					paddr_list = memremap(paddr, PAGE_SIZE, MEMREMAP_WT);
					paddr_list += (paddr & PAGE_OFFSET_MASK);
					is_paddr_memremap = true;
				}
				paddr = paddr_list[prp2_offs++];
			}
		} else {
			paddr = paddr_list[prp2_offs++];
		}

		io_size = min_t(size_t, remaining, PAGE_SIZE);

		if (paddr & PAGE_OFFSET_MASK) {
			mem_offs = paddr & PAGE_OFFSET_MASK;
			if (io_size + mem_offs > PAGE_SIZE)
				io_size = PAGE_SIZE - mem_offs;
		}

		if (pfn_valid(paddr >> PAGE_SHIFT)) {
			vaddr = kmap_atomic_pfn(PRP_PFN(paddr));
		} else {
			vaddr = memremap(paddr, PAGE_SIZE, MEMREMAP_WT);
			is_vaddr_memremap = true;
		}

		if (cmd->opcode == nvme_cmd_write ||
		    cmd->opcode == nvme_cmd_zone_append) {
			memcpy(nvmev_vdev->ns[nsid].mapped + offset, vaddr + mem_offs, io_size);
		} else if (cmd->opcode == nvme_cmd_read) {
			memcpy(vaddr + mem_offs, nvmev_vdev->ns[nsid].mapped + offset, io_size);
		}

		if (vaddr != NULL && !is_vaddr_memremap) {
			kunmap_atomic(vaddr);
			vaddr = NULL;
		} else if (vaddr != NULL && is_vaddr_memremap) {
			memunmap(vaddr);
			vaddr = NULL;
			is_vaddr_memremap = false;
		}

		remaining -= io_size;
		offset += io_size;
	}

	if (paddr_list) {
		if (!is_paddr_memremap)
			kunmap_atomic(paddr_list);
		else
			memunmap(paddr_list);
	}
	paddr_list = NULL;

	return length;
}

static u64 paddr_list[513] = {
	0,
}; // Not using index 0 to make max index == num_prp
static unsigned int __do_perform_io_using_dma(int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	struct nvme_rw_command *cmd = &sq_entry(sq_entry).rw;
	size_t offset;
	size_t length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	int num_prps = 0;
	u64 paddr;
	u64 *tmp_paddr_list = NULL;
	size_t io_size;
	size_t mem_offs = 0;
	bool is_memremap = false;

	offset = __cmd_io_offset(cmd);
	length = __cmd_io_size(cmd);
	remaining = length;

	memset(paddr_list, 0, sizeof(paddr_list));
	/* Loop to get the PRP list */
	while (remaining) {
		io_size = 0;

		prp_offs++;
		if (prp_offs == 1) {
			paddr_list[prp_offs] = cmd->prp1;
		} else if (prp_offs == 2) {
			paddr_list[prp_offs] = cmd->prp2;
			if (remaining > PAGE_SIZE) {
				if (pfn_valid(paddr_list[prp_offs] >> PAGE_SHIFT)) {
					tmp_paddr_list =
						kmap_atomic_pfn(PRP_PFN(paddr_list[prp_offs])) +
						(paddr_list[prp_offs] & PAGE_OFFSET_MASK);
				} else {
					tmp_paddr_list = memremap(paddr_list[prp_offs], PAGE_SIZE,
								 MEMREMAP_WT);
					tmp_paddr_list +=
						(paddr_list[prp_offs] & PAGE_OFFSET_MASK);
					is_memremap = true;
				}
				paddr_list[prp_offs] = tmp_paddr_list[prp2_offs++];
			}
		} else {
			paddr_list[prp_offs] = tmp_paddr_list[prp2_offs++];
		}

		io_size = min_t(size_t, remaining, PAGE_SIZE);

		if (paddr_list[prp_offs] & PAGE_OFFSET_MASK) {
			mem_offs = paddr_list[prp_offs] & PAGE_OFFSET_MASK;
			if (io_size + mem_offs > PAGE_SIZE)
				io_size = PAGE_SIZE - mem_offs;
		}

		remaining -= io_size;
	}
	num_prps = prp_offs;

	if (tmp_paddr_list != NULL && !is_memremap) {
		kunmap_atomic(tmp_paddr_list);
	} else if (tmp_paddr_list != NULL && is_memremap) {
		memunmap(tmp_paddr_list);
		is_memremap = false;
	}

	remaining = length;
	prp_offs = 1;

	/* Loop for data transfer */
	while (remaining) {
		size_t page_size;
		mem_offs = 0;
		io_size = 0;
		page_size = 0;

		paddr = paddr_list[prp_offs];
		page_size = min_t(size_t, remaining, PAGE_SIZE);

		/* For non-page aligned paddr, it will never be between continuous PRP list (Always first paddr)  */
		if (paddr & PAGE_OFFSET_MASK) {
			mem_offs = paddr & PAGE_OFFSET_MASK;
			if (page_size + mem_offs > PAGE_SIZE) {
				page_size = PAGE_SIZE - mem_offs;
			}
		}

		for (prp_offs++; prp_offs <= num_prps; prp_offs++) {
			if (paddr_list[prp_offs] == paddr_list[prp_offs - 1] + PAGE_SIZE)
				page_size += PAGE_SIZE;
			else
				break;
		}

		io_size = min_t(size_t, remaining, page_size);

		if (cmd->opcode == nvme_cmd_write ||
		    cmd->opcode == nvme_cmd_zone_append) {
			ioat_dma_submit(paddr, nvmev_vdev->config.storage_start + offset, io_size);
		} else if (cmd->opcode == nvme_cmd_read) {
			ioat_dma_submit(nvmev_vdev->config.storage_start + offset, paddr, io_size);
		}

		remaining -= io_size;
		offset += io_size;
	}

	return length;
}

static void __insert_req_sorted(unsigned int entry, struct nvmev_io_worker *worker,
				unsigned long nsecs_target)
{
	/**
	 * Requests are placed in @work_queue sorted by their target time.
	 * @work_queue is statically allocated and the ordered list is
	 * implemented by chaining the indexes of entries with @prev and @next.
	 * This implementation is nasty but we do this way over dynamically
	 * allocated linked list to minimize the influence of dynamic memory allocation.
	 * Also, this O(n) implementation can be improved to O(logn) scheme with
	 * e.g., red-black tree but....
	 */
	if (worker->io_seq == -1) {
		worker->io_seq = entry;
		worker->io_seq_end = entry;
	} else {
		unsigned int curr = worker->io_seq_end;

		while (curr != -1) {
			if (worker->work_queue[curr].nsecs_target <= worker->latest_nsecs)
				break;

			if (worker->work_queue[curr].nsecs_target <= nsecs_target)
				break;

			curr = worker->work_queue[curr].prev;
		}

		if (curr == -1) { /* Head inserted */
			worker->work_queue[worker->io_seq].prev = entry;
			worker->work_queue[entry].next = worker->io_seq;
			worker->io_seq = entry;
		} else if (worker->work_queue[curr].next == -1) { /* Tail */
			worker->work_queue[entry].prev = curr;
			worker->io_seq_end = entry;
			worker->work_queue[curr].next = entry;
		} else { /* In between */
			worker->work_queue[entry].prev = curr;
			worker->work_queue[entry].next = worker->work_queue[curr].next;

			worker->work_queue[worker->work_queue[entry].next].prev = entry;
			worker->work_queue[curr].next = entry;
		}
	}
}

static struct nvmev_io_worker *__allocate_work_queue_entry(
		struct nvmev_dispatcher_ctx *disp, int sqid, unsigned int *entry)
{
	unsigned int io_worker_turn = __get_io_worker(disp, sqid);
	struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[io_worker_turn];
	unsigned int e = worker->free_seq;
	struct nvmev_io_work *w = worker->work_queue + e;

	if (w->next >= NR_MAX_PARALLEL_IO) {
		if (unlikely(debug))
			disp->profile.alloc_reclaim_failures++;
		printk_ratelimited(KERN_WARNING
				   "%s: worker %u work queue is full for sqid %d, throttling dispatcher\n",
				   NVMEV_DRV_NAME, worker->id, sqid);
		return NULL;
	}

#ifndef CONFIG_NVMEV_IO_WORKER_BY_SQ
	if (++disp->io_worker_turn >= disp->nr_workers)
		disp->io_worker_turn = 0;
#endif

	worker->free_seq = w->next;
	BUG_ON(worker->free_seq >= NR_MAX_PARALLEL_IO);
	*entry = e;

	return worker;
}

static bool __enqueue_io_req(struct nvmev_dispatcher_ctx *disp, int sqid, int cqid,
			     int sq_entry, unsigned long long nsecs_start,
			     struct nvmev_result *ret)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	struct nvmev_io_worker *worker;
	struct nvmev_io_work *w;
	unsigned int entry;

	worker = __allocate_work_queue_entry(disp, sqid, &entry);
	if (!worker)
		return false;

	w = worker->work_queue + entry;

	NVMEV_DEBUG_VERBOSE("%s/%u[%d], sq %d cq %d, entry %d, %llu + %llu\n", worker->thread_name, entry,
		    sq_entry(sq_entry).rw.opcode, sqid, cqid, sq_entry, nsecs_start,
		    ret->nsecs_target - nsecs_start);

	/////////////////////////////////
	w->sqid = sqid;
	w->cqid = cqid;
	w->sq_entry = sq_entry;
	w->command_id = sq_entry(sq_entry).common.command_id;
	w->nsecs_start = nsecs_start;
	w->nsecs_enqueue = local_clock();
	w->nsecs_target = ret->nsecs_target;
	w->status = ret->status;
	w->is_completed = false;
	w->is_copied = false;
	w->prev = -1;
	w->next = -1;

	w->is_internal = false;
	mb(); /* IO worker shall see the updated w at once */

	if (unlikely(debug))
		disp->profile.sorted_enqueues++;
	__insert_req_sorted(entry, worker, ret->nsecs_target);

	return true;
}

void schedule_internal_operation(int sqid, unsigned long long nsecs_target,
				 struct buffer *write_buffer, size_t buffs_to_release)
{
	unsigned int disp_id = nvmev_dispatcher_id_for_sq(nvmev_vdev->nr_dispatchers, sqid);
	struct nvmev_dispatcher_ctx *disp = &nvmev_vdev->dispatchers[disp_id];
	struct nvmev_io_worker *worker;
	struct nvmev_io_work *w;
	unsigned int entry;

	worker = __allocate_work_queue_entry(disp, sqid, &entry);
	if (!worker)
		return;

	w = worker->work_queue + entry;

	NVMEV_DEBUG_VERBOSE("%s/%u, internal sq %d, %llu + %llu\n", worker->thread_name, entry, sqid,
		    local_clock(), nsecs_target - local_clock());

	/////////////////////////////////
	w->sqid = sqid;
	w->nsecs_start = w->nsecs_enqueue = local_clock();
	w->nsecs_target = nsecs_target;
	w->is_completed = false;
	w->is_copied = true;
	w->prev = -1;
	w->next = -1;

	w->is_internal = true;
	w->write_buffer = write_buffer;
	w->buffs_to_release = buffs_to_release;
	mb(); /* IO worker shall see the updated w at once */

	__insert_req_sorted(entry, worker, nsecs_target);
}

static void __reclaim_completed_reqs(struct nvmev_dispatcher_ctx *disp)
{
	unsigned int turn;
	unsigned int first_wid = disp->first_worker_id;
	unsigned int last_wid = first_wid + disp->nr_workers;
	unsigned int total_reclaimed = 0;
	unsigned long long reclaim_start = 0;

	if (unlikely(debug))
		reclaim_start = local_clock();

	for (turn = first_wid; turn < last_wid; turn++) {
		struct nvmev_io_worker *worker;
		struct nvmev_io_work *w;

		unsigned int first_entry = -1;
		unsigned int last_entry = -1;
		unsigned int curr;
		int nr_reclaimed = 0;

		worker = &nvmev_vdev->io_workers[turn];

		first_entry = worker->io_seq;
		curr = first_entry;

		while (curr != -1) {
			w = &worker->work_queue[curr];
			if (w->is_completed == true && w->is_copied == true &&
			    w->nsecs_target <= worker->latest_nsecs) {
				last_entry = curr;
				curr = w->next;
				nr_reclaimed++;
			} else {
				break;
			}
		}

			if (last_entry != -1) {
				w = &worker->work_queue[last_entry];
				worker->io_seq = w->next;
				if (w->next != -1) {
					worker->work_queue[w->next].prev = -1;
				}
				w->next = -1;

				w = &worker->work_queue[first_entry];
				w->prev = worker->free_seq_end;

				w = &worker->work_queue[worker->free_seq_end];
				w->next = first_entry;

				worker->free_seq_end = last_entry;
				total_reclaimed += nr_reclaimed;
				NVMEV_DEBUG_VERBOSE("%s: %u -- %u, %d\n", __func__,
						    first_entry, last_entry, nr_reclaimed);
			}
		}

	if (unlikely(debug)) {
		disp->profile.reclaim_calls++;
		disp->profile.reclaim_ns += local_clock() - reclaim_start;
		disp->profile.reclaimed_entries += total_reclaimed;
	}
}

static void __enqueue_pending_cq(struct nvmev_io_worker *worker, unsigned int cqid)
{
	unsigned long flags;

	if (unlikely(cqid > NR_MAX_IO_QUEUE))
		return;

	spin_lock_irqsave(&worker->pending_cq_lock, flags);

	if (worker->pending_cq_enqueued[cqid]) {
		spin_unlock_irqrestore(&worker->pending_cq_lock, flags);
		return;
	}

	worker->pending_cq_enqueued[cqid] = true;
	worker->pending_cq_next[cqid] = -1;

	if (worker->pending_cq_tail == -1) {
		worker->pending_cq_head = cqid;
		worker->pending_cq_tail = cqid;
	} else {
		worker->pending_cq_next[worker->pending_cq_tail] = cqid;
		worker->pending_cq_tail = cqid;
	}

	spin_unlock_irqrestore(&worker->pending_cq_lock, flags);
}

static int __dequeue_pending_cq(struct nvmev_io_worker *worker)
{
	unsigned long flags;
	unsigned int cqid;

	spin_lock_irqsave(&worker->pending_cq_lock, flags);

	cqid = worker->pending_cq_head;
	if (cqid == -1) {
		spin_unlock_irqrestore(&worker->pending_cq_lock, flags);
		return -1;
	}

	worker->pending_cq_head = worker->pending_cq_next[cqid];
	if (worker->pending_cq_head == -1)
		worker->pending_cq_tail = -1;

	worker->pending_cq_next[cqid] = -1;
	worker->pending_cq_enqueued[cqid] = false;

	spin_unlock_irqrestore(&worker->pending_cq_lock, flags);

	return cqid;
}

static size_t __nvmev_proc_io(struct nvmev_dispatcher_ctx *disp, int sqid, int sq_entry,
			      size_t *io_size)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	unsigned long long nsecs_start = __get_wallclock(disp->cpu_nr);
	unsigned long long prof_start = 0;
	struct nvme_command *cmd = &sq_entry(sq_entry);
#if (BASE_SSD == KV_PROTOTYPE)
	uint32_t nsid = 0;
#else
	uint32_t nsid = cmd->common.nsid - 1;
#endif
	struct nvmev_ns *ns = &nvmev_vdev->ns[nsid];

	struct nvmev_request req = {
		.cmd = cmd,
		.sq_id = sqid,
		.nsecs_start = nsecs_start,
	};
	struct nvmev_result ret = {
		.nsecs_target = nsecs_start,
		.status = NVME_SC_SUCCESS,
	};

#ifdef PERF_DEBUG
	unsigned long long prev_clock = local_clock();
	unsigned long long prev_clock2 = 0;
	unsigned long long prev_clock3 = 0;
	unsigned long long prev_clock4 = 0;
	static unsigned long long clock1 = 0;
	static unsigned long long clock2 = 0;
	static unsigned long long clock3 = 0;
	static unsigned long long counter = 0;
#endif

	if (unlikely(debug))
		prof_start = local_clock();

	if (!ns->proc_io_cmd(ns, &req, &ret))
		return false;
	if (unlikely(debug))
		disp->profile.proc_io_ns += local_clock() - prof_start;
	*io_size = __cmd_io_size(&sq_entry(sq_entry).rw);

#ifdef PERF_DEBUG
	prev_clock2 = local_clock();
#endif

	if (unlikely(debug))
		prof_start = local_clock();
	if (!__enqueue_io_req(disp, sqid, sq->cqid, sq_entry, nsecs_start, &ret)) {
		if (unlikely(debug))
			disp->profile.alloc_reclaim_calls++;
		__reclaim_completed_reqs(disp);
		return false;
	}
	if (unlikely(debug))
		disp->profile.enqueue_ns += local_clock() - prof_start;

#ifdef PERF_DEBUG
	prev_clock3 = local_clock();
#endif

	/* Reclaim after a successful enqueue so the dispatcher can keep feeding workers. */
	__reclaim_completed_reqs(disp);

#ifdef PERF_DEBUG
	prev_clock4 = local_clock();

	clock1 += (prev_clock2 - prev_clock);
	clock2 += (prev_clock3 - prev_clock2);
	clock3 += (prev_clock4 - prev_clock3);
	counter++;

	if (counter > 1000) {
		NVMEV_DEBUG("LAT: %llu, ENQ: %llu, CLN: %llu\n", clock1 / counter, clock2 / counter,
			    clock3 / counter);
		clock1 = 0;
		clock2 = 0;
		clock3 = 0;
		counter = 0;
	}
#endif
	return true;
}

int nvmev_proc_io_sq(struct nvmev_dispatcher_ctx *disp_ctx, int sqid, int new_db, int old_db)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	int num_proc = new_db - old_db;
	int seq;
	int sq_entry = old_db;
	int latest_db;

	if (unlikely(!sq))
		return old_db;
	if (unlikely(num_proc < 0))
		num_proc += sq->queue_size;

	if (unlikely(debug))
		disp_ctx->profile.sq_batches++;

	for (seq = 0; seq < num_proc; seq++) {
		size_t io_size;
		if (!__nvmev_proc_io(disp_ctx, sqid, sq_entry, &io_size))
			break;

		if (++sq_entry == sq->queue_size) {
			sq_entry = 0;
		}
		sq->stat.nr_dispatched++;
		sq->stat.nr_in_flight++;
		sq->stat.total_io += io_size;
	}
	if (unlikely(debug))
		disp_ctx->profile.sq_entries += seq;
	sq->stat.nr_dispatch++;
	sq->stat.max_nr_in_flight = max_t(int, sq->stat.max_nr_in_flight, sq->stat.nr_in_flight);

	latest_db = (old_db + seq) % sq->queue_size;
	return latest_db;
}

void nvmev_proc_io_cq(int cqid, int new_db, int old_db)
{
	struct nvmev_completion_queue *cq = nvmev_vdev->cqes[cqid];
	int i;
	for (i = old_db; i != new_db; i++) {
		int sqid;

		if (i >= cq->queue_size) {
			i = -1;
			continue;
		}
		sqid = cq_entry(i).sq_id;

		/* Should check the validity here since SPDK deletes SQ immediately
		 * before processing associated CQes */
		if (!nvmev_vdev->sqes[sqid]) continue;

		nvmev_vdev->sqes[sqid]->stat.nr_in_flight--;
	}

	cq->cq_tail = new_db - 1;
	if (new_db == -1)
		cq->cq_tail = cq->queue_size - 1;
}

static void __fill_cq_result(struct nvmev_io_worker *worker, struct nvmev_io_work *w)
{
	int sqid = w->sqid;
	int cqid = w->cqid;
	int sq_entry = w->sq_entry;
	unsigned int command_id = w->command_id;
	unsigned int status = w->status;
	unsigned int result0 = w->result0;
	unsigned int result1 = w->result1;

	struct nvmev_completion_queue *cq = nvmev_vdev->cqes[cqid];
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	struct nvme_completion *cqe;
	struct nvmev_io_worker *irq_worker;
	int cq_head;
	int sq_head;
	bool need_pending_irq = false;

	if (unlikely(!cq || !sq))
		return;

	/* SQHD must track the controller's current SQ head and must not move
	 * backwards when completions are returned out of order. */
	sq_head = READ_ONCE(nvmev_vdev->old_dbs[sqid * 2]);

	irq_worker = worker;
	if (cq->worker_id != worker->id)
		irq_worker = &nvmev_vdev->io_workers[cq->worker_id];

	spin_lock(&cq->entry_lock);
	cq_head = cq->cq_head;
	cqe = &cq_entry(cq_head);

	cqe->command_id = command_id;
	cqe->sq_id = sqid;
	/*
	 * SQHD reports the next SQ entry the controller expects, not the
	 * entry that just completed. SPDK uses this field to reclaim SQ slots.
	 */
	cqe->sq_head = sq_head;
	cqe->status = cq->phase | (status << 1);
	cqe->result0 = result0;
	cqe->result1 = result1;

	if (++cq_head == cq->queue_size) {
		cq_head = 0;
		cq->phase = !cq->phase;
	}

	cq->cq_head = cq_head;
	need_pending_irq = cq->irq_enabled && !cq->interrupt_ready;
	cq->interrupt_ready = true;
	spin_unlock(&cq->entry_lock);

	if (need_pending_irq)
		__enqueue_pending_cq(irq_worker, cqid);
}

static int nvmev_io_worker(void *data)
{
	struct nvmev_io_worker *worker = (struct nvmev_io_worker *)data;
	struct nvmev_ns *ns;
	static unsigned long last_io_time = 0;

#ifdef PERF_DEBUG
	static unsigned long long intr_clock[NR_MAX_IO_QUEUE + 1];
	static unsigned long long intr_counter[NR_MAX_IO_QUEUE + 1];

	unsigned long long prev_clock;
#endif

	NVMEV_INFO("%s started on cpu %d (node %d)\n", worker->thread_name, smp_processor_id(),
		   cpu_to_node(smp_processor_id()));

	while (!kthread_should_stop()) {
		unsigned long long curr_nsecs_wall = __get_wallclock(worker->cpu_nr_dispatcher);
		unsigned long long curr_nsecs_local = local_clock();
		long long delta = curr_nsecs_wall - curr_nsecs_local;
		unsigned long long prof_start = 0;

		volatile unsigned int curr = worker->io_seq;
		int qidx;

		if (unlikely(debug))
			worker->profile.loops++;

		while (curr != -1) {
			struct nvmev_io_work *w = &worker->work_queue[curr];
			unsigned long long curr_nsecs = local_clock() + delta;
			worker->latest_nsecs = curr_nsecs;

			if (unlikely(debug))
				worker->profile.scanned_entries++;

			if (w->is_completed == true) {
				curr = w->next;
				continue;
			}

			if (w->is_copied == false) {
				if (unlikely(debug))
					prof_start = local_clock();
				if (unlikely(debug) && !w->is_internal)
					w->nsecs_copy_start = local_clock() + delta;
				if (w->is_internal) {
					;
				} else if (io_using_dma) {
					__do_perform_io_using_dma(w->sqid, w->sq_entry);
				} else {
#if (BASE_SSD == KV_PROTOTYPE)
					struct nvmev_submission_queue *sq =
						nvmev_vdev->sqes[w->sqid];
					ns = &nvmev_vdev->ns[0];
					if (ns->identify_io_cmd(ns, sq_entry(w->sq_entry))) {
						w->result0 = ns->perform_io_cmd(
							ns, &sq_entry(w->sq_entry), &(w->status));
					} else {
						__do_perform_io(w->sqid, w->sq_entry);
					}
#else 
					__do_perform_io(w->sqid, w->sq_entry);
#endif
				}

				if (unlikely(debug) && !w->is_internal)
					w->nsecs_copy_done = local_clock() + delta;
				if (unlikely(debug)) {
					worker->profile.copy_calls++;
					worker->profile.copy_ns += local_clock() - prof_start;
				}
				w->is_copied = true;
				last_io_time = jiffies;

				NVMEV_DEBUG_VERBOSE("%s: copied %u, %d %d %d\n", worker->thread_name, curr,
					    w->sqid, w->cqid, w->sq_entry);
			}

			if (w->nsecs_target <= curr_nsecs) {
				if (unlikely(debug))
					prof_start = local_clock();
				if (w->is_internal) {
#if (SUPPORTED_SSD_TYPE(CONV) || SUPPORTED_SSD_TYPE(ZNS))
					buffer_release((struct buffer *)w->write_buffer,
						       w->buffs_to_release);
#endif
				} else {
					__fill_cq_result(worker, w);
				}

				NVMEV_DEBUG_VERBOSE("%s: completed %u, %d %d %d\n", worker->thread_name, curr,
					    w->sqid, w->cqid, w->sq_entry);

				if (unlikely(debug) && !w->is_internal) {
					w->nsecs_cq_filled = local_clock() + delta;
					worker->profile.latency_samples++;
					worker->profile.queue_wait_ns +=
						w->nsecs_copy_start - w->nsecs_enqueue;
					worker->profile.post_copy_wait_ns +=
						w->nsecs_cq_filled - w->nsecs_copy_done;
					worker->profile.total_device_ns +=
						w->nsecs_cq_filled - w->nsecs_start;
				}
#ifdef PERF_DEBUG
				trace_printk("%llu %llu %llu %llu %llu %llu\n", w->nsecs_start,
					     w->nsecs_enqueue - w->nsecs_start,
					     w->nsecs_copy_start - w->nsecs_start,
					     w->nsecs_copy_done - w->nsecs_start,
					     w->nsecs_cq_filled - w->nsecs_start,
					     w->nsecs_target - w->nsecs_start);
#endif
				if (unlikely(debug)) {
					worker->profile.complete_calls++;
					worker->profile.complete_ns += local_clock() - prof_start;
				}
				mb(); /* Reclaimer shall see after here */
				w->is_completed = true;
			}

			curr = w->next;
		}

#ifdef CONFIG_NVMEV_IO_WORKER_BY_SQ
		while ((qidx = __dequeue_pending_cq(worker)) != -1) {
			struct nvmev_completion_queue *cq = nvmev_vdev->cqes[qidx];
#else
		for (qidx = 1; qidx <= nvmev_vdev->nr_cq; qidx++) {
			struct nvmev_completion_queue *cq = nvmev_vdev->cqes[qidx];
#endif

			if (unlikely(debug))
				worker->profile.irq_checks++;
			if (cq == NULL || !cq->irq_enabled)
				continue;

			if (mutex_trylock(&cq->irq_lock)) {
				if (cq->interrupt_ready == true) {
					if (unlikely(debug))
						prof_start = local_clock();
#ifdef PERF_DEBUG
					prev_clock = local_clock();
#endif
					cq->interrupt_ready = false;
					nvmev_signal_irq(cq->irq_vector);
					if (unlikely(debug)) {
						worker->profile.irq_sent++;
						worker->profile.irq_ns += local_clock() - prof_start;
					}

#ifdef PERF_DEBUG
					intr_clock[qidx] += (local_clock() - prev_clock);
					intr_counter[qidx]++;

					if (intr_counter[qidx] > 1000) {
						NVMEV_DEBUG("Intr %d: %llu\n", qidx,
							    intr_clock[qidx] / intr_counter[qidx]);
						intr_clock[qidx] = 0;
						intr_counter[qidx] = 0;
					}
#endif
				}
				mutex_unlock(&cq->irq_lock);
			} else if (unlikely(debug)) {
				worker->profile.irq_lock_fail++;
#ifdef CONFIG_NVMEV_IO_WORKER_BY_SQ
				__enqueue_pending_cq(worker, qidx);
				break;
#endif
			}
		}
		if (CONFIG_NVMEVIRT_IDLE_TIMEOUT != 0 &&
		    time_after(jiffies, last_io_time + (CONFIG_NVMEVIRT_IDLE_TIMEOUT * HZ)))
			schedule_timeout_interruptible(1);
		else
			cond_resched();
	}

	return 0;
}

bool NVMEV_IO_WORKER_INIT(struct nvmev_dev *nvmev_vdev)
{
	unsigned int i, worker_id, disp_id;
	unsigned int nr_disp = nvmev_vdev->nr_dispatchers;
	unsigned int nr_workers = nvmev_vdev->config.nr_io_workers;
	unsigned int workers_per_disp = nr_workers / nr_disp;
	unsigned int extra_workers = nr_workers % nr_disp;
	unsigned int worker_offset = 0;

	nvmev_vdev->io_workers =
		kcalloc(nr_workers, sizeof(struct nvmev_io_worker), GFP_KERNEL);

	/* Assign workers to dispatchers before starting any kthreads */
	for (disp_id = 0; disp_id < nr_disp; disp_id++) {
		unsigned int count = workers_per_disp + (disp_id < extra_workers ? 1 : 0);
		unsigned int cpu_nr = nvmev_vdev->dispatchers[disp_id].cpu_nr;

		for (i = 0; i < count; i++) {
			struct nvmev_io_worker *w = &nvmev_vdev->io_workers[worker_offset + i];
			w->dispatcher_id = disp_id;
			w->cpu_nr_dispatcher = cpu_nr;
		}
		worker_offset += count;
	}

	for (worker_id = 0; worker_id < nr_workers; worker_id++) {
		struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[worker_id];

		worker->work_queue = kvcalloc(NR_MAX_PARALLEL_IO,
					      sizeof(struct nvmev_io_work),
					      GFP_KERNEL);
		if (!worker->work_queue) {
			NVMEV_ERROR("Failed to allocate work_queue for worker %u (%zu bytes)\n",
				    worker_id,
				    sizeof(struct nvmev_io_work) * (size_t)NR_MAX_PARALLEL_IO);
			goto err_alloc;
		}
		for (i = 0; i < NR_MAX_PARALLEL_IO; i++) {
			worker->work_queue[i].next = i + 1;
			worker->work_queue[i].prev = i - 1;
		}
		worker->work_queue[NR_MAX_PARALLEL_IO - 1].next = -1;
		worker->id = worker_id;
		worker->free_seq = 0;
		worker->free_seq_end = NR_MAX_PARALLEL_IO - 1;
		worker->io_seq = -1;
		worker->io_seq_end = -1;
		spin_lock_init(&worker->pending_cq_lock);
		worker->pending_cq_head = -1;
		worker->pending_cq_tail = -1;
		memset(worker->pending_cq_next, 0xff, sizeof(worker->pending_cq_next));
		worker->nr_cq_qids = 0;

		snprintf(worker->thread_name, sizeof(worker->thread_name),
			 "nvmev_io_worker_%d", worker_id);

		worker->task_struct = kthread_create(nvmev_io_worker, worker,
						     "%s", worker->thread_name);

		kthread_bind(worker->task_struct,
			     nvmev_vdev->config.cpu_nr_io_workers[worker_id]);
		wake_up_process(worker->task_struct);
	}

	return true;

err_alloc:
	while (worker_id-- > 0) {
		struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[worker_id];

		if (!IS_ERR_OR_NULL(worker->task_struct)) {
			kthread_stop(worker->task_struct);
		}

		kvfree(worker->work_queue);
	}

	kfree(nvmev_vdev->io_workers);
	nvmev_vdev->io_workers = NULL;
	return false;
}

void NVMEV_IO_WORKER_FINAL(struct nvmev_dev *nvmev_vdev)
{
	unsigned int i;

	for (i = 0; i < nvmev_vdev->config.nr_io_workers; i++) {
		struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[i];

		if (!IS_ERR_OR_NULL(worker->task_struct)) {
			kthread_stop(worker->task_struct);
		}

		kvfree(worker->work_queue);
	}

	kfree(nvmev_vdev->io_workers);
}
