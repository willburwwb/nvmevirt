// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#ifdef CONFIG_X86
#include <asm/e820/types.h>
#include <asm/e820/api.h>
#endif

#include "nvmev.h"
#include "conv_ftl.h"
#include "zns_ftl.h"
#include "simple_ftl.h"
#include "kv_ftl.h"
#include "dma.h"

/****************************************************************
 * Memory Layout
 ****************************************************************
 * virtDev
 *  - PCI header
 *    -> BAR at 1MiB area
 *  - PCI capability descriptors
 *
 * +--- memmap_start
 * |
 * v
 * +--------------+------------------------------------------+
 * | <---1MiB---> | <---------- Storage Area --------------> |
 * +--------------+------------------------------------------+
 *
 * 1MiB area for metadata
 *  - BAR : 1 page
 *	- DBS : 1 page
 *	- MSI-x table: 16 bytes/entry * 32
 *
 * Storage area
 *
 ****************************************************************/

/****************************************************************
 * Argument
 ****************************************************************
 * 1. Memmap start
 * 2. Memmap size
 ****************************************************************/

struct nvmev_dev *nvmev_vdev = NULL;

static unsigned long memmap_start = 0;
static unsigned long memmap_size = 0;

static unsigned int read_time = 1;
static unsigned int read_delay = 1;
static unsigned int read_trailing = 0;

static unsigned int write_time = 1;
static unsigned int write_delay = 1;
static unsigned int write_trailing = 0;

static unsigned int nr_io_units = 8;
static unsigned int io_unit_shift = 12;

static unsigned int nr_dispatchers = 1;

static char *cpus;
unsigned int debug = 0;

int io_using_dma = false;

static int set_parse_mem_param(const char *val, const struct kernel_param *kp)
{
	unsigned long *arg = (unsigned long *)kp->arg;
	*arg = memparse(val, NULL);
	return 0;
}

static struct kernel_param_ops ops_parse_mem_param = {
	.set = set_parse_mem_param,
	.get = param_get_ulong,
};

module_param_cb(memmap_start, &ops_parse_mem_param, &memmap_start, 0444);
MODULE_PARM_DESC(memmap_start, "Reserved memory address");
module_param_cb(memmap_size, &ops_parse_mem_param, &memmap_size, 0444);
MODULE_PARM_DESC(memmap_size, "Reserved memory size");
module_param(read_time, uint, 0644);
MODULE_PARM_DESC(read_time, "Read time in nanoseconds");
module_param(read_delay, uint, 0644);
MODULE_PARM_DESC(read_delay, "Read delay in nanoseconds");
module_param(read_trailing, uint, 0644);
MODULE_PARM_DESC(read_trailing, "Read trailing in nanoseconds");
module_param(write_time, uint, 0644);
MODULE_PARM_DESC(write_time, "Write time in nanoseconds");
module_param(write_delay, uint, 0644);
MODULE_PARM_DESC(write_delay, "Write delay in nanoseconds");
module_param(write_trailing, uint, 0644);
MODULE_PARM_DESC(write_trailing, "Write trailing in nanoseconds");
module_param(nr_io_units, uint, 0444);
MODULE_PARM_DESC(nr_io_units, "Number of I/O units that operate in parallel");
module_param(io_unit_shift, uint, 0444);
MODULE_PARM_DESC(io_unit_shift, "Size of each I/O unit (2^)");
module_param(nr_dispatchers, uint, 0444);
MODULE_PARM_DESC(nr_dispatchers, "Number of dispatcher threads (default 1)");
module_param(cpus, charp, 0444);
MODULE_PARM_DESC(cpus, "CPU list: first nr_dispatchers CPUs for dispatchers, rest for IO workers");
module_param(debug, uint, 0644);

static bool nvmev_proc_dbs(struct nvmev_dispatcher_ctx *disp_ctx)
{
	int dbs_idx;
	int new_db;
	int old_db;
	bool updated = false;
	unsigned int idx;
	unsigned int nr_qids;

	if (disp_ctx->id == 0) {
		new_db = nvmev_vdev->dbs[0];
		if (new_db != nvmev_vdev->old_dbs[0]) {
			nvmev_proc_admin_sq(new_db, nvmev_vdev->old_dbs[0]);
			nvmev_vdev->old_dbs[0] = new_db;
			updated = true;
		}
		new_db = nvmev_vdev->dbs[1];
		if (new_db != nvmev_vdev->old_dbs[1]) {
			nvmev_proc_admin_cq(new_db, nvmev_vdev->old_dbs[1]);
			nvmev_vdev->old_dbs[1] = new_db;
			updated = true;
		}
	}

	nr_qids = READ_ONCE(disp_ctx->nr_sq_qids);
	for (idx = 0; idx < nr_qids; idx++) {
		unsigned int qid = READ_ONCE(disp_ctx->sq_qids[idx]);

		if (qid == 0 || qid > nvmev_vdev->nr_sq)
			continue;
		if (nvmev_vdev->sqes[qid] == NULL)
			continue;
		dbs_idx = qid * 2;
		new_db = nvmev_vdev->dbs[dbs_idx];
		old_db = nvmev_vdev->old_dbs[dbs_idx];
		if (new_db != old_db) {
			nvmev_vdev->old_dbs[dbs_idx] =
				nvmev_proc_io_sq(disp_ctx, qid, new_db, old_db);
			updated = true;
		}
	}

	nr_qids = READ_ONCE(disp_ctx->nr_cq_qids);
	for (idx = 0; idx < nr_qids; idx++) {
		unsigned int qid = READ_ONCE(disp_ctx->cq_qids[idx]);

		if (qid == 0 || qid > nvmev_vdev->nr_cq)
			continue;
		if (nvmev_vdev->cqes[qid] == NULL)
			continue;
		dbs_idx = qid * 2 + 1;
		new_db = nvmev_vdev->dbs[dbs_idx];
		old_db = nvmev_vdev->old_dbs[dbs_idx];
		if (new_db != old_db) {
			nvmev_proc_io_cq(qid, new_db, old_db);
			nvmev_vdev->old_dbs[dbs_idx] = new_db;
			updated = true;
		}
	}

	return updated;
}

static int nvmev_dispatcher(void *data)
{
	struct nvmev_dispatcher_ctx *disp_ctx = (struct nvmev_dispatcher_ctx *)data;
	unsigned long last_dispatched_time = 0;

	NVMEV_INFO("%s started on cpu %d (node %d), workers [%u..%u]\n",
		   disp_ctx->thread_name, disp_ctx->cpu_nr,
		   cpu_to_node(disp_ctx->cpu_nr),
		   disp_ctx->first_worker_id,
		   disp_ctx->first_worker_id + disp_ctx->nr_workers - 1);

	while (!kthread_should_stop()) {
		bool updated = false;

		if (unlikely(debug))
			disp_ctx->profile.loops++;

		if (disp_ctx->id == 0 && nvmev_proc_bars()) {
			last_dispatched_time = jiffies;
			updated = true;
		}
		if (nvmev_proc_dbs(disp_ctx)) {
			last_dispatched_time = jiffies;
			updated = true;
		}
		if (unlikely(debug) && updated)
			disp_ctx->profile.active_loops++;

		if (CONFIG_NVMEVIRT_IDLE_TIMEOUT != 0 &&
		    time_after(jiffies, last_dispatched_time + (CONFIG_NVMEVIRT_IDLE_TIMEOUT * HZ)))
			schedule_timeout_interruptible(1);
		else
			cond_resched();
	}

	return 0;
}

static void NVMEV_DISPATCHER_INIT(struct nvmev_dev *nvmev_vdev)
{
	unsigned int i;
	unsigned int nr_disp = nvmev_vdev->config.nr_dispatchers;
	unsigned int nr_workers = nvmev_vdev->config.nr_io_workers;
	unsigned int workers_per_disp = nr_workers / nr_disp;
	unsigned int extra_workers = nr_workers % nr_disp;
	unsigned int worker_offset = 0;

	nvmev_vdev->dispatchers = kcalloc(nr_disp, sizeof(struct nvmev_dispatcher_ctx), GFP_KERNEL);
	nvmev_vdev->nr_dispatchers = nr_disp;

	for (i = 0; i < nr_disp; i++) {
		struct nvmev_dispatcher_ctx *disp = &nvmev_vdev->dispatchers[i];

		disp->id = i;
		disp->cpu_nr = nvmev_vdev->config.cpu_nr_dispatchers[i];
		disp->first_worker_id = worker_offset;
		disp->nr_workers = workers_per_disp + (i < extra_workers ? 1 : 0);
		disp->io_worker_turn = 0;
		disp->reclaim_batch_count = 0;
		disp->nr_sq_qids = 0;
		disp->nr_cq_qids = 0;

		snprintf(disp->thread_name, sizeof(disp->thread_name),
			 "nvmev_dispatcher_%u", i);

		worker_offset += disp->nr_workers;

		disp->task_struct = kthread_create(nvmev_dispatcher, disp,
						   "%s", disp->thread_name);
		kthread_bind(disp->task_struct, disp->cpu_nr);
		wake_up_process(disp->task_struct);
	}
}

static void NVMEV_DISPATCHER_FINAL(struct nvmev_dev *nvmev_vdev)
{
	unsigned int i;

	if (!nvmev_vdev->dispatchers)
		return;

	for (i = 0; i < nvmev_vdev->nr_dispatchers; i++) {
		struct nvmev_dispatcher_ctx *disp = &nvmev_vdev->dispatchers[i];
		if (!IS_ERR_OR_NULL(disp->task_struct)) {
			kthread_stop(disp->task_struct);
			disp->task_struct = NULL;
		}
	}

	kfree(nvmev_vdev->dispatchers);
	nvmev_vdev->dispatchers = NULL;
}

#ifdef CONFIG_X86
static int __validate_configs_arch(void)
{
	unsigned long resv_start_bytes;
	unsigned long resv_end_bytes;

	resv_start_bytes = memmap_start;
	resv_end_bytes = resv_start_bytes + memmap_size - 1;

	if (e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RAM) ||
	    e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RESERVED_KERN)) {
		NVMEV_ERROR("[mem %#010lx-%#010lx] is usable, not reseved region\n",
			    (unsigned long)resv_start_bytes, (unsigned long)resv_end_bytes);
		return -EPERM;
	}

	if (!e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RESERVED)) {
		NVMEV_ERROR("[mem %#010lx-%#010lx] is not reseved region\n",
			    (unsigned long)resv_start_bytes, (unsigned long)resv_end_bytes);
		return -EPERM;
	}
	return 0;
}
#else
static int __validate_configs_arch(void)
{
	/* TODO: Validate architecture-specific configurations */
	return 0;
}
#endif

static int __validate_configs(void)
{
	if (!memmap_start) {
		NVMEV_ERROR("[memmap_start] should be specified\n");
		return -EINVAL;
	}

	if (!memmap_size) {
		NVMEV_ERROR("[memmap_size] should be specified\n");
		return -EINVAL;
	} else if (memmap_size <= MB(1)) {
		NVMEV_ERROR("[memmap_size] should be bigger than 1 MiB\n");
		return -EINVAL;
	}

	if (__validate_configs_arch()) {
		return -EPERM;
	}

	if (nr_io_units == 0 || io_unit_shift == 0) {
		NVMEV_ERROR("Need non-zero IO unit size and at least one IO unit\n");
		return -EINVAL;
	}
	if (read_time == 0) {
		NVMEV_ERROR("Need non-zero read time\n");
		return -EINVAL;
	}
	if (write_time == 0) {
		NVMEV_ERROR("Need non-zero write time\n");
		return -EINVAL;
	}

	return 0;
}

static void __print_perf_configs(void)
{
#ifdef CONFIG_NVMEV_VERBOSE
	unsigned long unit_perf_kb =
			nvmev_vdev->config.nr_io_units << (nvmev_vdev->config.io_unit_shift - 10);
	struct nvmev_config *cfg = &nvmev_vdev->config;

	NVMEV_INFO("=============== Configurations ===============\n");
	NVMEV_INFO("* IO units : %d x %d\n",
			cfg->nr_io_units, 1 << cfg->io_unit_shift);
	NVMEV_INFO("* I/O times\n");
	NVMEV_INFO("  Read     : %u + %u x + %u ns\n",
				cfg->read_delay, cfg->read_time, cfg->read_trailing);
	NVMEV_INFO("  Write    : %u + %u x + %u ns\n",
				cfg->write_delay, cfg->write_time, cfg->write_trailing);
	NVMEV_INFO("* Bandwidth\n");
	NVMEV_INFO("  Read     : %lu MiB/s\n",
			(1000000000UL / (cfg->read_time + cfg->read_delay + cfg->read_trailing)) * unit_perf_kb >> 10);
	NVMEV_INFO("  Write    : %lu MiB/s\n",
			(1000000000UL / (cfg->write_time + cfg->write_delay + cfg->write_trailing)) * unit_perf_kb >> 10);
#endif
}

static int __get_nr_entries(int dbs_idx, int queue_size)
{
	int diff = nvmev_vdev->dbs[dbs_idx] - nvmev_vdev->old_dbs[dbs_idx];
	if (diff < 0) {
		diff += queue_size;
	}
	return diff;
}

static void __reset_debug_stats(void)
{
	unsigned int i;

	if (!nvmev_vdev)
		return;

	if (nvmev_vdev->dispatchers != NULL) {
		for (i = 0; i < nvmev_vdev->nr_dispatchers; i++)
			memset(&nvmev_vdev->dispatchers[i].profile, 0x0,
			       sizeof(nvmev_vdev->dispatchers[i].profile));
	}

	if (nvmev_vdev->io_workers != NULL) {
		for (i = 0; i < nvmev_vdev->config.nr_io_workers; i++)
			memset(&nvmev_vdev->io_workers[i].profile, 0x0,
			       sizeof(nvmev_vdev->io_workers[i].profile));
	}
}

static int __proc_file_read(struct seq_file *m, void *data)
{
	const char *filename = m->private;
	struct nvmev_config *cfg = &nvmev_vdev->config;

	if (strcmp(filename, "read_times") == 0) {
		seq_printf(m, "%u + %u x + %u", cfg->read_delay, cfg->read_time,
			   cfg->read_trailing);
	} else if (strcmp(filename, "write_times") == 0) {
		seq_printf(m, "%u + %u x + %u", cfg->write_delay, cfg->write_time,
			   cfg->write_trailing);
	} else if (strcmp(filename, "io_units") == 0) {
		seq_printf(m, "%u x %u", cfg->nr_io_units, cfg->io_unit_shift);
	} else if (strcmp(filename, "stat") == 0) {
		int i;
		unsigned int nr_in_flight = 0;
		unsigned int nr_dispatch = 0;
		unsigned int nr_dispatched = 0;
		unsigned long long total_io = 0;
		for (i = 1; i <= nvmev_vdev->nr_sq; i++) {
			struct nvmev_submission_queue *sq = nvmev_vdev->sqes[i];
			if (!sq)
				continue;

			seq_printf(m, "%2d: %2u %4u %4u %4u %4u %llu\n", i,
				   __get_nr_entries(i * 2, sq->queue_size), sq->stat.nr_in_flight,
				   sq->stat.max_nr_in_flight, sq->stat.nr_dispatch,
				   sq->stat.nr_dispatched, sq->stat.total_io);

			nr_in_flight += sq->stat.nr_in_flight;
			nr_dispatch += sq->stat.nr_dispatch;
			nr_dispatched += sq->stat.nr_dispatched;
			total_io += sq->stat.total_io;

			barrier();
			sq->stat.max_nr_in_flight = 0;
		}
		seq_printf(m, "total: %u %u %u %llu\n", nr_in_flight, nr_dispatch, nr_dispatched,
			   total_io);
	} else if (strcmp(filename, "debug") == 0) {
		unsigned int i;

		seq_printf(m, "enabled %u\n", debug);
		seq_puts(m, "dispatcher id loops active sq_batches sq_entries avg_proc_io_ns "
			    "avg_enqueue_ns reclaim_calls avg_reclaim_ns reclaimed immediate "
			    "sorted alloc_reclaim alloc_reclaim_fail\n");
		if (nvmev_vdev->dispatchers != NULL) {
			for (i = 0; i < nvmev_vdev->nr_dispatchers; i++) {
				struct nvmev_dispatcher_ctx *disp = &nvmev_vdev->dispatchers[i];
				struct nvmev_dispatcher_profile *p = &disp->profile;
				unsigned long long avg_proc =
					p->sq_entries ? div64_u64(p->proc_io_ns, p->sq_entries) : 0;
				unsigned long long avg_enqueue =
					p->sq_entries ? div64_u64(p->enqueue_ns, p->sq_entries) : 0;
				unsigned long long avg_reclaim =
					p->reclaim_calls ? div64_u64(p->reclaim_ns,
								      p->reclaim_calls) : 0;

				seq_printf(m,
					   "dispatcher %u %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
					   i, p->loops, p->active_loops, p->sq_batches,
					   p->sq_entries, avg_proc, avg_enqueue,
					   p->reclaim_calls, avg_reclaim,
					   p->reclaimed_entries, p->immediate_enqueues,
					   p->sorted_enqueues, p->alloc_reclaim_calls,
					   p->alloc_reclaim_failures);
			}
		}

		seq_puts(m, "worker id loops scanned avg_scanned copy_calls avg_copy_ns "
			    "complete_calls avg_complete_ns irq_checks irq_sent avg_irq_ns "
			    "irq_lock_fail\n");
		if (nvmev_vdev->io_workers != NULL) {
			for (i = 0; i < nvmev_vdev->config.nr_io_workers; i++) {
				struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[i];
				struct nvmev_io_worker_profile *p = &worker->profile;
				unsigned long long avg_scanned =
					p->loops ? div64_u64(p->scanned_entries, p->loops) : 0;
				unsigned long long avg_copy =
					p->copy_calls ? div64_u64(p->copy_ns, p->copy_calls) : 0;
				unsigned long long avg_complete =
					p->complete_calls ? div64_u64(p->complete_ns,
								      p->complete_calls) : 0;
				unsigned long long avg_irq =
					p->irq_sent ? div64_u64(p->irq_ns, p->irq_sent) : 0;

				seq_printf(m,
					   "worker %u %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
					   i, p->loops, p->scanned_entries, avg_scanned,
					   p->copy_calls, avg_copy, p->complete_calls,
					   avg_complete, p->irq_checks, p->irq_sent,
					   avg_irq, p->irq_lock_fail);
			}
		}

		seq_puts(m, "worker_io id avg_queue_wait_ns avg_post_copy_wait_ns "
			    "avg_total_device_ns latency_samples\n");
		if (nvmev_vdev->io_workers != NULL) {
			for (i = 0; i < nvmev_vdev->config.nr_io_workers; i++) {
				struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[i];
				struct nvmev_io_worker_profile *p = &worker->profile;
				unsigned long long avg_queue_wait =
					p->latency_samples ? div64_u64(p->queue_wait_ns,
									p->latency_samples) : 0;
				unsigned long long avg_post_copy_wait =
					p->latency_samples ? div64_u64(p->post_copy_wait_ns,
									p->latency_samples) : 0;
				unsigned long long avg_total_device =
					p->latency_samples ? div64_u64(p->total_device_ns,
									p->latency_samples) : 0;

				seq_printf(m,
					   "worker_io %u %llu %llu %llu %llu\n",
					   i, avg_queue_wait, avg_post_copy_wait,
					   avg_total_device, p->latency_samples);
			}
		}
	}

	return 0;
}

static ssize_t __proc_file_write(struct file *file, const char __user *buf, size_t len,
				 loff_t *offp)
{
	ssize_t count = len;
	const char *filename = file->f_path.dentry->d_name.name;
	char input[128];
	unsigned int ret;
	unsigned int new_debug;
	unsigned long long *old_stat;
	struct nvmev_config *cfg = &nvmev_vdev->config;
	size_t nr_copied;
	size_t input_len;

	input_len = min(len, sizeof(input) - 1);
	nr_copied = copy_from_user(input, buf, input_len);
	input[input_len - nr_copied] = '\0';

	if (!strcmp(filename, "read_times")) {
		ret = sscanf(input, "%u %u %u", &cfg->read_delay, &cfg->read_time,
			     &cfg->read_trailing);
		//adjust_ftl_latency(0, cfg->read_time);
	} else if (!strcmp(filename, "write_times")) {
		ret = sscanf(input, "%u %u %u", &cfg->write_delay, &cfg->write_time,
			     &cfg->write_trailing);
		//adjust_ftl_latency(1, cfg->write_time);
	} else if (!strcmp(filename, "io_units")) {
		ret = sscanf(input, "%d %d", &cfg->nr_io_units, &cfg->io_unit_shift);
		if (ret < 1)
			goto out;

		old_stat = nvmev_vdev->io_unit_stat;
		nvmev_vdev->io_unit_stat =
			kzalloc(sizeof(*nvmev_vdev->io_unit_stat) * cfg->nr_io_units, GFP_KERNEL);

		mdelay(100); /* XXX: Delay the free of old stat so that outstanding
						 * requests accessing the unit_stat are all returned
						 */
		kfree(old_stat);
	} else if (!strcmp(filename, "stat")) {
		int i;
		for (i = 1; i <= nvmev_vdev->nr_sq; i++) {
			struct nvmev_submission_queue *sq = nvmev_vdev->sqes[i];
			if (!sq)
				continue;

			memset(&sq->stat, 0x00, sizeof(sq->stat));
		}
	} else if (!strcmp(filename, "debug")) {
		if (sysfs_streq(input, "reset")) {
			__reset_debug_stats();
		} else if (kstrtouint(input, 0, &new_debug) == 0) {
			debug = new_debug;
			__reset_debug_stats();
		}
	}

out:
	__print_perf_configs();

	return count;
}

static int __proc_file_open(struct inode *inode, struct file *file)
{
	return single_open(file, __proc_file_read, (char *)file->f_path.dentry->d_name.name);
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 0, 0)
static const struct proc_ops proc_file_fops = {
	.proc_open = __proc_file_open,
	.proc_write = __proc_file_write,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations proc_file_fops = {
	.open = __proc_file_open,
	.write = __proc_file_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static void NVMEV_STORAGE_INIT(struct nvmev_dev *nvmev_vdev)
{
	NVMEV_INFO("Storage: %#010lx-%#010lx (%lu MiB)\n",
			nvmev_vdev->config.storage_start,
			nvmev_vdev->config.storage_start + nvmev_vdev->config.storage_size,
			BYTE_TO_MB(nvmev_vdev->config.storage_size));

	nvmev_vdev->io_unit_stat = kzalloc(
		sizeof(*nvmev_vdev->io_unit_stat) * nvmev_vdev->config.nr_io_units, GFP_KERNEL);

	nvmev_vdev->storage_mapped = memremap(nvmev_vdev->config.storage_start,
					      nvmev_vdev->config.storage_size, MEMREMAP_WB);

	if (nvmev_vdev->storage_mapped == NULL)
		NVMEV_ERROR("Failed to map storage memory.\n");

	nvmev_vdev->proc_root = proc_mkdir("nvmev", NULL);
	nvmev_vdev->proc_read_times =
		proc_create("read_times", 0664, nvmev_vdev->proc_root, &proc_file_fops);
	nvmev_vdev->proc_write_times =
		proc_create("write_times", 0664, nvmev_vdev->proc_root, &proc_file_fops);
	nvmev_vdev->proc_io_units =
		proc_create("io_units", 0664, nvmev_vdev->proc_root, &proc_file_fops);
	nvmev_vdev->proc_stat = proc_create("stat", 0444, nvmev_vdev->proc_root, &proc_file_fops);
	nvmev_vdev->proc_debug = proc_create("debug", 0664, nvmev_vdev->proc_root, &proc_file_fops);
}

static void NVMEV_STORAGE_FINAL(struct nvmev_dev *nvmev_vdev)
{
	remove_proc_entry("read_times", nvmev_vdev->proc_root);
	remove_proc_entry("write_times", nvmev_vdev->proc_root);
	remove_proc_entry("io_units", nvmev_vdev->proc_root);
	remove_proc_entry("stat", nvmev_vdev->proc_root);
	remove_proc_entry("debug", nvmev_vdev->proc_root);

	remove_proc_entry("nvmev", NULL);

	if (nvmev_vdev->storage_mapped)
		memunmap(nvmev_vdev->storage_mapped);

	if (nvmev_vdev->io_unit_stat)
		kfree(nvmev_vdev->io_unit_stat);
}

static bool __load_configs(struct nvmev_config *config)
{
	unsigned int cpu_nr;
	unsigned int cpu_idx = 0;
	char *cpu;

	if (__validate_configs() < 0) {
		return false;
	}

	if (nr_dispatchers == 0 || nr_dispatchers > NR_MAX_DISPATCHERS) {
		NVMEV_ERROR("nr_dispatchers must be between 1 and %d\n", NR_MAX_DISPATCHERS);
		return false;
	}

#if (BASE_SSD == KV_PROTOTYPE)
	memmap_size -= KV_MAPPING_TABLE_SIZE;
#endif

	config->memmap_start = memmap_start;
	config->memmap_size = memmap_size;
	config->storage_start = memmap_start + MB(1);
	config->storage_size = memmap_size - MB(1);

	config->read_time = read_time;
	config->read_delay = read_delay;
	config->read_trailing = read_trailing;
	config->write_time = write_time;
	config->write_delay = write_delay;
	config->write_trailing = write_trailing;
	config->nr_io_units = nr_io_units;
	config->io_unit_shift = io_unit_shift;

	config->nr_dispatchers = nr_dispatchers;
	config->nr_io_workers = 0;
	memset(config->cpu_nr_dispatchers, 0xff, sizeof(config->cpu_nr_dispatchers));

	while ((cpu = strsep(&cpus, ",")) != NULL) {
		cpu_nr = (unsigned int)simple_strtol(cpu, NULL, 10);
		if (cpu_idx < nr_dispatchers) {
			config->cpu_nr_dispatchers[cpu_idx] = cpu_nr;
		} else {
			config->cpu_nr_io_workers[config->nr_io_workers] = cpu_nr;
			config->nr_io_workers++;
		}
		cpu_idx++;
	}

	if (cpu_idx < nr_dispatchers) {
		NVMEV_ERROR("Not enough CPUs specified: need at least %u for dispatchers\n",
			    nr_dispatchers);
		return false;
	}

	NVMEV_INFO("Configured %u dispatcher(s), %u IO worker(s)\n",
		   config->nr_dispatchers, config->nr_io_workers);

	return true;
}

static void NVMEV_NAMESPACE_INIT(struct nvmev_dev *nvmev_vdev)
{
	unsigned long long remaining_capacity = nvmev_vdev->config.storage_size;
	void *ns_addr = nvmev_vdev->storage_mapped;
	const int nr_ns = NR_NAMESPACES; // XXX: allow for dynamic nr_ns
	const unsigned int disp_no = nvmev_vdev->config.cpu_nr_dispatchers[0];
	int i;
	unsigned long long size;

	struct nvmev_ns *ns = kmalloc(sizeof(struct nvmev_ns) * nr_ns, GFP_KERNEL);

	for (i = 0; i < nr_ns; i++) {
		if (NS_CAPACITY(i) == 0)
			size = remaining_capacity;
		else
			size = min(NS_CAPACITY(i), remaining_capacity);

		if (NS_SSD_TYPE(i) == SSD_TYPE_NVM)
			simple_init_namespace(&ns[i], i, size, ns_addr, disp_no);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_CONV)
			conv_init_namespace(&ns[i], i, size, ns_addr, disp_no);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_ZNS)
			zns_init_namespace(&ns[i], i, size, ns_addr, disp_no);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_KV)
			kv_init_namespace(&ns[i], i, size, ns_addr, disp_no);
		else
			BUG_ON(1);

		remaining_capacity -= size;
		ns_addr += size;
		NVMEV_INFO("ns %d/%d: size %lld MiB\n", i, nr_ns, BYTE_TO_MB(ns[i].size));
	}

	nvmev_vdev->ns = ns;
	nvmev_vdev->nr_ns = nr_ns;
	nvmev_vdev->mdts = MDTS;
}

static void NVMEV_NAMESPACE_FINAL(struct nvmev_dev *nvmev_vdev)
{
	struct nvmev_ns *ns = nvmev_vdev->ns;
	const int nr_ns = NR_NAMESPACES; // XXX: allow for dynamic nvmev_vdev->nr_ns
	int i;

	for (i = 0; i < nr_ns; i++) {
		if (NS_SSD_TYPE(i) == SSD_TYPE_NVM)
			simple_remove_namespace(&ns[i]);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_CONV)
			conv_remove_namespace(&ns[i]);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_ZNS)
			zns_remove_namespace(&ns[i]);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_KV)
			kv_remove_namespace(&ns[i]);
		else
			BUG_ON(1);
	}

	kfree(ns);
	nvmev_vdev->ns = NULL;
}

static void __print_base_config(void)
{
	const char *type = "unknown";
	switch (BASE_SSD) {
	case INTEL_OPTANE:
		type = "NVM SSD";
		break;
	case SAMSUNG_970PRO:
		type = "Samsung 970 Pro SSD";
		break;
	case ZNS_PROTOTYPE:
		type = "ZNS SSD Prototype";
		break;
	case KV_PROTOTYPE:
		type = "KVSSD Prototype";
		break;
	case WD_ZN540:
		type = "WD ZN540 ZNS SSD";
		break;
	}

	NVMEV_INFO("Version %x.%x for >> %s <<\n",
			(NVMEV_VERSION & 0xff00) >> 8, (NVMEV_VERSION & 0x00ff), type);
}

static int NVMeV_init(void)
{
	int ret = 0;

	__print_base_config();

	nvmev_vdev = VDEV_INIT();
	if (!nvmev_vdev)
		return -EINVAL;

	if (!__load_configs(&nvmev_vdev->config)) {
		goto ret_err;
	}

	NVMEV_STORAGE_INIT(nvmev_vdev);

	NVMEV_NAMESPACE_INIT(nvmev_vdev);

	if (io_using_dma) {
		if (ioat_dma_chan_set("dma7chan0") != 0) {
			io_using_dma = false;
			NVMEV_ERROR("Cannot use DMA engine, Fall back to memcpy\n");
		}
	}

	if (!NVMEV_PCI_INIT(nvmev_vdev)) {
		goto ret_err;
	}

	__print_perf_configs();

	NVMEV_DISPATCHER_INIT(nvmev_vdev);
	if (!NVMEV_IO_WORKER_INIT(nvmev_vdev)) {
		goto ret_err;
	}

	pci_bus_add_devices(nvmev_vdev->virt_bus);

	NVMEV_INFO("Virtual NVMe device created\n");

	return 0;

ret_err:
	VDEV_FINALIZE(nvmev_vdev);
	return -EIO;
}

static void NVMeV_exit(void)
{
	int i;

	if (nvmev_vdev->virt_bus != NULL) {
		pci_stop_root_bus(nvmev_vdev->virt_bus);
		pci_remove_root_bus(nvmev_vdev->virt_bus);
	}

	/*
	 * Workers may still reference nvmev_vdev->dispatchers for CQ-to-worker
	 * routing. Stop workers first, then stop/free dispatchers.
	 */
	NVMEV_IO_WORKER_FINAL(nvmev_vdev);
	NVMEV_DISPATCHER_FINAL(nvmev_vdev);

	NVMEV_NAMESPACE_FINAL(nvmev_vdev);
	NVMEV_STORAGE_FINAL(nvmev_vdev);

	if (io_using_dma) {
		ioat_dma_cleanup();
	}

	for (i = 0; i < nvmev_vdev->nr_sq; i++) {
		kfree(nvmev_vdev->sqes[i]);
	}

	for (i = 0; i < nvmev_vdev->nr_cq; i++) {
		kfree(nvmev_vdev->cqes[i]);
	}

	VDEV_FINALIZE(nvmev_vdev);

	NVMEV_INFO("Virtual NVMe device closed\n");
}

MODULE_LICENSE("GPL v2");
module_init(NVMeV_init);
module_exit(NVMeV_exit);
