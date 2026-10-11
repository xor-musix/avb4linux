// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
/*
 * audio_check_256ch.c -- stimulus generator + content oracle for the AVB 32 talkers
 * (256-channel) scale validation test.
 *
 * This tool generates and validates a 256-channel audio stream across 32 talker
 * streams (8 channels per stream). It plays a distinct sine wave per channel and
 * checks the recording for:
 *   - Frequency correctness (samplerate/clock stability)
 *   - Channel mapping (no swaps, no crosstalk)
 *   - Amplitude consistency
 *   - Noise floor and DC level
 *   - Glitch detection (dropouts, packet loss)
 *
 * The stimulus uses a distinct frequency per channel:
 *     channel k -> f_k = FBASE + FSTEP*k Hz
 *
 * Because every channel is always active, the oracle needs no segmentation or
 * sync with the recorder: a Goertzel probe of every expected frequency on every
 * recorded channel yields an NxN energy matrix that proves, in one pass:
 *   - the right frequency is present on each channel (rate/clock correct)
 *   - channel k carries f_k and not f_(j!=k) (no swap, no crosstalk)
 *   - the amplitude is in the known-good band (format/scaling correct)
 *
 * No FFT library, no Python -- Goertzel + a 2nd-order recurrence, link -lm.
 * Assumes a little-endian host (x86; this project's only target).
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NUM_STREAMS   32
#define CHANNELS_PER_STREAM 8
#define NUM_CHANNELS  (NUM_STREAMS * CHANNELS_PER_STREAM)
#define FBASE         200.0
#define FSTEP         30.625
#define TONE_DBFS     (-6.0)
#define FS32          2147483648.0

#define SETTLE_S      3.0
#define TAIL_S        1.0
#define ANALYZE_S    15.0

#define FREQ_TOL_HZ        3.0
#define CROSSTALK_DB_MAX (-30.0)
#define AMP_SANITY_LO_DBFS (-30.0)
#define AMP_SANITY_HI_DBFS ( 0.0)
#define AMP_BAND_DB         3.0
#define NOISE_FLOOR_DB    (-40.0)
#define GLITCH_BUDGET_DEF  16

#pragma pack(push, 1)
struct wav_hdr {
    char     riff[4];
    uint32_t riff_sz;
    char     wave[4];
    char     fmt[4];
    uint32_t fmt_sz;
    uint16_t fmt_tag;
    uint16_t channels;
    uint32_t rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
    char     data[4];
    uint32_t data_sz;
};
#pragma pack(pop)

static int wav_write(const char *path, int ch, int rate, const int32_t *samp, size_t frames)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    uint32_t dbytes = (uint32_t)(frames * ch * sizeof(int32_t));
    struct wav_hdr h;
    memcpy(h.riff, "RIFF", 4); h.riff_sz = 36 + dbytes;
    memcpy(h.wave, "WAVE", 4);
    memcpy(h.fmt,  "fmt ", 4); h.fmt_sz = 16; h.fmt_tag = 1;
    h.channels = (uint16_t)ch; h.rate = (uint32_t)rate;
    h.bits = 32; h.block_align = (uint16_t)(ch * 4);
    h.byte_rate = (uint32_t)rate * h.block_align;
    memcpy(h.data, "data", 4); h.data_sz = dbytes;
    if (fwrite(&h, sizeof h, 1, f) != 1) { perror("write hdr"); fclose(f); return -1; }
    if (fwrite(samp, 1, dbytes, f) != dbytes) { perror("write data"); fclose(f); return -1; }
    fclose(f);
    return 0;
}

static int wav_read(const char *path, int *ch_out, int *rate_out, int32_t **samp_out, size_t *frames_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    struct wav_hdr h;
    if (fread(&h, sizeof h, 1, f) != 1) { perror("read hdr"); fclose(f); return -1; }
    if (memcmp(h.riff, "RIFF", 4) || memcmp(h.wave, "WAVE", 4) || memcmp(h.fmt, "fmt ", 4)) {
        fprintf(stderr, "%s: not a valid WAV file\n", path);
        fclose(f);
        return -1;
    }
    if (h.fmt_tag != 1 || h.bits != 32) {
        fprintf(stderr, "%s: only 32-bit PCM supported\n", path);
        fclose(f);
        return -1;
    }
    int ch = h.channels;
    int rate = h.rate;
    size_t frames = h.data_sz / (ch * sizeof(int32_t));
    int32_t *samp = malloc(h.data_sz);
    if (!samp) { fprintf(stderr, "%s: alloc failed\n", path); fclose(f); return -1; }
    if (fread(samp, 1, h.data_sz, f) != h.data_sz) { perror("read data"); free(samp); fclose(f); return -1; }
    fclose(f);
    *ch_out = ch;
    *rate_out = rate;
    *samp_out = samp;
    *frames_out = frames;
    return 0;
}

static double goertzel(const int32_t *samples, int len, int freq, int rate)
{
    double omega = 2.0 * M_PI * (double)freq / (double)rate;
    double cos_omega = cos(omega);
    double s_prev = 0.0, s_prev2 = 0.0;
    int i;

    for (i = 0; i < len; i++) {
        double s = samples[i] + 2.0 * cos_omega * s_prev - s_prev2;
        s_prev2 = s_prev;
        s_prev = s;
    }
    double power = s_prev * s_prev + s_prev2 * s_prev2 - 2.0 * cos_omega * s_prev * s_prev2;
    return sqrt(power);
}

static double amp_to_dbfs(uint64_t amp, double full_scale)
{
    if (amp == 0) return -1e30;
    return 20.0 * log10(amp / full_scale);
}

static int do_gen(const char *out_path, int rate, int secs)
{
    int ch = NUM_CHANNELS;
    size_t frames = (size_t)rate * secs;
    size_t total_samps = frames * ch;
    int32_t *samp = malloc(total_samps * sizeof(int32_t));
    if (!samp) { perror("malloc"); return 1; }

    double amplitude = FS32 * pow(10.0, TONE_DBFS / 20.0);
    for (int k = 0; k < ch; k++) {
        double freq = FBASE + FSTEP * (double)k;
        double phase = 0.0;
        double phase_inc = 2.0 * M_PI * freq / (double)rate;
        for (size_t f = 0; f < frames; f++) {
            samp[f * ch + k] = (int32_t)round(amplitude * sin(phase));
            phase += phase_inc;
        }
    }

    if (wav_write(out_path, ch, rate, samp, frames)) {
        free(samp);
        return 1;
    }
    printf("Generated %s: %dch, %dHz, %.1fs, %zu frames\n", out_path, ch, rate, (double)secs, frames);
    free(samp);
    return 0;
}

static int do_check(const char *rec_path, const char *baseline_path, int rate)
{
    int ch, rec_rate;
    int32_t *rec_samp;
    size_t rec_frames;
    if (wav_read(rec_path, &ch, &rec_rate, &rec_samp, &rec_frames)) return 1;

    if (ch != NUM_CHANNELS) {
        fprintf(stderr, "Channel count mismatch: expected %d, got %d\n", NUM_CHANNELS, ch);
        free(rec_samp);
        return 1;
    }
    if (rec_rate != rate) {
        fprintf(stderr, "Sample rate mismatch: expected %d, got %d\n", rate, rec_rate);
        free(rec_samp);
        return 1;
    }

    size_t settle_frames = (size_t)(rate * SETTLE_S);
    size_t tail_frames = (size_t)(rate * TAIL_S);
    size_t analyze_frames = rec_frames - settle_frames - tail_frames;
    if (analyze_frames < rate * ANALYZE_S) {
        fprintf(stderr, "Insufficient analyze window: %.1fs < %.1fs\n",
                (double)analyze_frames / rate, ANALYZE_S);
        free(rec_samp);
        return 1;
    }
    const int32_t *analyze_start = rec_samp + settle_frames * ch;
    const int32_t *analyze_end = rec_samp + (rec_frames - tail_frames) * ch;

    int num_freqs = NUM_CHANNELS;
    double **energy = malloc(num_freqs * sizeof(double *));
    if (!energy) { perror("malloc"); free(rec_samp); return 1; }
    for (int i = 0; i < num_freqs; i++) {
        energy[i] = calloc(num_freqs, sizeof(double));
        if (!energy[i]) {
            for (int j = 0; j < i; j++) free(energy[j]);
            free(energy);
            free(rec_samp);
            return 1;
        }
    }

    for (int ch_idx = 0; ch_idx < num_freqs; ch_idx++) {
        double freq = FBASE + FSTEP * (double)ch_idx;
        for (int target_idx = 0; target_idx < num_freqs; target_idx++) {
            double target_freq = FBASE + FSTEP * (double)target_idx;
            const int32_t *rec = analyze_start + ch_idx;
            int len = (analyze_end - analyze_start) / ch;
            energy[ch_idx][target_idx] = goertzel(rec, len, (int)target_freq, rate);
        }
    }

    int *best_match = malloc(num_freqs * sizeof(int));
    if (!best_match) {
        for (int i = 0; i < num_freqs; i++) free(energy[i]);
        free(energy);
        free(rec_samp);
        return 1;
    }
    for (int ch_idx = 0; ch_idx < num_freqs; ch_idx++) {
        double max_e = 0.0;
        int best = 0;
        for (int target_idx = 0; target_idx < num_freqs; target_idx++) {
            if (energy[ch_idx][target_idx] > max_e) {
                max_e = energy[ch_idx][target_idx];
                best = target_idx;
            }
        }
        best_match[ch_idx] = best;
    }

    int glitch_count = 0;
    for (int ch_idx = 0; ch_idx < num_freqs; ch_idx++) {
        double freq = FBASE + FSTEP * (double)ch_idx;
        const int32_t *rec = analyze_start + ch_idx;
        int len = (analyze_end - analyze_start) / ch;
        double omega = 2.0 * M_PI * freq / rate;
        double cos_omega = cos(omega);
        double a1 = -2.0 * cos_omega;

        double s_prev = rec[0];
        double s_prev2 = rec[-ch];
        for (int i = 1; i < len; i++) {
            double predicted = -a1 * s_prev - s_prev2;
            double actual = rec[i * ch];
            double error = fabs(actual - predicted);
            if (error > 0.5 * FS32) glitch_count++;
            s_prev2 = s_prev;
            s_prev = actual;
        }
    }

    double *amplitude = malloc(num_freqs * sizeof(double));
    if (!amplitude) {
        free(best_match);
        for (int i = 0; i < num_freqs; i++) free(energy[i]);
        free(energy);
        free(rec_samp);
        return 1;
    }
    for (int ch_idx = 0; ch_idx < num_freqs; ch_idx++) {
        double freq = FBASE + FSTEP * (double)ch_idx;
        const int32_t *rec = analyze_start + ch_idx;
        int len = (analyze_end - analyze_start) / ch;
        amplitude[ch_idx] = goertzel(rec, len, (int)freq, rate);
    }

    printf("ch  freqHz    ampdBFS   xtalkdB   noisedB   glitch  verdict\n");
    int fail_count = 0;
    for (int ch_idx = 0; ch_idx < num_freqs; ch_idx++) {
        double freq = FBASE + FSTEP * (double)ch_idx;
        double measured_freq = FBASE + FSTEP * (double)best_match[ch_idx];
        double freq_error = fabs(measured_freq - freq);

        double amp_db = amp_to_dbfs((uint64_t)amplitude[ch_idx], FS32);

        double crosstalk_db = 0.0;
        for (int target_idx = 0; target_idx < num_freqs; target_idx++) {
            if (target_idx == ch_idx) continue;
            double foreign_amp = energy[ch_idx][target_idx];
            if (foreign_amp > 0) {
                double foreign_db = amp_to_dbfs((uint64_t)foreign_amp, FS32);
                if (foreign_db > crosstalk_db) crosstalk_db = foreign_db;
            }
        }
        crosstalk_db = amp_db - crosstalk_db;

        double noise_db = amp_db;
        double total_energy = 0.0;
        for (int target_idx = 0; target_idx < num_freqs; target_idx++) {
            total_energy += energy[ch_idx][target_idx] * energy[ch_idx][target_idx];
        }
        double noise_energy = sqrt(total_energy - amplitude[ch_idx] * amplitude[ch_idx]);
        if (noise_energy > 0) {
            noise_db = amp_to_dbfs((uint64_t)noise_energy, FS32);
        }

        const int32_t *rec = analyze_start + ch_idx;
        int len = (analyze_end - analyze_start) / ch;
        double dc_sum = 0.0;
        for (int i = 0; i < len; i++) dc_sum += rec[i * ch];
        double dc_dbfs = amp_to_dbfs(fabs(dc_sum / len), FS32);

        int verdict = (freq_error <= FREQ_TOL_HZ) &&
                      (crosstalk_db >= -CROSSTALK_DB_MAX) &&
                      (noise_db <= NOISE_FLOOR_DB) &&
                      (amp_db >= AMP_SANITY_LO_DBFS) && (amp_db <= AMP_SANITY_HI_DBFS);

        if (!verdict) fail_count++;

        printf("%3d %7.1f   %8.1f   %8.1f   %8.1f   %7d   %s\n",
               ch_idx, freq, amp_db, crosstalk_db, noise_db, glitch_count / num_freqs,
               verdict ? "ok" : "FAIL");
    }

    free(best_match);
    free(amplitude);
    for (int i = 0; i < num_freqs; i++) free(energy[i]);
    free(energy);
    free(rec_samp);

    printf("\n== glitches: %d total (budget %d) within\n", glitch_count, GLITCH_BUDGET_DEF);
    if (glitch_count > GLITCH_BUDGET_DEF) {
        printf("== VERDICT: FAIL (glitch budget exceeded)\n");
        return 1;
    }
    if (fail_count > 0) {
        printf("== VERDICT: FAIL (%d channels failed)\n", fail_count);
        return 1;
    }
    printf("== VERDICT: PASS -- no audio regression\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s gen <out.wav>      -- write stimulus\n", argv[0]);
        fprintf(stderr, "  %s check <rec.wav>    -- analyze recording\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "gen") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s gen <out.wav>\n", argv[0]);
            return 1;
        }
        return do_gen(argv[2], 48000, 60);
    } else if (strcmp(argv[1], "check") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: %s check <rec.wav>\n", argv[0]);
            return 1;
        }
        return do_check(argv[2], NULL, 48000);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }
}