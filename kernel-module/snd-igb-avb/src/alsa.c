// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019, Ralf Beck
 * Copyright (c) 2026, mnemonic powerplay <mnemonic@xor-musix.de>
 */
#include "igb.h"
#include "avpdu.h"
#include "stream_id.h"
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <asm/fpu/api.h>
#include "avb-config.h"
#include "avb-bench.h"
#include "avb-log.h"
#include <linux/hrtimer.h>
#include <linux/etherdevice.h>
#include <linux/ctype.h>

/* 24, not 18: the flex filter mask covers 8 bytes per mask byte. */
#define AVB_RX_FILTER_LEN 24

#define AVB_RX_HW_FILTERS 8

static void avb_fill_rx_filter(const struct avb_stream *stream,
                               u8 *filter, u8 *filter_mask)
{
    memset(filter, 0, AVB_RX_FILTER_LEN);
    memset(filter_mask, 0, AVB_RX_FILTER_LEN / 8);

    memcpy(&filter[0], stream->dest_mac, 6);

    memcpy(&filter[6], stream->src_mac, 6);

    filter[12] = 0x81;
    filter[13] = 0x00;
    filter[14] = 0x60;
    filter[15] = 0x02;
    filter[16] = 0x22;
    filter[17] = 0xf0;

    filter_mask[0] = 0xff;
    filter_mask[1] = 0xff;
    filter_mask[2] = 0x03;
}

static void avb_fill_talker_rx_filter(const u8 *src_mac, u8 *filter, u8 *filter_mask)
{
    memset(filter, 0, AVB_RX_FILTER_LEN);
    memset(filter_mask, 0, AVB_RX_FILTER_LEN / 8);

    memcpy(&filter[6], src_mac, 6);
    filter[12] = 0x81;
    filter[13] = 0x00;
    filter[14] = 0x60;
    filter[15] = 0x02;
    filter[16] = 0x22;
    filter[17] = 0xf0;

    filter_mask[0] = 0xc0;
    filter_mask[1] = 0xff;
    filter_mask[2] = 0x03;
}

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

static int dev = 0;
static int num_streams = 1;

static int avb_rx_hw_filter_slot(int stream_i)
{
    return num_streams <= AVB_RX_HW_FILTERS ? stream_i : -1;
}
/* Must not be u8: 256 channels (32 streams x 8) would wrap to 0. */
unsigned int channels = CHANNELS_PER_STREAM;

static char *own_mac;
module_param(own_mac, charp, 0444);
MODULE_PARM_DESC(own_mac, "i210 own MAC (AVTP source), e.g. 00:1b:21:aa:bb:cc");

static char *avb_device_source_mac;
module_param(avb_device_source_mac, charp, 0444);
MODULE_PARM_DESC(avb_device_source_mac, "remote AVB device MAC");

static char *avb_device_talker_mac;
module_param(avb_device_talker_mac, charp, 0444);
MODULE_PARM_DESC(avb_device_talker_mac, "remote AVB device talker stream multicast MAC");

static char *avb_talker_dest_mac;
module_param(avb_talker_dest_mac, charp, 0644);
MODULE_PARM_DESC(avb_talker_dest_mac, "Unused (superseded by avb_talker_dest_macs); kept for insmod compatibility");

static char *avb_talker_dest_macs;

static char *avb_listener_dest_macs;
static char *avb_listener_src_macs;
static char *avb_listener_stream_ids;

static int clamp_num_streams(struct igb_adapter *adapter)
{
        if (num_streams < 1) {
                avb_warn(adapter, "num_streams=%d is invalid, clamping to 1\n",
                         num_streams);
                num_streams = 1;
        } else if (num_streams > MAX_STREAMS) {
                avb_warn(adapter, "num_streams=%d exceeds max %d, clamping\n",
                         num_streams, MAX_STREAMS);
                num_streams = MAX_STREAMS;
        }

        return num_streams;
}

module_param(num_streams, int, 0444);
MODULE_PARM_DESC(num_streams, "number of AVB streams (default 1, max 32)");

#define DMA_BUFFERSIZE (6*8*2*8) 

#define MAXPACKETSIZE 1024
#define MAXDESCRIPTORS 256
#define LAUNCH_OFFSET 125000
#define PRESENTATION_TIME_OFFSET avb_presentation_offset_ns

static int avb_presentation_offset_ns = 2125000;
module_param(avb_presentation_offset_ns, int, 0644);
MODULE_PARM_DESC(avb_presentation_offset_ns,
        "AVTP presentation time offset in ns (default 2125000 = 2 ms effective, "
        "the class-A convention; effective offset is this minus 125000)");
#define AVB_TX_INTERVAL_NS 125000
#define AVB_TX_SCHED_LEAD_NS 1000000

#define AVB_RX_DESC_BUDGET_MIN 64

static int avb_rx_desc_budget(void)
{
        return clamp(8 * num_streams, AVB_RX_DESC_BUDGET_MIN, MAXDESCRIPTORS / 2);
}

#define AVB_RX_LAP_HEADROOM 32

#define AVB_RX_LEAD_MAX 8
static int avb_rx_lead_packets = 2;
module_param(avb_rx_lead_packets, int, 0644);
MODULE_PARM_DESC(avb_rx_lead_packets,
        "capture write lead ahead of the ALSA pointer, in packets (default 2, max 8; 0 = no lead)");

static int avb_rx_slip_ns = 250000;
module_param(avb_rx_slip_ns, int, 0644);
MODULE_PARM_DESC(avb_rx_slip_ns,
        "RX media-clock slip threshold in ns (default 250000 = 2 packet intervals)");

static int avb_rx_drift_warn_ppb = 5000;
module_param(avb_rx_drift_warn_ppb, int, 0644);
MODULE_PARM_DESC(avb_rx_drift_warn_ppb,
        "warn when the peer's media clock differs by more than this many ppb (default 5000 = 5 ppm; 0 disables)");

static enum hrtimer_restart avb_tx_timer_fn(struct hrtimer *timer);

static u64 avb_encode_launch_time(u32 nsec)
{
        u32 launch_time_l = nsec + LAUNCH_OFFSET;
        u64 launch_time;

        if (launch_time_l >= NSEC_PER_SEC)
                launch_time_l -= NSEC_PER_SEC;

        launch_time = ((u64)(launch_time_l >> 5)) << 32;
        return launch_time & 0x1ffffff00000000ULL;
}

static u64 avb_encode_launch_time_abs(u32 nsec)
{
        return (((u64)(nsec >> 5)) << 32) & 0x1ffffff00000000ULL;
}

static u32 avb_decode_launch_time_ns(u64 launch_time)
{
        return (u32)(((launch_time & 0x1ffffff00000000ULL) >> 32) << 5);
}

static s64 avb_delta_u32_ns(u32 a, u32 b)
{
        return (s32)(a - b);
}

static s64 avb_delta_1s_ns(u32 a, u32 b)
{
        s64 delta = (s64)a - b;

        if (delta > NSEC_PER_SEC / 2)
                delta -= NSEC_PER_SEC;
        else if (delta < -NSEC_PER_SEC / 2)
                delta += NSEC_PER_SEC;

        return delta;
}

static u64 avb_align_up_u64(u64 value, u32 quantum)
{
        u64 rem = value % quantum;

        return rem ? value + quantum - rem : value;
}

static u32 avb_tx_desc_groups_used(struct igb_adapter *adapter, u32 next_group)
{
        u32 hw_head = E1000_READ_REG(&adapter->hw,
                                     E1000_TDH(adapter->tx_ring[0]->reg_idx));
        u32 groups = adapter->avb_tx_groups;
        u32 clean_group = (hw_head / 2) % groups;

        return (next_group + groups - clean_group) % groups;
}

static u32 avb_advance_playback_pointer(struct igb_adapter *adapter, u32 pointer,
                                        u32 frames)
{
        u32 channels = adapter->playback_channels;
        u32 buffer_frames = adapter->playback_buffer_size;

        if (!channels || !buffer_frames)
                return pointer;

        return (pointer + frames * channels) % (buffer_frames * channels);
}

static void avb_deinterleave_playback(struct igb_adapter *adapter,
                                      u32 *dma_area, int start_frame,
                                      int stream, AVPDU_HEADER *pavpdu)
{
        int channels = adapter->playback_channels;
        int buf = adapter->playback_buffer_size;
        int spp = adapter->samples_per_packet;
        int base = stream * CHANNELS_PER_STREAM;
        int frame = start_frame;
        int sp, ch;

#ifdef DEBUG_TICK_COST
        u64 start_ns = ktime_to_ns(ktime_get());
#endif

        for (sp = 0; sp < spp; sp++) {
                u32 *src = dma_area + frame * channels + base;

                for (ch = 0; ch < CHANNELS_PER_STREAM; ch++) {
                        u32 data = (src[ch] >> 8) | AVB_AM824_MBLA_24BIT;

                        pavpdu->audio_data[sp * CHANNELS_PER_STREAM + ch] =
                                htonl(data);
                }
                if (++frame >= buf)
                        frame = 0;
        }

#ifdef DEBUG_TICK_COST
        u64 end_ns = ktime_to_ns(ktime_get());
        pr_info_ratelimited("deinterleave cost: %llu.%03llu µs (budget 125.000 µs, %dch, %d streams)\n",
                            (end_ns - start_ns) / 1000,
                            (end_ns - start_ns) % 1000,
                            channels, adapter->num_streams);
#endif
}

static void avb_interleave_capture(struct igb_adapter *adapter,
                                   u32 *dma_area, int start_frame,
                                   const struct avb_stream *stream,
                                   const AVPDU_HEADER_NO_VLAN *cavpdu)
{
        int channels = adapter->capture_channels;
        int buf = adapter->capture_buffer_size;
        int spp = adapter->samples_per_packet;
        int base = stream->ring_slice_base;
        int frame, sp, ch;

        if (!buf || base + CHANNELS_PER_STREAM > channels)
                return;

        frame = start_frame % buf;
        if (frame < 0)
                frame += buf;

        for (sp = 0; sp < spp; sp++) {
                u32 *dst = dma_area + frame * channels + base;

                for (ch = 0; ch < CHANNELS_PER_STREAM; ch++) {
                        u32 data = cavpdu->audio_data[sp * CHANNELS_PER_STREAM + ch];

                        dst[ch] = ntohl(data >> 8);
                }
                if (++frame >= buf)
                        frame = 0;
        }
}

#define AVB_PCM_BUFFER_FRAMES 2048
#define AVB_PCM_BUFFER_BYTES  (AVB_PCM_BUFFER_FRAMES * MAX_PCM_CHANNELS * 4)

static struct snd_pcm_hardware snd_avb_playback_hw = {
        .info = (SNDRV_PCM_INFO_MMAP |
                 SNDRV_PCM_INFO_INTERLEAVED |
                 SNDRV_PCM_INFO_BLOCK_TRANSFER |
                 SNDRV_PCM_INFO_MMAP_VALID),
        .formats =          SNDRV_PCM_FMTBIT_S32_LE,
        .rates =            SNDRV_PCM_RATE_44100 |
                            SNDRV_PCM_RATE_48000 |
                            SNDRV_PCM_RATE_88200 |
                            SNDRV_PCM_RATE_96000 |
                            SNDRV_PCM_RATE_176400 |
                            SNDRV_PCM_RATE_192000,
        .rate_min =         44100,
        .rate_max =         192000,
        .channels_min =     CHANNELS_PER_STREAM,
        .channels_max =     CHANNELS_PER_STREAM,
        .buffer_bytes_max = AVB_PCM_BUFFER_BYTES,
        .period_bytes_min = 6*4*CHANNELS_PER_STREAM,
        .period_bytes_max = AVB_PCM_BUFFER_BYTES / 2,
        .periods_min =      2,
        .periods_max =      4,
};

static struct snd_pcm_hardware snd_avb_capture_hw = {
        .info = (SNDRV_PCM_INFO_MMAP |
                 SNDRV_PCM_INFO_INTERLEAVED |
                 SNDRV_PCM_INFO_BLOCK_TRANSFER |
                 SNDRV_PCM_INFO_MMAP_VALID),
        .formats =          SNDRV_PCM_FMTBIT_S32_LE,
        .rates =            SNDRV_PCM_RATE_44100 |
                            SNDRV_PCM_RATE_48000 |
                            SNDRV_PCM_RATE_88200 |
                            SNDRV_PCM_RATE_96000 |
                            SNDRV_PCM_RATE_176400 |
                            SNDRV_PCM_RATE_192000,
        .rate_min =         44100,
        .rate_max =         192000,
        .channels_min =     CHANNELS_PER_STREAM,
        .channels_max =     CHANNELS_PER_STREAM,
        .buffer_bytes_max = AVB_PCM_BUFFER_BYTES,
        .period_bytes_min = 6*4*CHANNELS_PER_STREAM,
        .period_bytes_max = AVB_PCM_BUFFER_BYTES / 2,
        .periods_min =      2,
        .periods_max =      4,
};

static bool avb_parse_mac(const char *s, u8 *dest);

static bool avb_mac_from_param(struct igb_adapter *adapter, const char *param, u8 *dest)
{
    if (param && param[0]) {
        if (avb_parse_mac(param, dest))
            return true;
        avb_warn(adapter, "ignoring invalid MAC parameter \"%s\"\n", param);
    }
    return false;
}

static bool avb_parse_mac(const char *s, u8 *dest)
{
    u8 mac[6];
    int n = 0;

    memset(mac, 0, sizeof(mac));
    for (; *s; s++) {
        int d;

        if (*s == ':' || *s == '-')
            continue;
        if (*s >= '0' && *s <= '9')
            d = *s - '0';
        else if (*s >= 'a' && *s <= 'f')
            d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F')
            d = *s - 'A' + 10;
        else
            return false;
        if (n >= 12)
            return false;
        mac[n / 2] = (mac[n / 2] << 4) | d;
        n++;
    }
    if (n != 12)
        return false;

    memcpy(dest, mac, 6);
    return true;
}

static u32 avb_tx_dest_fallback_mask;

static bool avb_talker_dest_macs_set(void)
{
    return avb_talker_dest_macs && avb_talker_dest_macs[0];
}

static void avb_tx_dest_note(struct igb_adapter *adapter, int stream_idx,
                             bool fallback, const u8 *dest)
{
    u32 bit = BIT(stream_idx);

    BUILD_BUG_ON(MAX_STREAMS > 32);

    if (fallback) {
        if (avb_tx_dest_fallback_mask & bit)
            return;
        if (avb_talker_dest_macs_set())
            avb_warn(adapter, "TX dest FALLBACK stream %d -> %pM: avb_talker_dest_macs has no valid entry for it -- transmitting to a dest nobody reserved\n",
                     stream_idx, dest);
        else if (!avb_tx_dest_fallback_mask)
            avb_info(adapter, "TX dest fallback: avb_talker_dest_macs not published yet -- streams use the unreserved fixed base %pM + stream index\n",
                     dest);
        avb_tx_dest_fallback_mask |= bit;
    } else if (unlikely(avb_tx_dest_fallback_mask & bit)) {
        avb_tx_dest_fallback_mask &= ~bit;
        if (!avb_tx_dest_fallback_mask)
            avb_info(adapter, "TX dest fallback cleared: every stream uses avb_talker_dest_macs\n");
    }
}

static DEFINE_SPINLOCK(avb_tx_dest_lock);

static void avb_stamp_talker_dest(struct igb_adapter *adapter, AVPDU_HEADER *pavpdu, int stream_idx)
{
    spin_lock(&avb_tx_dest_lock);
    memcpy(pavpdu->dest, adapter->streams_out[stream_idx].dest_mac, ETH_ALEN);
    spin_unlock(&avb_tx_dest_lock);
}

static void avb_procfs_init(struct igb_adapter *adapter);
static void avb_procfs_exit(void);

static bool avb_rx_lookup_streams(struct igb_adapter *adapter,
                                  const AVPDU_HEADER_NO_VLAN *cavpdu,
                                  unsigned long *match);

static int avb_pcm_constrain_period(struct igb_adapter *adapter,
                                    struct snd_pcm_runtime *runtime)
{
        int err;

        err = snd_pcm_hw_constraint_minmax(runtime,
                                           SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
                                           adapter->samples_per_packet,
                                           UINT_MAX);
        if (err < 0)
                return err;

        return snd_pcm_hw_constraint_integer(runtime,
                                             SNDRV_PCM_HW_PARAM_PERIODS);
}

static int snd_avb_playback_open(struct snd_pcm_substream *substream)
{
        struct igb_adapter *adapter = snd_pcm_substream_chip(substream);
        struct snd_pcm_runtime *runtime = substream->runtime;
        int err;

        runtime->hw = snd_avb_playback_hw;
        adapter->playback = 0;

        err = avb_pcm_constrain_period(adapter, runtime);
        if (err < 0)
                return err;

        return 0;
}

static int snd_avb_playback_close(struct snd_pcm_substream *substream)
{
        return 0;

}

static int snd_avb_capture_open(struct snd_pcm_substream *substream)
{
        struct igb_adapter *adapter = snd_pcm_substream_chip(substream);
        struct snd_pcm_runtime *runtime = substream->runtime;
        int err;

        runtime->hw = snd_avb_capture_hw;
        adapter->capture = 0;

        err = avb_pcm_constrain_period(adapter, runtime);
        if (err < 0)
                return err;

        return 0;
}

static int snd_avb_capture_close(struct snd_pcm_substream *substream)
{
        return 0;

}

static int snd_avb_pcm_hw_params(struct snd_pcm_substream *substream,
                             struct snd_pcm_hw_params *hw_params)
{
        int ret;

        ret = snd_pcm_lib_malloc_pages(substream,
                                   params_buffer_bytes(hw_params));

        return ret;
}

static int snd_avb_pcm_hw_free(struct snd_pcm_substream *substream)
{
        return snd_pcm_lib_free_pages(substream);
}

static int snd_avb_pcm_playback_prepare(struct snd_pcm_substream *substream)
{
        struct igb_adapter *adapter = snd_pcm_substream_chip(substream);
        struct snd_pcm_runtime *runtime = substream->runtime;

         if (runtime->rate != adapter->rate)
        {
           avb_err(adapter, "samplerate change not supported (current: %u, requested: %u)\n",
                   adapter->rate, runtime->rate);
           return -1;
        }
        adapter->rate = runtime->rate;
        adapter->playback_channels = runtime->channels;
        adapter->playback_period_size = runtime->period_size;
        adapter->playback_buffer_size = runtime->buffer_size;
        adapter->hw_playback_pointer = 0;
        adapter->p_cnt = 0;
        adapter->tx_period_acc = 0;
        /* Do not re-anchor the TX launch grid here: the listener re-aligns on the jump. */

        switch (adapter->rate)
        {
           case 44100:
           case 48000:
             adapter->samples_per_packet = 6;
             break;
           case 88200:
           case 96000:
             adapter->samples_per_packet = 12;
             break;
           case 176400:
           case 192000:
             adapter->samples_per_packet = 24;
             break;
        }

        return 0;
}

static void avb_rx_rearm_stream(struct avb_stream *s)
{
        s->rx_seq_seen = 0;
        s->rx_ts_seen = 0;
        s->rx_ts_span_ns = 0;
        s->rx_ts_frames = 0;
        s->rx_pkts_at_ts = 0;
        s->rx_ts_span_snap[0] = 0;
        s->rx_ts_span_snap[1] = 0;
        s->rx_ts_frames_snap[0] = 0;
        s->rx_ts_frames_snap[1] = 0;
        s->rx_snap_idx = 0;
        s->rx_pkts_at_snap[0] = 0;
        s->rx_pkts_at_snap[1] = 0;
        s->rx_window_ready = 0;
        s->rx_pkt_ts_seen = 0;
        s->align_offset = 0;
}

static int snd_avb_pcm_capture_prepare(struct snd_pcm_substream *substream)
{
        struct igb_adapter *adapter = snd_pcm_substream_chip(substream);
        struct snd_pcm_runtime *runtime = substream->runtime;

         if (runtime->rate != adapter->rate)
        {
           avb_err(adapter, "samplerate change not supported (current: %u, requested: %u)\n",
                   adapter->rate, runtime->rate);
           return -1;
        }

        adapter->rate = runtime->rate;
        adapter->capture_channels = runtime->channels;
        adapter->capture_period_size = runtime->period_size;
        adapter->capture_buffer_size = runtime->buffer_size;
        adapter->hw_capture_pointer = 0;
        adapter->c_cnt = 0;
        memset(runtime->dma_area, 0, runtime->dma_bytes);

        {
           int s;

           for (s = 0; s < num_streams; s++)
              avb_rx_rearm_stream(&adapter->streams_in[s]);
        }

        switch (adapter->rate)
        {
           case 44100:
           case 48000:
             adapter->samples_per_packet = 6;
             break;
           case 88200:
           case 96000:
             adapter->samples_per_packet = 12;
             break;
           case 176400:
           case 192000:
             adapter->samples_per_packet = 24;
             break;
        }

        adapter->rx_lead_frames = clamp(avb_rx_lead_packets, 0, AVB_RX_LEAD_MAX) *
                                  adapter->samples_per_packet;
        adapter->rx_lead_filled = 0;

        return 0;
}

static int snd_avb_pcm_playback_trigger(struct snd_pcm_substream *substream,
                                  int cmd)
{
        struct igb_adapter *adapter = snd_pcm_substream_chip(substream);

        switch (cmd) {
        case SNDRV_PCM_TRIGGER_START:
                adapter->playback = 1;
                printk(KERN_INFO "avb tx playback enabled\n");
                break;
        case SNDRV_PCM_TRIGGER_STOP:
                adapter->playback = 0;
                printk(KERN_INFO "avb tx playback disabled\n");
                break;
        default:
                return -EINVAL;
        }
        return 0;
}

static int snd_avb_pcm_capture_trigger(struct snd_pcm_substream *substream,
                                  int cmd)
{
        struct igb_adapter *adapter = snd_pcm_substream_chip(substream);

        switch (cmd) {
        case SNDRV_PCM_TRIGGER_START:
                if (!adapter->avb_listener_ready) {
                        avb_err(adapter, "refusing capture start -- stream 0 has no listener identity (set avb_listener_dest_macs/avb_listener_src_macs or avb_device_talker_mac/avb_device_source_mac)\n");
                        return -ENODEV;
                }
                adapter->capture = 1;
                break;
        case SNDRV_PCM_TRIGGER_STOP:
                adapter->capture = 0;
                udelay(300);
                break;
        default:
                return -EINVAL;
        }
       return 0;
}

static snd_pcm_uframes_t
snd_avb_pcm_playback_pointer(struct snd_pcm_substream *substream)
{
        struct igb_adapter *adapter = snd_pcm_substream_chip(substream);

        return adapter->hw_playback_pointer / adapter->playback_channels;
}

static snd_pcm_uframes_t
snd_avb_pcm_capture_pointer(struct snd_pcm_substream *substream)
{
        struct igb_adapter *adapter = snd_pcm_substream_chip(substream);

        return adapter->hw_capture_pointer / adapter->capture_channels;
}

static struct snd_pcm_ops snd_avb_playback_ops = {
        .open =        snd_avb_playback_open,
        .close =       snd_avb_playback_close,
        .ioctl =       snd_pcm_lib_ioctl,
        .hw_params =   snd_avb_pcm_hw_params,
        .hw_free =     snd_avb_pcm_hw_free,
        .prepare =     snd_avb_pcm_playback_prepare,
        .trigger =     snd_avb_pcm_playback_trigger,
        .pointer =     snd_avb_pcm_playback_pointer,
};

static struct snd_pcm_ops snd_avb_capture_ops = {
        .open =        snd_avb_capture_open,
        .close =       snd_avb_capture_close,
        .ioctl =       snd_pcm_lib_ioctl,
        .hw_params =   snd_avb_pcm_hw_params,
        .hw_free =     snd_avb_pcm_hw_free,
        .prepare =     snd_avb_pcm_capture_prepare,
        .trigger =     snd_avb_pcm_capture_trigger,
        .pointer =     snd_avb_pcm_capture_pointer,
};

static void avb_tx_dest_apply(struct igb_adapter *adapter);

static int snd_avb_new_pcm(struct igb_adapter *adapter)
{
        struct snd_pcm *pcm;
        int err;
        int i;
        int stream_i = 0;

        u8 src[6];
        u8 stream_id[8];

        err = snd_pcm_new(adapter->card, "AVB", 0, 1, 1, &pcm);
         if (err < 0)
                 avb_err(adapter, "snd_pcm_new failed: %d\n", err);
          else
                  avb_info(adapter, "PCM device created successfully\n");
        if (err < 0)
                return err;

        pcm->private_data = adapter;
        sprintf(pcm->name, "igb AVB %dch S32_LE", channels);
        adapter->pcm = pcm;

        snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
                        &snd_avb_playback_ops);
        snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE,
                        &snd_avb_capture_ops);

        pcm->nonatomic = 0;

         avb_info(adapter, "preallocating DMA pages for PCM buffers\n");

        snd_pcm_lib_preallocate_pages_for_all
            (pcm, SNDRV_DMA_TYPE_CONTINUOUS, 
             NULL,
             AVB_PCM_BUFFER_BYTES, AVB_PCM_BUFFER_BYTES);

        adapter->avb_tx_groups = adapter->tx_ring[0]->count / 2;
        if (adapter->avb_tx_groups == 0)
                adapter->avb_tx_groups = MAXDESCRIPTORS >> 1;
        adapter->tx_dma_bytes = adapter->avb_tx_groups * MAXPACKETSIZE;
        {
                u32 need = (AVB_TX_SCHED_LEAD_NS / AVB_TX_INTERVAL_NS + 1) *
                           num_streams + num_streams;

                avb_info(adapter, "TX ring: %u desc -> %u groups, %u KiB slots (need >=%u groups for %d streams)\n",
                         adapter->tx_ring[0]->count, adapter->avb_tx_groups,
                         adapter->tx_dma_bytes >> 10, need, num_streams);
                if (adapter->avb_tx_groups < need)
                        avb_warn(adapter, "TX ring too small for %d streams (have %u groups, need >=%u) -- expect tx_xrun; raise tx_size (avb_up.sh auto-sizes it)\n",
                                 num_streams, adapter->avb_tx_groups, need);
        }

         avb_info(adapter, "allocating TX DMA buffer\n");
        adapter->tx_addr = dma_alloc_coherent(&adapter->pdev->dev, adapter->tx_dma_bytes,
                                           &adapter->tx_physaddr, GFP_KERNEL);
        if (adapter->tx_addr == NULL) {
                avb_err(adapter, "TX DMA buffer allocation failed\n");
                return -ENOMEM;
        }

         avb_info(adapter, "allocating RX DMA buffer\n");
        adapter->rx_addr = dma_alloc_coherent(&adapter->pdev->dev, 4096*16*4,
                                           &adapter->rx_physaddr, GFP_KERNEL);
        if (adapter->rx_addr == NULL) {
                avb_err(adapter, "RX DMA buffer allocation failed\n");
                if (adapter->tx_addr) {
                        dma_free_coherent(&adapter->pdev->dev, adapter->tx_dma_bytes,
                                          adapter->tx_addr, adapter->tx_physaddr);
                        adapter->tx_addr = 0;
                }
                return -ENOMEM;
        }

        memset(adapter->tx_addr, 0, adapter->tx_dma_bytes);

         if (!avb_mac_from_param(adapter, own_mac, src))
            memcpy(src, adapter->netdev->dev_addr, 6);
          u64_to_array8(array6_to_u64(src) << 16, stream_id);
           avb_info(adapter, "talker stream: src=%pM stream_id=%016llx\n", src, (unsigned long long)array6_to_u64(src) << 16);

        for (stream_i = 0; stream_i < num_streams; stream_i++) {
            adapter->streams_out[stream_i].stream_id = AVB_STREAM_ID(array6_to_u64(src), stream_i);
           adapter->streams_out[stream_i].vlan = 0x6002;
           adapter->streams_out[stream_i].format = 0;
           adapter->streams_out[stream_i].channels = CHANNELS_PER_STREAM;
           adapter->streams_out[stream_i].samplerate = adapter->rate;
        }

        kernel_param_lock(THIS_MODULE);
        avb_tx_dest_apply(adapter);
        kernel_param_unlock(THIS_MODULE);

        for (i=0; i<(int)adapter->avb_tx_groups; i++) {
           u8 stream_id_be[8];
           int s = i % num_streams;

           u64_to_array8(adapter->streams_out[s].stream_id, stream_id_be);
           init_silent_avpdu_header(adapter->tx_addr + i*1024,
                                    adapter->streams_out[s].dest_mac,
                                    src,
                                    stream_id_be,
                                    adapter->streams_out[s].channels,
                                    adapter->streams_out[s].samplerate);
        }

        for (stream_i = 0; stream_i < num_streams; stream_i++)
           printk(KERN_INFO "snd_igb_avb: TX framing from streams_out[%03d] id=%016llx dest=%pM\n",
                  stream_i,
                  (unsigned long long)adapter->streams_out[stream_i].stream_id,
                  adapter->streams_out[stream_i].dest_mac);

        return 0;
}

#define AVB_FRAME_L1_OVERHEAD_BYTES 20

static u32 avb_stream_idleslope_bps(int rate, int samples_per_packet)
{
        int wire_bytes_per_frame =
                54 + AVB_FRAME_L1_OVERHEAD_BYTES + 32 * samples_per_packet;
        int packet_rate = rate / samples_per_packet;

        return wire_bytes_per_frame * 8 * packet_rate;
}

static u64 avb_aggregate_idleslope_bps(struct igb_adapter *adapter)
{
        int rate = adapter->rate;
        int samples_per_packet = adapter->samples_per_packet;
        int num_streams = adapter->num_streams;
        u32 per_stream_bps;
        u64 aggregate_bps;

        per_stream_bps = avb_stream_idleslope_bps(rate, samples_per_packet);
        aggregate_bps = (u64)per_stream_bps * num_streams;

        adapter->aggregate_idleslope_bps = aggregate_bps;
        adapter->reserved_bw_permille = (u32)((aggregate_bps * 1000) / 1000000000ULL);

        avb_info(adapter, "CBS calc: per-stream=%u bps, aggregate=%llu bps, reserved=%u‰ of link (streams=%d)\n",
                 per_stream_bps, (unsigned long long)aggregate_bps,
                 adapter->reserved_bw_permille, num_streams);

        if (adapter->reserved_bw_permille > 750) {
                avb_warn(adapter, "SR reservation exceeds 75%% link (%u‰), clamp num_streams or reduce rate\n",
                         adapter->reserved_bw_permille);
        }

        return aggregate_bps;
}

static struct igb_adapter *avb_listener_adapter;

static u64 avb_rx_unmatched;

static int avb_list_field(const char *list, int idx, char *buf, size_t len)
{
    const char *end;
    size_t n;

    buf[0] = '\0';
    if (!list)
        return 0;
    for (; idx > 0; idx--) {
        list = strchr(list, ',');
        if (!list)
            return 0;
        list++;
    }
    end = strchrnul(list, ',');
    while (list < end && isspace(*list))
        list++;
    while (end > list && isspace(end[-1]))
        end--;
    n = end - list;
    if (!n)
        return 0;
    strscpy(buf, list, min(n + 1, len));
    return n < len ? 1 : -1;
}

static bool avb_listener_mac(struct igb_adapter *adapter, const char *name,
                             const char *list, int k, const char *fallback,
                             u8 *dest)
{
    char buf[32];
    u8 mac[ETH_ALEN];
    int r = avb_list_field(list, k, buf, sizeof(buf));

    if (r > 0 && avb_parse_mac(buf, mac)) {
        if (!is_zero_ether_addr(mac)) {
            memcpy(dest, mac, ETH_ALEN);
            return true;
        }
    } else if (r) {
        avb_warn(adapter, "%s: ignoring invalid entry %d \"%s\"\n", name, k, buf);
    }
    return k == 0 && avb_mac_from_param(adapter, fallback, dest);
}

static bool avb_listener_sid(struct igb_adapter *adapter, int k, u64 *sid)
{
    char buf[32];
    u64 v;
    int r = avb_list_field(avb_listener_stream_ids, k, buf, sizeof(buf));

    if (r > 0 && !kstrtou64(buf, 16, &v)) {
        if (!v)
            return false;
        *sid = v;
        return true;
    }
    if (r)
        avb_warn(adapter, "avb_listener_stream_ids: ignoring invalid entry %d \"%s\"\n", k, buf);
    return false;
}

static u8 avb_rx_talker_src[AVB_RX_HW_FILTERS][ETH_ALEN];

static void avb_rx_set_talker_filters(struct igb_adapter *adapter, bool force)
{
    u8 want[AVB_RX_HW_FILTERS][ETH_ALEN] = { { 0 } };
    int i, k;

    if (avb_rx_bench)
        return;
    if (force)
        memset(avb_rx_talker_src, 0, sizeof(avb_rx_talker_src));

    for (i = 0; i < AVB_RX_HW_FILTERS; i++) {
        if (is_zero_ether_addr(avb_rx_talker_src[i]))
            continue;
        for (k = 0; k < num_streams; k++)
            if (ether_addr_equal(adapter->streams_in[k].src_mac, avb_rx_talker_src[i]))
                break;
        if (k < num_streams)
            memcpy(want[i], avb_rx_talker_src[i], ETH_ALEN);
    }
    for (k = 0; k < num_streams; k++) {
        const u8 *src = adapter->streams_in[k].src_mac;
        int free_slot = -1;

        if (is_zero_ether_addr(src))
            continue;
        for (i = 0; i < AVB_RX_HW_FILTERS; i++) {
            if (ether_addr_equal(want[i], src))
                break;
            if (free_slot < 0 && is_zero_ether_addr(want[i]))
                free_slot = i;
        }
        if (i < AVB_RX_HW_FILTERS)
            continue;
        if (free_slot < 0) {
            avb_warn(adapter, "rx: more than %d talkers -- stream %d from %pM gets no filter and will not arrive\n",
                     AVB_RX_HW_FILTERS, k, src);
            continue;
        }
        memcpy(want[free_slot], src, ETH_ALEN);
    }

    for (i = 0; i < AVB_RX_HW_FILTERS; i++) {
        unsigned long flags;

        if (!force && ether_addr_equal(want[i], avb_rx_talker_src[i]))
            continue;
        spin_lock_irqsave(&adapter->rx_ident_lock, flags);
        igb_clear_flex_filter(adapter, i);
        if (!is_zero_ether_addr(want[i])) {
            u8 filter[AVB_RX_FILTER_LEN];
            u8 filter_mask[AVB_RX_FILTER_LEN / 8];

            avb_fill_talker_rx_filter(want[i], filter, filter_mask);
            igb_setup_flex_filter(adapter, 1, i, AVB_RX_FILTER_LEN,
                                  filter, filter_mask);
        }
        spin_unlock_irqrestore(&adapter->rx_ident_lock, flags);
        if (!is_zero_ether_addr(want[i]))
            avb_info(adapter, "rx talker filter %d: all AVTP from %pM (sw-demux)\n", i, want[i]);
        else if (!is_zero_ether_addr(avb_rx_talker_src[i]))
            avb_info(adapter, "rx talker filter %d: cleared (talker %pM gone)\n", i, avb_rx_talker_src[i]);
        memcpy(avb_rx_talker_src[i], want[i], ETH_ALEN);
    }
}

static void avb_rx_set_identities(struct igb_adapter *adapter, bool force)
{
    bool changed = false;
    int k, j;

    for (k = 0; k < num_streams; k++) {
        struct avb_stream *s = &adapter->streams_in[k];
        u8 dest[ETH_ALEN] = { 0 }, src[ETH_ALEN] = { 0 };
        int slot = avb_rx_hw_filter_slot(k);
        unsigned long flags;
        bool has_dest, has_src, bound;
        int filter_id;
        u64 sid = 0;

        has_dest = avb_listener_mac(adapter, "avb_listener_dest_macs",
                                    avb_listener_dest_macs, k,
                                    avb_device_talker_mac, dest);
        has_src = avb_listener_mac(adapter, "avb_listener_src_macs",
                                   avb_listener_src_macs, k,
                                   avb_device_source_mac, src);
        bound = has_dest && has_src;
        if (bound) {
            if (!avb_listener_sid(adapter, k, &sid))
                sid = (array6_to_u64(src) << 16) | k;
        } else {
            eth_zero_addr(dest);
            eth_zero_addr(src);
        }
        filter_id = bound ? slot : -1;

        if (!force && ether_addr_equal(s->dest_mac, dest) &&
            ether_addr_equal(s->src_mac, src) &&
            s->expected_stream_id == sid && s->rx_filter_id == filter_id)
            continue;
        changed = true;

        spin_lock_irqsave(&adapter->rx_ident_lock, flags);
        if (slot >= 0)
            igb_clear_flex_filter(adapter, slot);
        memcpy(s->dest_mac, dest, ETH_ALEN);
        memcpy(s->src_mac, src, ETH_ALEN);
        s->stream_id = sid;
        s->expected_stream_id = sid;
        s->rx_filter_id = filter_id;
        s->vlan = bound ? 0x6002 : 0;
        s->format = 0;
        s->channels = bound ? CHANNELS_PER_STREAM : 0;
        s->samplerate = bound ? adapter->rate : 0;
        avb_rx_rearm_stream(s);
        if (filter_id >= 0) {
            u8 filter[AVB_RX_FILTER_LEN];
            u8 filter_mask[AVB_RX_FILTER_LEN / 8];

            avb_fill_rx_filter(s, filter, filter_mask);
            igb_setup_flex_filter(adapter, 1, filter_id, AVB_RX_FILTER_LEN,
                                  filter, filter_mask);
        }
        spin_unlock_irqrestore(&adapter->rx_ident_lock, flags);

        if (!bound)
            avb_info(adapter, "rx filter stream %d unbound (no listener MACs) filter_id=-1\n", k);
        else if (filter_id >= 0)
            avb_info(adapter, "rx filter stream %d talker=%pM device=%pM sid=%016llx filter_id=%d\n",
                     k, s->dest_mac, s->src_mac,
                     (unsigned long long)sid, filter_id);
        else
            avb_info(adapter, "rx filter stream %d talker=%pM device=%pM sid=%016llx sw-demux (>%d streams) filter_id=-1\n",
                     k, s->dest_mac, s->src_mac,
                     (unsigned long long)sid, AVB_RX_HW_FILTERS);
    }

    for (k = 0; changed && k < num_streams; k++) {
        u64 sid = adapter->streams_in[k].expected_stream_id;

        if (!sid)
            continue;
        for (j = 0; j < k; j++)
            if (adapter->streams_in[j].expected_stream_id == sid)
                break;
        if (j < k)
            avb_info(adapter, "rx: stream %d shares sid %016llx with stream %d (fan-out)\n",
                     k, (unsigned long long)sid, j);
    }

    if (num_streams > AVB_RX_HW_FILTERS)
        avb_rx_set_talker_filters(adapter, force);

    if (adapter->avb_listener_ready && is_zero_ether_addr(adapter->streams_in[0].dest_mac) &&
        adapter->capture)
        avb_warn(adapter, "stream 0 unbound while capturing -- capture stalls until it is bound again\n");
    adapter->avb_listener_ready = avb_rx_bench ||
                                  !is_zero_ether_addr(adapter->streams_in[0].dest_mac);
}

static int avb_listener_macs_set(const char *val, const struct kernel_param *kp)
{
    int err = param_set_charp(val, kp);

    if (!err && avb_listener_adapter)
        avb_rx_set_identities(avb_listener_adapter, false);
    return err;
}

static const struct kernel_param_ops avb_listener_macs_ops = {
    .set = avb_listener_macs_set,
    .get = param_get_charp,
    .free = param_free_charp,
};

module_param_cb(avb_listener_dest_macs, &avb_listener_macs_ops,
                &avb_listener_dest_macs, 0644);
MODULE_PARM_DESC(avb_listener_dest_macs, "Per-stream listener (RX) stream dest MACs, comma-separated; empty entry = unbound; writable at runtime");
module_param_cb(avb_listener_src_macs, &avb_listener_macs_ops,
                &avb_listener_src_macs, 0644);
MODULE_PARM_DESC(avb_listener_src_macs, "Per-stream listener (RX) talker source MACs, comma-separated; empty entry = unbound; writable at runtime");
module_param_cb(avb_listener_stream_ids, &avb_listener_macs_ops,
                &avb_listener_stream_ids, 0644);
MODULE_PARM_DESC(avb_listener_stream_ids, "Per-stream listener (RX) stream ids (16 hex digits), comma-separated; empty entry = talker source MAC << 16 | k; writable at runtime");

static void avb_tx_dest_apply(struct igb_adapter *adapter)
{
    int k;

    for (k = 0; k < num_streams; k++) {
        char buf[32];
        u8 dest[ETH_ALEN];
        int r = avb_list_field(avb_talker_dest_macs, k, buf, sizeof(buf));
        bool from_list = r > 0 && avb_parse_mac(buf, dest) &&
                         !is_zero_ether_addr(dest);

        if (r && !from_list)
            avb_warn(adapter, "avb_talker_dest_macs: ignoring invalid entry %d \"%s\"\n", k, buf);
        if (!from_list)
            u64_to_array6(MAAP_LOCALLY_ADMINISTRATORED_BASE + k, dest);

        if (!ether_addr_equal(adapter->streams_out[k].dest_mac, dest)) {
            spin_lock_bh(&avb_tx_dest_lock);
            memcpy(adapter->streams_out[k].dest_mac, dest, ETH_ALEN);
            spin_unlock_bh(&avb_tx_dest_lock);
            if (from_list)
                avb_info(adapter, "talker dest stream %d -> %pM (avb_talker_dest_macs)\n", k, dest);
        }
        avb_tx_dest_note(adapter, k, !from_list, dest);
    }
}

static int avb_talker_dest_macs_param_set(const char *val, const struct kernel_param *kp)
{
    int err = param_set_charp(val, kp);

    if (!err && avb_listener_adapter)
        avb_tx_dest_apply(avb_listener_adapter);
    return err;
}

static const struct kernel_param_ops avb_talker_dest_macs_ops = {
    .set = avb_talker_dest_macs_param_set,
    .get = param_get_charp,
    .free = param_free_charp,
};

module_param_cb(avb_talker_dest_macs, &avb_talker_dest_macs_ops,
                &avb_talker_dest_macs, 0644);
MODULE_PARM_DESC(avb_talker_dest_macs, "Per-stream talker (TX) dest MACs, comma-separated; missing/invalid entry = fixed base + stream index; writable at runtime");

int snd_avb_probe(struct igb_adapter *adapter, int samplerate)
{
        struct snd_card *card;

        int err;

         int stream_i = 0;

         adapter->card = 0;
         adapter->capture = 0;
         adapter->playback = 0;

         clamp_num_streams(adapter);
         channels = num_streams * CHANNELS_PER_STREAM;
         snd_avb_playback_hw.channels_min = channels;
         snd_avb_playback_hw.channels_max = channels;
         snd_avb_capture_hw.channels_min = channels;
         snd_avb_capture_hw.channels_max = channels;

          for (stream_i = 0; stream_i < num_streams; stream_i++) {
              memset(&adapter->streams_in[stream_i], 0, sizeof(adapter->streams_in[stream_i]));
              memset(&adapter->streams_out[stream_i], 0, sizeof(adapter->streams_out[stream_i]));
              adapter->streams_in[stream_i].ring_slice_base = stream_i * 8;
              adapter->streams_in[stream_i].ring_slice_chans = CHANNELS_PER_STREAM;
          }
          printk(KERN_INFO "snd_igb_avb: %d avb_stream slots initialised\n", num_streams);

          spin_lock_init(&adapter->rx_ident_lock);

        switch (samplerate)
        {
           case 44100:
               adapter->samples_per_packet = 6;
               snd_avb_playback_hw.rates = SNDRV_PCM_RATE_44100;
               snd_avb_capture_hw.rates = SNDRV_PCM_RATE_44100;
               break;
           case 48000:
               snd_avb_playback_hw.rates = SNDRV_PCM_RATE_48000;
               snd_avb_capture_hw.rates = SNDRV_PCM_RATE_48000;
               adapter->samples_per_packet = 6;
               break;
           case 88200:
               snd_avb_playback_hw.rates = SNDRV_PCM_RATE_88200;
               snd_avb_capture_hw.rates = SNDRV_PCM_RATE_88200;
               adapter->samples_per_packet = 12;
               break;
           case 96000:
               snd_avb_playback_hw.rates = SNDRV_PCM_RATE_96000;
               snd_avb_capture_hw.rates = SNDRV_PCM_RATE_96000;
               adapter->samples_per_packet = 12;
               break;
           case 176400:
               snd_avb_playback_hw.rates = SNDRV_PCM_RATE_176400;
               snd_avb_capture_hw.rates = SNDRV_PCM_RATE_176400;
               adapter->samples_per_packet = 24;
               break;
           case 192000:
               snd_avb_playback_hw.rates = SNDRV_PCM_RATE_192000;
               snd_avb_capture_hw.rates = SNDRV_PCM_RATE_192000;
               adapter->samples_per_packet = 24;
               break;
           default:
             avb_err(adapter, "Invalid sample rate %d\n", samplerate);
             return -1;
        }

         adapter->rate = samplerate;
         adapter->num_streams = num_streams;

         avb_aggregate_idleslope_bps(adapter);

         avb_info(adapter, "initializing AVB device (rate=%dHz, %dch, %d streams)\n",
                  samplerate, channels, num_streams);
        if (dev >= SNDRV_CARDS)
                return -ENODEV;

        if (!enable[dev]) {
                dev++;
                return -ENOENT;
        }

        err = snd_card_new(&adapter->pdev->dev, index[dev], id[dev], THIS_MODULE, 0, &card);
        if (err < 0) {
                avb_err(adapter, "failed to create ALSA card: %d\n", err);
                return err;
        }
        adapter->card = card;

        err = snd_avb_new_pcm(adapter);
        if (err < 0) {
                avb_err(adapter, "failed to create PCM device: %d\n", err);
                goto error;
        }

        strcpy(card->driver, "IGB-AVB");
        strcpy(card->shortname, "Intel igb AVB");
        sprintf(card->longname, "%s at %s, %dch S32_LE 44.1-192kHz",
                card->shortname, pci_name(adapter->pdev), channels);
        if (id[dev] == NULL || id[dev][0] == '\0')
                strcpy(card->id, "igbavb");

        err = snd_card_register(card);
        if (err < 0) {
                avb_err(adapter, "failed to register ALSA card: %d\n", err);
                goto error;
        }

        avb_info(adapter, "ALSA card registered (id=%s, name=%s)\n", card->id, card->shortname);
        dev++;

        kernel_param_lock(THIS_MODULE);
        avb_rx_unmatched = 0;
        avb_rx_set_identities(adapter, true);
        avb_tx_dest_apply(adapter);
        avb_listener_adapter = adapter;
        kernel_param_unlock(THIS_MODULE);

         if (avb_rx_bench)
                 avb_bench_setup_filter(adapter, NULL);

        {
           union e1000_adv_rx_desc *desc = adapter->rx_ring[1]->desc;

           int i;

           for (i=0; i<MAXDESCRIPTORS;i++)
           {
              desc[i].read.pkt_addr = adapter->rx_physaddr + MAXPACKETSIZE * i;
              desc[i].read.hdr_addr = 0;
           }

	   wmb();
	   writel(16, adapter->rx_ring[1]->tail);
        }

          if (avb_rx_bench)
                  avb_bench_init(adapter);

          avb_procfs_init(adapter);

          hrtimer_setup(&adapter->avb_tx_timer, avb_tx_timer_fn,
                        CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
          adapter->tx_engine_ticks = 0;
          adapter->tx_silence_packets = 0;
          adapter->tx_phc_pres = 0;
          adapter->tx_phc_launch = 0;
          adapter->tx_late_launch = 0;
          adapter->tx_xrun = 0;
          adapter->tx_gated = 1;
          adapter->tx_gated_ticks = 0;
          adapter->tx_gptp_offset_ns = 0;
          adapter->tx_seq = 0;
          adapter->tx_dbc = 0;
          adapter->tx_avb_desc_group = 0;
          adapter->tx_next_launch_ns = 0;
          adapter->tx_period_acc = 0;
          adapter->rx_desc_backlog = 0;
          adapter->rx_desc_backlog_max = 0;
          adapter->rx_ring_near_lap = 0;
          adapter->rx_budget_hits = 0;
          adapter->rx_packets_total = 0;
          adapter->capture_frames_total = 0;
          adapter->rx_last_pres_delta_ns = 0;
          adapter->rx_last_launch_delta_ns = 0;
          adapter->tx_timer_active = 1;
          hrtimer_start(&adapter->avb_tx_timer, ns_to_ktime(AVB_TX_INTERVAL_NS),
                        HRTIMER_MODE_REL_SOFT);

         return 0;

error:
        avb_procfs_exit();
        snd_card_free(card);
        return err;
}

void snd_avb_remove(struct igb_adapter *adapter)
{
        int stream_i;

        avb_info(adapter, "shutting down AVB device\n");

        adapter->capture = 0;
        adapter->playback = 0;
        adapter->tx_timer_active = 0;

        hrtimer_cancel(&adapter->avb_tx_timer);

        if (avb_rx_bench)
                avb_bench_exit();

        udelay(300);

        kernel_param_lock(THIS_MODULE);
        avb_listener_adapter = NULL;
        kernel_param_unlock(THIS_MODULE);

        for (stream_i = 0; stream_i < AVB_RX_HW_FILTERS; stream_i++)
                igb_clear_flex_filter(adapter, stream_i);

        if (adapter->tx_addr) {
                dma_free_coherent(&adapter->pdev->dev, adapter->tx_dma_bytes,
                                  adapter->tx_addr, adapter->tx_physaddr);
                adapter->tx_addr = 0;
        }

        if (adapter->rx_addr) {
                dma_free_coherent(&adapter->pdev->dev, 4096*16*4,
                                  adapter->rx_addr, adapter->rx_physaddr);
                adapter->rx_addr = 0;
        }

        if (adapter->card) {
                avb_info(adapter, "freeing ALSA card\n");
                snd_card_free(adapter->card);
                adapter->card = 0;
        }

        avb_procfs_exit();

        avb_info(adapter, "AVB device shutdown complete\n");
}

static s64 avb_rx_drift_ppb(const struct avb_stream *stream, int rate)
{
        s64 nominal;

        if (!rate || !stream->rx_ts_frames)
                return 0;

        nominal = (s64)div_u64(stream->rx_ts_frames * NSEC_PER_SEC, rate);
        if (!nominal)
                return 0;

        return div64_s64((stream->rx_ts_span_ns - nominal) * 1000000000LL,
                         nominal);
}

static s64 avb_rx_drift_ppb_sliding(const struct avb_stream *stream, int rate,
                                    int *settling)
{
        s64 delta_span, delta_frames, nominal;
        u64 span0, span1, frames0, frames1;

        *settling = 0;

        if (!stream->rx_window_ready)
                *settling = 1;

        span0 = stream->rx_ts_span_snap[0];
        span1 = stream->rx_ts_span_snap[1];
        frames0 = stream->rx_ts_frames_snap[0];
        frames1 = stream->rx_ts_frames_snap[1];

        if (!span0 && !span1 && !frames0 && !frames1)
                return 0;

        if (!frames1)
                return 0;

        delta_span = (s64)(span1 - span0);
        delta_frames = (s64)(frames1 - frames0);

        if (!delta_frames)
                return 0;

        nominal = (s64)div_s64(delta_frames * NSEC_PER_SEC, rate);
        if (!nominal)
                return 0;

        return div_s64((delta_span - nominal) * 1000000000LL, nominal);
}

static void avb_rx_check_integrity(struct avb_stream *stream,
                                   const AVPDU_HEADER_NO_VLAN *cavpdu,
                                   int spp, int rate)
{
        if (stream->rx_seq_seen) {
                u8 expect = (u8)(stream->last_sequence + 1);

                if (cavpdu->sequence_number != expect) {
                        stream->rx_last_seq_gap =
                                (int)(u8)(cavpdu->sequence_number - expect);
                        stream->rx_seq_errors++;
                        printk_ratelimited(KERN_WARNING
                                "avb rx: seq discontinuity sid=%016llx got=%u expected=%u gap=%d\n",
                                (unsigned long long)stream->expected_stream_id,
                                cavpdu->sequence_number, expect,
                                stream->rx_last_seq_gap);
                }
        }
        stream->last_sequence = cavpdu->sequence_number;
        stream->rx_seq_seen = 1;

        if (rate && (cavpdu->flags1 & TIMESTAMP_VALID) && cavpdu->timestamp) {
                u32 ts = ntohl(cavpdu->timestamp);

                if (stream->rx_ts_seen) {
                        u64 frames = (stream->rx_packets - stream->rx_pkts_at_ts) *
                                     (u64)spp;
                        s64 nominal = (s64)div_u64(frames * NSEC_PER_SEC, rate);
                        s64 step = (s32)(ts - stream->rx_last_ts);
                        s64 err = step - nominal;

                        stream->rx_ts_span_ns += step;
                        stream->rx_ts_frames  += frames;

                        if (err < -avb_rx_slip_ns || err > avb_rx_slip_ns) {
                                stream->rx_last_slip_ns = (int)err;
                                stream->rx_clock_slips++;
                                printk_ratelimited(KERN_WARNING
                                        "avb rx: media-clock slip sid=%016llx %lldns over %llu frames "
                                        "(peer not locked to the stream clock?)\n",
                                        (unsigned long long)stream->expected_stream_id,
                                        (long long)err, (unsigned long long)frames);
                        }
                }
                stream->rx_last_ts = ts;
                stream->rx_pkts_at_ts = stream->rx_packets;
                stream->rx_ts_seen = 1;

                if (stream->rx_packets -
                    stream->rx_pkts_at_snap[1 - stream->rx_snap_idx] >= 0x2000) {
                        int idx = stream->rx_snap_idx;

                        stream->rx_ts_span_snap[idx] = stream->rx_ts_span_ns;
                        stream->rx_ts_frames_snap[idx] = stream->rx_ts_frames;
                        stream->rx_pkts_at_snap[idx] = stream->rx_packets;

                        stream->rx_snap_idx = 1 - stream->rx_snap_idx;

                        if (!stream->rx_window_ready && stream->rx_ts_frames_snap[1] > 0) {
                                stream->rx_window_ready = 1;
                        }

                        if (avb_rx_drift_warn_ppb && idx == 1) {
                                int settling;
                                s64 ppb = avb_rx_drift_ppb_sliding(stream, rate, &settling);

                                if (!settling && (ppb > avb_rx_drift_warn_ppb || ppb < -avb_rx_drift_warn_ppb))
                                        printk_ratelimited(KERN_WARNING
                                                "avb rx: peer media clock off by %lld ppb sid=%016llx -- "
                                                "peer is not locked to this stream; expect periodic sample splices\n",
                                                (long long)ppb,
                                                (unsigned long long)stream->expected_stream_id);
                        }
                }
        }
}

static void avb_rx_track_media_time(struct avb_stream *stream,
                                    const AVPDU_HEADER_NO_VLAN *cavpdu,
                                    int spp, int rate)
{
        u32 syt_interval = spp * 4 / 3;
        u32 idx;

        if (!rate || !syt_interval)
                return;

        if ((cavpdu->flags1 & TIMESTAMP_VALID) && cavpdu->timestamp) {
                idx = (syt_interval - cavpdu->data_block_continuity % syt_interval) %
                      syt_interval;
                if (idx < spp) {
                        stream->rx_pkt_ts = ntohl(cavpdu->timestamp) -
                                (u32)div_u64((u64)idx * NSEC_PER_SEC, rate);
                        stream->rx_pkt_ts_seen = 1;
                        return;
                }
        }
        if (stream->rx_pkt_ts_seen)
                stream->rx_pkt_ts += (u32)div_u64((u64)spp * NSEC_PER_SEC, rate);
}

static bool avb_rx_align_frames(struct igb_adapter *adapter,
                                const struct avb_stream *stream, int *offset)
{
        const struct avb_stream *ref = &adapter->streams_in[0];
        s64 num;

        if (stream == ref) {
                *offset = 0;
                return true;
        }
        if (!adapter->rate || !ref->rx_pkt_ts_seen || !stream->rx_pkt_ts_seen)
                return false;

        num = (s64)(s32)(stream->rx_pkt_ts - ref->rx_pkt_ts) * adapter->rate;
        num += num < 0 ? -(s64)NSEC_PER_SEC / 2 : (s64)NSEC_PER_SEC / 2;
        *offset = (int)div_s64(num, NSEC_PER_SEC);
        return true;
}

void snd_avb_receive(struct igb_adapter *adapter)
{
     static int i = 0;
     int tmp = adapter->rx_ring[1]->next_to_clean;
     int processed = 0;
     const int budget = avb_rx_desc_budget();
     u32 rdh, backlog;

     union e1000_adv_rx_desc *rx_desc = adapter->rx_ring[1]->desc;

     int oldcptr = adapter->hw_capture_pointer;
     int newcptr = oldcptr;

     int samples_per_packet = adapter->samples_per_packet;
     int lead = adapter->rx_lead_filled;
     u32 presentation_time = 0;
     u32 launch_time_l = 0;
     u64 launch_time = 0;

     struct snd_pcm_substream *csubs = ((struct snd_pcm *)adapter->pcm)->streams[SNDRV_PCM_STREAM_CAPTURE].substream;

     int capture = adapter->capture;
     int c_cnt = adapter->c_cnt;

     u32 *dma_area = NULL;

     if (csubs && csubs->runtime)
        dma_area = (u32 *)csubs->runtime->dma_area;

     rdh = E1000_READ_REG(&adapter->hw, E1000_RDH(adapter->rx_ring[1]->reg_idx));
     backlog = (rdh + MAXDESCRIPTORS - tmp) % MAXDESCRIPTORS;
     adapter->rx_desc_backlog = backlog;
     if (backlog > adapter->rx_desc_backlog_max)
        adapter->rx_desc_backlog_max = backlog;
     if (backlog > MAXDESCRIPTORS - AVB_RX_LAP_HEADROOM) {
        adapter->rx_ring_near_lap++;
        printk_ratelimited(KERN_WARNING
                "avb rx: descriptor backlog %u/%u -- within %u of a full ring lap\n",
                backlog, MAXDESCRIPTORS, AVB_RX_LAP_HEADROOM);
     }

     spin_lock(&adapter->rx_ident_lock);

     while (processed < budget &&
            (READ_ONCE(rx_desc[tmp].wb.upper.status_error) & 1))
     {
         AVPDU_HEADER_NO_VLAN *cavpdu;

         processed++;

         /* DD is the only NIC handshake: payload loads must not be hoisted above it. */
         dma_rmb();

         cavpdu = (AVPDU_HEADER_NO_VLAN *)(adapter->rx_addr + MAXPACKETSIZE * tmp + 16);

         presentation_time = ntohl(cavpdu->timestamp) + PRESENTATION_TIME_OFFSET;
         presentation_time = htonl(presentation_time);

         launch_time_l = *(u32 *)(adapter->rx_addr + MAXPACKETSIZE * tmp + 8);
         launch_time = avb_encode_launch_time(launch_time_l);

         if (adapter->tx_phc_pres) {
            u32 phc_pres = ntohl(adapter->tx_phc_pres);
            u32 rx_pres = ntohl(presentation_time);
            u32 rx_timestamp = ntohl(cavpdu->timestamp);
            int rx_pres_valid = rx_timestamp && (cavpdu->flags1 & TIMESTAMP_VALID);

            adapter->rx_last_launch_delta_ns = (int)avb_delta_1s_ns(
                    avb_decode_launch_time_ns((u64)adapter->tx_phc_launch),
                    avb_decode_launch_time_ns(launch_time));

            if (rx_pres_valid)
               adapter->rx_last_pres_delta_ns =
                       (int)avb_delta_u32_ns(phc_pres, rx_pres);
         }

         if (capture)
         {
             DECLARE_BITMAP(listeners, MAX_STREAMS);
             int li;

             if (!avb_rx_lookup_streams(adapter, cavpdu, listeners)) {
                 struct avb_stream *owner = NULL;

                 for (int si = 0; si < num_streams; si++) {
                     if (ether_addr_equal(adapter->streams_in[si].dest_mac,
                                          cavpdu->dest)) {
                         owner = &adapter->streams_in[si];
                         break;
                     }
                 }
                 if (owner)
                     owner->rx_drops++;
                 else
                     avb_rx_unmatched++;
                 goto rx_next;
             }

             adapter->rx_packets_total++;
             for_each_set_bit(li, listeners, num_streams) {
               struct avb_stream *stream = &adapter->streams_in[li];
               bool is_ref = li == 0;
               bool ref_anchored = adapter->streams_in[0].rx_pkt_ts_seen;
               int start_frame;

              stream->rx_packets++;
              avb_rx_check_integrity(stream, cavpdu, samples_per_packet,
                                     adapter->rate);
              avb_rx_track_media_time(stream, cavpdu, samples_per_packet,
                                      adapter->rate);

                int align_offset = 0;

                if (num_streams > 1) {
                        int raw;

                        align_offset = stream->align_offset;
                        if (avb_rx_align_frames(adapter, stream, &raw)) {
                            align_offset = raw;
                            if (raw > ALIGN_MAX_FRAMES) {
                                align_offset = ALIGN_MAX_FRAMES;
                                stream->rx_drops++;
                                printk_ratelimited(KERN_WARNING
                                    "avb rx: align clamp sid=%016llx off=%dfr (>max)\n",
                                    (unsigned long long)stream->expected_stream_id,
                                    raw);
                            } else if (raw < -ALIGN_MAX_FRAMES) {
                                align_offset = -ALIGN_MAX_FRAMES;
                                stream->rx_drops++;
                                printk_ratelimited(KERN_WARNING
                                    "avb rx: align clamp sid=%016llx off=%dfr (<-max)\n",
                                    (unsigned long long)stream->expected_stream_id,
                                    raw);
                            }
                        }
                }
                stream->align_offset = align_offset;

                if (num_streams > 1 && !adapter->streams_in[0].rx_pkt_ts_seen)
                        continue;
                if (num_streams > 1 && is_ref && !ref_anchored)
                        continue;
                start_frame = oldcptr / adapter->capture_channels + lead + align_offset;
                if (!is_ref) {
                        start_frame -= samples_per_packet;
                        if (lead >= adapter->rx_lead_frames &&
                            start_frame < oldcptr / adapter->capture_channels)
                                stream->rx_late_writes++;
                }

                if (dma_area)
                        avb_interleave_capture(adapter, dma_area, start_frame,
                                               stream, cavpdu);

                if (is_ref && lead < adapter->rx_lead_frames) {
                        lead += samples_per_packet;
                        adapter->rx_lead_filled = lead;
                } else if (is_ref) {
                        c_cnt += samples_per_packet;
                        adapter->capture_frames_total += samples_per_packet;
                        newcptr = (newcptr + samples_per_packet * adapter->capture_channels) %
                                  (adapter->capture_buffer_size * adapter->capture_channels);
                        oldcptr = newcptr;
                        adapter->hw_capture_pointer = newcptr;
                }
             }
         }

rx_next:
         if (unlikely(avb_rx_bench))
            avb_bench_count(cavpdu->stream_id);

         if (unlikely(!i++))
         {
             avb_debug(adapter, "interrupt handler called (in_interrupt=%ld)\n", in_interrupt());
         }

         rx_desc[tmp].read.pkt_addr = adapter->rx_physaddr + MAXPACKETSIZE * tmp;
         rx_desc[tmp].read.hdr_addr = 0;
         tmp = (tmp + 1) % MAXDESCRIPTORS;
     }

     spin_unlock(&adapter->rx_ident_lock);

     if (processed == budget &&
         (READ_ONCE(rx_desc[tmp].wb.upper.status_error) & 1))
        adapter->rx_budget_hits++;

     if (capture)
     {
        while (c_cnt >= adapter->capture_period_size)
        {
           c_cnt -= adapter->capture_period_size;
           snd_pcm_period_elapsed(csubs);
        }
     }
     adapter->c_cnt = c_cnt;

      if (adapter->rx_ring[1]->next_to_clean != tmp)
      {
         adapter->rx_ring[1]->next_to_clean = tmp;
		wmb();

		/* Tail one short of next_to_clean (as upstream igb) so the NIC cannot lap the ring. */
		writel((tmp + MAXDESCRIPTORS - 1) % MAXDESCRIPTORS,
		       adapter->rx_ring[1]->tail);
      }

}

#include <linux/proc_fs.h>
#include <linux/seq_file.h>

static struct proc_dir_entry *avb_proc_dir;

static bool avb_tx_mirror;
module_param(avb_tx_mirror, bool, 0644);
MODULE_PARM_DESC(avb_tx_mirror,
                 "mirror built TX AVTP frames to avbtap0 for tcpdump (default 0)");

static struct net_device *avb_tap_dev;
static u64 avb_tx_mirror_frames;
static u64 avb_tx_mirror_drops;

static netdev_tx_t avb_tap_xmit(struct sk_buff *skb, struct net_device *dev)
{
    dev_kfree_skb_any(skb);
    return NETDEV_TX_OK;
}

static const struct net_device_ops avb_tap_netdev_ops = {
    .ndo_start_xmit = avb_tap_xmit,
};

static void avb_tap_setup(struct net_device *dev)
{
    ether_setup(dev);
    dev->netdev_ops = &avb_tap_netdev_ops;
    dev->flags |= IFF_NOARP;
}

static void avb_tap_init(void)
{
    struct net_device *dev;
    int err;

    if (avb_tap_dev)
        return;

    dev = alloc_netdev(0, "avbtap%d", NET_NAME_ENUM, avb_tap_setup);
    if (!dev)
        return;

    /* Runs under rtnl (.ndo_open): register_netdev() would deadlock. */
    dev->needs_free_netdev = true;

    err = register_netdevice(dev);
    if (err) {
        free_netdev(dev);
        return;
    }
    avb_tap_dev = dev;
}

static void avb_tap_exit(void)
{
    if (avb_tap_dev) {
        /* Freed via needs_free_netdev at rtnl_unlock: do not free_netdev() here. */
        unregister_netdevice(avb_tap_dev);
        avb_tap_dev = NULL;
    }
}

static void avb_tx_mirror_frame(const void *frame, u32 len)
{
    struct net_device *dev = avb_tap_dev;
    struct sk_buff *skb;

    if (!avb_tx_mirror || !dev || !len)
        return;

    skb = netdev_alloc_skb(dev, len);
    if (!skb) {
        avb_tx_mirror_drops++;
        return;
    }
    skb_put_data(skb, frame, len);
    skb->protocol = eth_type_trans(skb, dev);
    skb->ip_summed = CHECKSUM_UNNECESSARY;
    netif_rx(skb);
    avb_tx_mirror_frames++;
}

static enum hrtimer_restart avb_tx_timer_fn(struct hrtimer *timer)
{
    struct igb_adapter *adapter = container_of(timer, struct igb_adapter, avb_tx_timer);
    ktime_t now = ktime_get();
    struct snd_pcm *pcm = adapter->pcm;
    struct snd_pcm_substream *psubs = NULL;
    bool have_playback;
    struct timespec64 phc_ts;
    u64 phc_now_ns;
    u64 launch_abs_ns;
    u32 pres_time;
    struct igb_avb_tx_desc *tx_desc;
    AVPDU_HEADER *pavpdu;
    u64 launch_time;
    u32 launch_time_l;
    u64 paylen;
    u32 group;
    int j;
    u8 *tx_addr;
    u32 spp;
    int emitted;
    u64 horizon_ns;

    if (pcm)
        psubs = pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
    have_playback = adapter->playback && psubs && psubs->runtime &&
                    psubs->runtime->dma_area;

    igb_ptp_read_i210(adapter, &phc_ts);
    phc_now_ns = (u64)phc_ts.tv_sec * NSEC_PER_SEC + phc_ts.tv_nsec;

    tx_desc = (struct igb_avb_tx_desc *)adapter->tx_ring[0]->desc;
    tx_addr = adapter->tx_addr;
    spp = adapter->samples_per_packet;

    if (spp) {
        horizon_ns = phc_now_ns + AVB_TX_SCHED_LEAD_NS;

        if (adapter->tx_gated) {
            group = adapter->tx_avb_desc_group % adapter->avb_tx_groups;
            pavpdu = (AVPDU_HEADER *)(tx_addr + group * MAXPACKETSIZE);
            memset(pavpdu->audio_data, 0,
                   CHANNELS_PER_STREAM * spp * sizeof(pavpdu->audio_data[0]));
            adapter->tx_next_launch_ns = 0;
            adapter->tx_gated_ticks++;
            goto rearm;
        }

        if (!adapter->tx_next_launch_ns)
            adapter->tx_next_launch_ns =
                avb_align_up_u64(horizon_ns, AVB_TX_INTERVAL_NS);

        if ((s64)(adapter->tx_next_launch_ns - horizon_ns) > AVB_TX_SCHED_LEAD_NS) {
            avb_warn(adapter, "tx schedule re-aligned: PHC stepped back %lldns\n",
                     (long long)(adapter->tx_next_launch_ns - horizon_ns));
            adapter->tx_late_launch++;
            adapter->tx_next_launch_ns =
                avb_align_up_u64(horizon_ns, AVB_TX_INTERVAL_NS);
        }

        if ((s64)(adapter->tx_next_launch_ns - phc_now_ns) < LAUNCH_OFFSET) {
            adapter->tx_late_launch++;
            adapter->tx_next_launch_ns =
                avb_align_up_u64(horizon_ns, AVB_TX_INTERVAL_NS);
        }

        emitted = 0;
        while ((s64)(adapter->tx_next_launch_ns - horizon_ns) <= 0) {
            u32 base_group;
            u64 stream_id_base;
            int start_frame = 0;
            u32 seq, dbc_base, blk_pres;
            int syt_here, s;

            launch_abs_ns = adapter->tx_next_launch_ns;
            base_group = adapter->tx_avb_desc_group % adapter->avb_tx_groups;

            if (avb_tx_desc_groups_used(adapter, base_group) >=
                adapter->avb_tx_groups - num_streams) {
                adapter->tx_xrun++;
                break;
            }

            launch_time_l = (u32)(launch_abs_ns % NSEC_PER_SEC);
            pres_time = (u32)(launch_abs_ns + PRESENTATION_TIME_OFFSET -
                              LAUNCH_OFFSET);
            launch_time = avb_encode_launch_time_abs(launch_time_l);
            paylen = 50 + CHANNELS_PER_STREAM * spp * 4;

            seq = adapter->tx_seq;
            dbc_base = adapter->tx_dbc;
            {
                u32 syt_ival = spp + spp / 3;
                u32 rem      = syt_ival ? dbc_base % syt_ival : 0;
                u32 blk_off  = rem ? syt_ival - rem : 0;

                syt_here = (rem == 0) || (syt_ival - rem < spp);
                blk_pres = pres_time +
                    (u32)((u64)blk_off * NSEC_PER_SEC / adapter->rate);
            }

            if (have_playback)
                start_frame = adapter->hw_playback_pointer /
                              adapter->playback_channels;
            stream_id_base = adapter->streams_out[0].stream_id;

            for (s = 0; s < num_streams; s++) {
                group = (base_group + s) % adapter->avb_tx_groups;
                pavpdu = (AVPDU_HEADER *)(tx_addr + group * MAXPACKETSIZE);

                avb_stamp_talker_dest(adapter, pavpdu, s);
                u64_to_array8(stream_id_base | s, pavpdu->stream_id);

                if (have_playback)
                    avb_deinterleave_playback(adapter,
                                              (u32 *)psubs->runtime->dma_area,
                                              start_frame, s, pavpdu);
                else
                    for (j = 0; j < CHANNELS_PER_STREAM * spp; j++)
                        pavpdu->audio_data[j] = htonl(AVB_AM824_MBLA_24BIT);

                pavpdu->sequence_number = seq;
                pavpdu->data_block_continuity = dbc_base;
                if (syt_here) {
                    pavpdu->timestamp = htonl(blk_pres);
                    pavpdu->flags1 = STREAM_ID_VALID | TIMESTAMP_VALID;
                } else {
                    pavpdu->timestamp = 0;
                    pavpdu->flags1 = STREAM_ID_VALID;
                }

                tx_desc[group].launch_time = launch_time;
                tx_desc[group].cmd1 = E1000_ADVTXD_DCMD_DEXT |
                                      E1000_ADVTXD_DTYP_CTXT;
                tx_desc[group].addr = adapter->tx_physaddr +
                                      MAXPACKETSIZE * group;
                tx_desc[group].cmd2 = (paylen << 46) |
                                      E1000_ADVTXD_DCMD_EOP  |
                                      E1000_ADVTXD_DCMD_RS   |
                                      E1000_ADVTXD_DTYP_DATA |
                                      E1000_ADVTXD_DCMD_IFCS |
                                      E1000_ADVTXD_DCMD_DEXT |
                                      paylen;

                avb_tx_mirror_frame(pavpdu, (u32)paylen);
            }

            if (!have_playback)
                adapter->tx_silence_packets += num_streams;

            adapter->tx_seq++;
            adapter->tx_dbc += spp;

            if (have_playback) {
                adapter->hw_playback_pointer =
                    avb_advance_playback_pointer(adapter,
                                                 adapter->hw_playback_pointer,
                                                 spp);

                adapter->tx_period_acc += spp;
                while (adapter->tx_period_acc >= adapter->playback_period_size) {
                    adapter->tx_period_acc -= adapter->playback_period_size;
                    snd_pcm_period_elapsed(psubs);
                }
            }

            adapter->tx_avb_desc_group = (base_group + num_streams) %
                                         adapter->avb_tx_groups;
            adapter->tx_next_launch_ns = launch_abs_ns + AVB_TX_INTERVAL_NS;

            adapter->tx_phc_launch = launch_time;
            adapter->tx_phc_pres = htonl(pres_time);
            adapter->tx_engine_ticks++;
            emitted++;
        }

        if (emitted) {
            wmb();
            writel(adapter->tx_avb_desc_group * 2,
                   adapter->tx_ring[0]->tail);
        }
    }

rearm:
    hrtimer_forward(timer, now, ns_to_ktime(AVB_TX_INTERVAL_NS));
    return HRTIMER_RESTART;
}

static bool avb_rx_lookup_streams(struct igb_adapter *adapter,
                                  const AVPDU_HEADER_NO_VLAN *cavpdu,
                                  unsigned long *match)
{
    uint64_t stream_id = array8_to_u64(cavpdu->stream_id);
    const struct avb_stream *s;
    bool found = false;

    bitmap_zero(match, MAX_STREAMS);
    for (int i = 0; i < num_streams; i++) {
        s = &adapter->streams_in[i];
        if (s->expected_stream_id && s->expected_stream_id == stream_id) {
            __set_bit(i, match);
            found = true;
        }
    }
    if (found)
        return true;
    for (int i = 0; i < num_streams; i++) {
        s = &adapter->streams_in[i];
        if (!s->expected_stream_id && !is_zero_ether_addr(s->dest_mac) &&
            ether_addr_equal(s->dest_mac, cavpdu->dest)) {
            __set_bit(i, match);
            found = true;
        }
    }
    return found;
}

static int stream_seq_show(struct seq_file *m, void *v)
{
     struct igb_adapter *adapter = m->private;
     int i;

     if (!adapter)
         return -EINVAL;

      for (i = 0; i < num_streams; i++) {
          seq_printf(m, "stream_id=%016llx dest_mac=%pM vlan=%u seq=%u dbc=%u format=%llu channels=%u samplerate=%u\n",
                     adapter->streams_out[i].stream_id,
                     adapter->streams_out[i].dest_mac,
                     adapter->streams_out[i].vlan,
                     adapter->streams_out[i].sequence,
                     adapter->streams_out[i].dbc,
                     adapter->streams_out[i].format,
                     adapter->streams_out[i].channels,
                     adapter->streams_out[i].samplerate);
          seq_printf(m, "  rx: expected_sid=%016llx src_mac=%pM dst_mac=%pM filter=%d slice ch%02d-%02d rx_pkts=%llu rx_drops=%llu align=%d late=%llu\n",
                     adapter->streams_in[i].expected_stream_id,
                     adapter->streams_in[i].src_mac,
                     adapter->streams_in[i].dest_mac,
                     adapter->streams_in[i].rx_filter_id,
                     adapter->streams_in[i].ring_slice_base,
                     adapter->streams_in[i].ring_slice_base + adapter->streams_in[i].ring_slice_chans - 1,
                     adapter->streams_in[i].rx_packets,
                     adapter->streams_in[i].rx_drops,
                     adapter->streams_in[i].align_offset,
                     adapter->streams_in[i].rx_late_writes);
          seq_printf(m, "  rx: seq_errors=%llu last_seq_gap=%d clock_slips=%llu last_slip_ns=%d drift_ppb=%lld\n",
                     adapter->streams_in[i].rx_seq_errors,
                     adapter->streams_in[i].rx_last_seq_gap,
                     adapter->streams_in[i].rx_clock_slips,
                     adapter->streams_in[i].rx_last_slip_ns,
                     (long long)avb_rx_drift_ppb(&adapter->streams_in[i], adapter->rate));
      }
      return 0;
  }

static int stream_seq_open(struct inode *inode, struct file *file)
{
     return single_open(file, stream_seq_show, pde_data(inode));
}

static const struct proc_ops stream_seq_fops = {
     .proc_open = stream_seq_open,
     .proc_read = seq_read,
     .proc_lseek = seq_lseek,
     .proc_release = single_release,
};

static int tx_engine_seq_show(struct seq_file *m, void *v)
{
     struct igb_adapter *adapter = m->private;

     if (!adapter)
         return -EINVAL;

     seq_printf(m, "tx_timer_active=%d\n", adapter->tx_timer_active);
     seq_printf(m, "num_streams=%d\n", num_streams);
     seq_printf(m, "avb_tx_groups=%u\n", adapter->avb_tx_groups);
     seq_printf(m, "tx_engine_ticks=%llu\n", (unsigned long long)adapter->tx_engine_ticks);
     seq_printf(m, "tx_silence_packets=%llu\n", (unsigned long long)adapter->tx_silence_packets);
     seq_printf(m, "tx_phc_pres=%u\n", ntohl(adapter->tx_phc_pres));
     seq_printf(m, "tx_phc_launch=%llu\n", (unsigned long long)adapter->tx_phc_launch);
     seq_printf(m, "tx_next_launch_ns=%llu\n", (unsigned long long)adapter->tx_next_launch_ns);
     seq_printf(m, "tx_period_acc=%u\n", adapter->tx_period_acc);
     seq_printf(m, "tx_late_launch=%llu\n", (unsigned long long)adapter->tx_late_launch);
     seq_printf(m, "tx_xrun=%llu\n", (unsigned long long)adapter->tx_xrun);
     seq_printf(m, "tx_gated=%d\n", adapter->tx_gated);
     seq_printf(m, "tx_gated_ticks=%llu\n", (unsigned long long)adapter->tx_gated_ticks);
     seq_printf(m, "tx_gptp_offset_ns=%lld\n", (long long)adapter->tx_gptp_offset_ns);
     seq_printf(m, "tx_mirror=%d\n", avb_tx_mirror);
     seq_printf(m, "tx_mirror_dev=%s\n", avb_tap_dev ? avb_tap_dev->name : "(none)");
     seq_printf(m, "tx_mirror_frames=%llu\n", (unsigned long long)avb_tx_mirror_frames);
     seq_printf(m, "tx_mirror_drops=%llu\n", (unsigned long long)avb_tx_mirror_drops);

     seq_printf(m, "aggregate_idleslope_bps=%u\n", adapter->aggregate_idleslope_bps);
     seq_printf(m, "reserved_bw_permille=%u\n", adapter->reserved_bw_permille);

     return 0;
}

static int tx_engine_seq_open(struct inode *inode, struct file *file)
{
     return single_open(file, tx_engine_seq_show, pde_data(inode));
}

static ssize_t tx_engine_seq_write(struct file *file, const char __user *buf,
                                   size_t count, loff_t *pos)
{
     struct seq_file *seq = file->private_data;
     struct igb_adapter *adapter = seq ? seq->private : NULL;
     char kbuf[64];
     int synced;
     long long offset;
     int gated;

     if (!adapter)
         return -EINVAL;
     if (count >= sizeof(kbuf))
         return -EINVAL;
     if (copy_from_user(kbuf, buf, count))
         return -EFAULT;
     kbuf[count] = '\0';
     if (sscanf(kbuf, "sync_state=%d offset_ns=%lld", &synced, &offset) != 2 &&
         sscanf(kbuf, "%d %lld", &synced, &offset) != 2)
         return -EINVAL;

     gated = !synced;
     adapter->tx_gptp_offset_ns = offset;
     if (adapter->tx_gated != gated) {
         adapter->tx_gated = gated;
         if (gated)
             avb_warn(adapter, "tx gated: PHC offset %lldns > threshold\n", offset);
         else
             avb_info(adapter, "tx armed\n");
     }
     return count;
}

static const struct proc_ops tx_engine_seq_fops = {
     .proc_open = tx_engine_seq_open,
     .proc_read = seq_read,
     .proc_write = tx_engine_seq_write,
     .proc_lseek = seq_lseek,
     .proc_release = single_release,
};

static int rx_engine_seq_show(struct seq_file *m, void *v)
{
     struct igb_adapter *adapter = m->private;
     u64 seq_errors = 0, clock_slips = 0;
     s64 worst_ppb = 0;
     int i;

     if (!adapter)
         return -EINVAL;

     for (i = 0; i < num_streams; i++) {
         s64 ppb = avb_rx_drift_ppb(&adapter->streams_in[i], adapter->rate);

         seq_errors  += adapter->streams_in[i].rx_seq_errors;
         clock_slips += adapter->streams_in[i].rx_clock_slips;
         if (abs(ppb) > abs(worst_ppb))
                 worst_ppb = ppb;
     }

     seq_printf(m, "rx_ring_desc=%d\n", MAXDESCRIPTORS);
     seq_printf(m, "rx_desc_budget=%d\n", avb_rx_desc_budget());
     seq_printf(m, "rx_packets_total=%llu\n", (unsigned long long)adapter->rx_packets_total);
     seq_printf(m, "capture_frames_total=%llu\n", (unsigned long long)adapter->capture_frames_total);
     seq_printf(m, "rx_desc_backlog=%u\n", adapter->rx_desc_backlog);
     seq_printf(m, "rx_desc_backlog_max=%u\n", adapter->rx_desc_backlog_max);
     seq_printf(m, "rx_ring_near_lap=%llu\n", (unsigned long long)adapter->rx_ring_near_lap);
     seq_printf(m, "rx_budget_hits=%llu\n", (unsigned long long)adapter->rx_budget_hits);
     seq_printf(m, "rx_seq_errors=%llu\n", (unsigned long long)seq_errors);
     seq_printf(m, "rx_clock_slips=%llu\n", (unsigned long long)clock_slips);
     seq_printf(m, "rx_unmatched=%llu\n", (unsigned long long)avb_rx_unmatched);
      seq_printf(m, "rx_media_drift_ppb=%lld\n", (long long)worst_ppb);
      seq_printf(m, "rx_ts_span_ns=%lld\n", (long long)adapter->streams_in[0].rx_ts_span_ns);
      seq_printf(m, "rx_ts_frames=%llu\n", (unsigned long long)adapter->streams_in[0].rx_ts_frames);

      seq_printf(m, "rx_ts_span_snap[0]=%llu\n", (unsigned long long)adapter->streams_in[0].rx_ts_span_snap[0]);
      seq_printf(m, "rx_ts_span_snap[1]=%llu\n", (unsigned long long)adapter->streams_in[0].rx_ts_span_snap[1]);
      seq_printf(m, "rx_ts_frames_snap[0]=%llu\n", (unsigned long long)adapter->streams_in[0].rx_ts_frames_snap[0]);
      seq_printf(m, "rx_ts_frames_snap[1]=%llu\n", (unsigned long long)adapter->streams_in[0].rx_ts_frames_snap[1]);
      seq_printf(m, "rx_snap_idx=%d\n", adapter->streams_in[0].rx_snap_idx);
      seq_printf(m, "rx_pkts_at_snap[0]=%llu\n", (unsigned long long)adapter->streams_in[0].rx_pkts_at_snap[0]);
      seq_printf(m, "rx_pkts_at_snap[1]=%llu\n", (unsigned long long)adapter->streams_in[0].rx_pkts_at_snap[1]);
      seq_printf(m, "rx_window_ready=%d\n", adapter->streams_in[0].rx_window_ready);

      seq_printf(m, "rx_slip_threshold_ns=%d\n", avb_rx_slip_ns);
      seq_printf(m, "rx_drift_warn_ppb=%d\n", avb_rx_drift_warn_ppb);
      seq_printf(m, "rx_last_pres_delta_ns=%d\n", adapter->rx_last_pres_delta_ns);
      seq_printf(m, "rx_last_launch_delta_ns=%d\n", adapter->rx_last_launch_delta_ns);

      for (i = 0; i < num_streams; i++) {
          const struct avb_stream *s = &adapter->streams_in[i];
          int settling;
          s64 sliding = avb_rx_drift_ppb_sliding(s, adapter->rate, &settling);

          seq_printf(m, "stream[%d]: rx_pkts=%llu drift_ppb=%lld sliding_ppb=%lld window_ready=%d clock_slips=%llu seq_errors=%llu\n",
                     i, (unsigned long long)s->rx_packets,
                     (long long)avb_rx_drift_ppb(s, adapter->rate),
                     (long long)sliding, s->rx_window_ready,
                     (unsigned long long)s->rx_clock_slips,
                     (unsigned long long)s->rx_seq_errors);
      }
      return 0;
}

static int rx_engine_seq_open(struct inode *inode, struct file *file)
{
     return single_open(file, rx_engine_seq_show, pde_data(inode));
}

static const struct proc_ops rx_engine_seq_fops = {
     .proc_open = rx_engine_seq_open,
     .proc_read = seq_read,
     .proc_lseek = seq_lseek,
     .proc_release = single_release,
};

static void avb_procfs_init(struct igb_adapter *adapter)
{
     avb_proc_dir = proc_mkdir("snd_igb_avb", NULL);
     if (!avb_proc_dir)
         return;

     proc_create_data("streams", 0444, avb_proc_dir, &stream_seq_fops, adapter);
     proc_create_data("tx_engine", 0644, avb_proc_dir, &tx_engine_seq_fops, adapter);
     proc_create_data("rx_engine", 0444, avb_proc_dir, &rx_engine_seq_fops, adapter);

     avb_tap_init();
}

static void avb_procfs_exit(void)
{
     avb_tap_exit();

     if (avb_proc_dir) {
         proc_remove(avb_proc_dir);
         avb_proc_dir = NULL;
     }
}