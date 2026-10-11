// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
/* avb-bench.c -- AVB RX throughput-measurement support (see avb-bench.h).
 *
 * Kept in its own translation unit so the production data path stays clean:
 * alsa.c reaches this code only through four small guarded calls. Everything
 * here is inert unless the module is loaded with avb_rx_bench=1.
 *
 * What it does in measurement mode:
 *   - broadens the RX-q1 flex filter to capture every AVTP frame (any MAC), so a
 *     bare AVTP blaster on a second box can load the i210 with no MSRP/MAC setup;
 *   - counts every packet the custom datapath drains from RX-q1 (that path
 *     bypasses igb's NAPI accounting, so ethtool's rx_queue_1_packets never moves
 *     -- this counter is the only way to see the received rate);
 *   - exposes the running total + a per-stream histogram + the live drop
 *     accumulators via debugfs, so the throughput-test/ harness reads exact
 *     numbers instead of scraping dmesg.
 *
 * Drops themselves are NOT read here from hardware registers: the igb watchdog
 * reads-and-clears RQDPC(1)/MPC/RNBC every ~2 s, so the trustworthy values are
 * the driver's software accumulators (rx_ring[1]->rx_stats.drops, stats.mpc,
 * stats.rnbc), which is what the debugfs readout below reports.
 */

#include "igb.h"
#include "avpdu.h"
#include "avb-bench.h"
#include "avb-log.h"

#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/string.h>

int avb_rx_bench;
module_param(avb_rx_bench, int, 0444);
MODULE_PARM_DESC(avb_rx_bench,
	"1 = RX throughput-measurement mode: broad AVTP capture on RX-q1 + packet "
	"counting, no ALSA client needed (used by throughput-test/). Default 0 = "
	"normal audio operation.");

#define AVB_BENCH_MAX_STREAMS 64

static u64 avb_bench_total;
static u64 avb_bench_per_stream[AVB_BENCH_MAX_STREAMS];
static struct igb_adapter *avb_bench_adapter;
static struct dentry *avb_bench_dir;

void avb_bench_setup_filter(struct igb_adapter *adapter, u8 *filter_mask)
{
	/* each mask bit enables one filter byte: match VLAN tag + AVTP ethertype (bytes 12-17) only */
	filter_mask[0] = 0x00;
	filter_mask[1] = 0xf0;
	filter_mask[2] = 0x03;
	adapter->avb_listener_ready = true;
	avb_info(adapter, "AVB RX benchmark mode -- capturing ALL AVTP on RX-q1 (any MAC)\n");
}

void avb_bench_count(const u8 *stream_id)
{
	avb_bench_total++;
	avb_bench_per_stream[stream_id[STREAM_ID_LEN - 1] % AVB_BENCH_MAX_STREAMS]++;
}

static int avb_bench_show(struct seq_file *m, void *v)
{
	struct igb_adapter *a = avb_bench_adapter;
	int i;

	seq_printf(m, "total=%llu\n", avb_bench_total);
	/* the igb watchdog read-clears RQDPC/MPC/RNBC, so report its software accumulators */
	if (a && a->rx_ring[1])
		seq_printf(m, "q1drops=%llu mpc=%llu rnbc=%llu\n",
			   a->rx_ring[1]->rx_stats.drops,
			   a->stats.mpc, a->stats.rnbc);
	for (i = 0; i < AVB_BENCH_MAX_STREAMS; i++)
		if (avb_bench_per_stream[i])
			seq_printf(m, "stream[%d]=%llu\n", i,
				   avb_bench_per_stream[i]);
	return 0;
}

static int avb_bench_open(struct inode *inode, struct file *file)
{
	return single_open(file, avb_bench_show, NULL);
}

static ssize_t avb_bench_write(struct file *file, const char __user *buf,
			       size_t len, loff_t *ppos)
{
	(void)file; (void)buf; (void)ppos;
	memset(avb_bench_per_stream, 0, sizeof(avb_bench_per_stream));
	avb_bench_total = 0;
	return len;
}

static const struct file_operations avb_bench_fops = {
	.owner   = THIS_MODULE,
	.open    = avb_bench_open,
	.read    = seq_read,
	.write   = avb_bench_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

void avb_bench_init(struct igb_adapter *adapter)
{
	avb_bench_adapter = adapter;
	avb_bench_total = 0;
	memset(avb_bench_per_stream, 0, sizeof(avb_bench_per_stream));

	avb_bench_dir = debugfs_create_dir("snd_igb_avb", NULL);
	debugfs_create_file("rx_bench", 0644, avb_bench_dir, NULL,
			    &avb_bench_fops);
	avb_info(adapter, "rx_bench counters at /sys/kernel/debug/snd_igb_avb/rx_bench\n");
}

void avb_bench_exit(void)
{
	debugfs_remove_recursive(avb_bench_dir);
	avb_bench_dir = NULL;
	avb_bench_adapter = NULL;
}