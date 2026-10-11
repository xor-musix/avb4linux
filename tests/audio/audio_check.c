// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
/*
 * audio_check.c -- stimulus generator + content oracle for the AVB audio
 * loopback regression test (tests/audio/test_loopback.sh).
 *
 * The audio data plane on the i210 is DMA-ring-injected and BYPASSES the netdev,
 * so it cannot be observed with tcpdump -- the only way to validate it is to play
 * a known signal out the talker, record it back through the listener (a MOTU
 * AVB-in->AVB-out loopback), and check the captured WAV. This tool is both halves:
 *
 *   audio_check gen   <out.wav>   -- write the known multitone stimulus
 *   audio_check gen   - --raw     -- stream it as raw S32_LE to stdout, any length
 *   audio_check check <rec.wav>.. -- analyze a recording, PASS/FAIL + metrics
 *
 * Size limits: a WAV's data size is a 32-bit field, so one file holds < 4 GiB.
 * `gen --raw` has no limit, and `check` reads a raw S32_LE recording (*.raw,
 * needs --ch and --rate), which test_loopback.sh uses past 2 GiB.  Do NOT feed
 * it arecord's own WAV splits (rec-01.wav, ...): arecord drops the rest of the
 * read chunk at each split, so the parts are not contiguous.  `check` does take
 * several files and joins them, for parts that really are contiguous.
 *
 * The stimulus puts ONE distinct continuous sine on each channel:
 *     channel k  ->  f_k = FBASE + FSTEP*k Hz   (denser grids above 128 ch)
 * Because every channel is always active, the oracle needs no segmentation and no
 * sync with the recorder: a Goertzel probe of every expected frequency on every
 * recorded channel yields an NxN energy matrix that proves, in one pass:
 *   - the right frequency is present on each channel   (rate/clock correct)
 *   - channel k carries f_k and not f_(j!=k)           (no swap, no crosstalk)
 *   - the amplitude is in the known-good band          (format/scaling correct)
 * plus a time-domain sine-predictor residual that counts glitches (dropouts /
 * packet loss / clock slips).
 *
 * SIGNAL QUALITY AND TIMELINE INTEGRITY ARE MEASURED SEPARATELY.  They used to
 * be conflated, and it cost a full diagnostic round on 2026-08-02: a recording
 * that was bit-exact apart from ONE 4870-frame splice at t=75.56s came back as
 * eight channels of "FAIL NOISE" with amplitudes scattered from -6 to -17 dBFS.
 * The cause was that amplitude was a Goertzel coherent over the whole 120 s
 * window, so a single phase discontinuity split the integration into two
 * partially-cancelling halves; the energy that left the tone bin was then
 * reported as broadband noise.  The per-channel severities encoded nothing but
 * f_c and the splice size -- |x + (1-x)*exp(j*2*pi*frac(f_c*D/rate))| predicted
 * all eight observed amplitudes to within 0.05 dB.
 *
 * So: amplitude, crosstalk and noise are per-block MEDIANS (one block = one
 * stimulus period), which one bad block cannot move, and a timeline break is
 * detected and reported explicitly -- as a count, a size in frames, and a
 * timestamp -- by `check`'s splice detector.  A splice fails the run on its own
 * budget (default 0); it no longer masquerades as a noise problem.
 *
 * LIVE CHANNELS.  `check --live-ch N` judges only channels < N as above; every
 * channel >= N must be SILENT.  That is the multi-stream RX case where only some
 * listener streams can arrive: the stimulus still drives every channel, so audio
 * that leaks into a channel that must be silent carries a FOREIGN tone, and the
 * dominant tone names the channel it came from (`FAIL LEAK ... (ch3 tone)`).
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
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static double get_time_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

#define PROGRESS_AFTER_S 2.0

static void fmt_dur(double s, char *buf, size_t n)
{
    long t = (long)(s + 0.5);
    if (t >= 60) snprintf(buf, n, "%ldm%02lds", t / 60, t % 60);
    else         snprintf(buf, n, "%lds", t);
}

struct progress { double t0; int shown, last_decile, tty; };

static void progress_init(struct progress *p)
{
    p->t0 = get_time_s(); p->shown = 0; p->last_decile = -1;
    p->tty = isatty(STDERR_FILENO);
}

static void progress_step(struct progress *p, int done, int total)
{
    double el = get_time_s() - p->t0;
    int decile = total ? done * 10 / total : 10;
    char e[32], r[32];

    if (el < PROGRESS_AFTER_S || done <= 0)
        return;
    if (!p->tty && decile == p->last_decile && done < total)
        return;
    p->last_decile = decile;
    fmt_dur(el, e, sizeof e);
    fmt_dur(el / done * (total - done), r, sizeof r);
    fprintf(stderr, "%s   analysing: channel %d/%d (%d%%)  %s elapsed, ~%s left%s",
            p->tty ? "\r\033[K" : "", done, total, done * 100 / total, e, r,
            p->tty ? "" : "\n");
    p->shown = 1;
}

static void progress_done(struct progress *p, const char *what)
{
    char e[32];

    if (!p->shown)
        return;
    fmt_dur(get_time_s() - p->t0, e, sizeof e);
    fprintf(stderr, "%s   %s in %s\n", p->tty ? "\r\033[K" : "", what, e);
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Tone grids need a composite period >= 1 s and no harmonic pairs; never change one once used. */
#define FBASE   661.0
#define FSTEP   181.0
#define TONE_DBFS (-6.0)
#define FS32    2147483648.0

#define SETTLE_S    0.0
#define TAIL_S      0.0

#define FREQ_TOL_HZ        3.0
#define CROSSTALK_DB_MAX (-30.0)
#define AMP_SANITY_LO_DBFS (-30.0)
#define AMP_SANITY_HI_DBFS ( 0.0)
#define AMP_BAND_DB         3.0
#define NOISE_FLOOR_DB    (-40.0)
#define SILENT_MAX_DBFS   (TONE_DBFS + NOISE_FLOOR_DB)
#define GLITCH_BUDGET_DEF   16
#define SPLICE_BUDGET_DEF    0
#define SPLICE_FIT_MAX     0.10

#define MAXCH 256
#define TWO_PI (2.0 * M_PI)

static const struct { int maxch; double fbase, fstep; } tone_grids[] = {
    { 128, FBASE, FSTEP },
    { 192, 667.0, 120.0 },
    { 256, 667.0,  90.0 },
};

static double g_fbase = FBASE, g_fstep = FSTEP;

static double tone_freq(int ch) { return g_fbase + g_fstep * (double)ch; }

static int select_grid(int ch, int rate)
{
    size_t i = 0;

    while (i + 1 < sizeof tone_grids / sizeof tone_grids[0] && ch > tone_grids[i].maxch)
        i++;
    g_fbase = tone_grids[i].fbase;
    g_fstep = tone_grids[i].fstep;
    if (tone_freq(ch - 1) >= rate / 2.0) {
        fprintf(stderr, "%dch @ %dHz: top tone %.0f Hz is not below Nyquist (%.0f Hz)\n",
                ch, rate, tone_freq(ch - 1), rate / 2.0);
        return -1;
    }
    return 0;
}

static long gcd_l(long a, long b) { while (b) { long t = a % b; a = b; b = t; } return a < 0 ? -a : a; }

static long stimulus_period_frames(int ch, int rate)
{
    long period = 1, cap = 100L * rate;

    for (int c = 0; c < ch; c++) {
        long f = lround(tone_freq(c));
        long p = f ? rate / gcd_l(f, rate) : 1;

        period = period / gcd_l(period, p) * p;
        if (period >= cap) return cap;
    }
    return period;
}

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
    uint64_t bytes = (uint64_t)frames * ch * sizeof(int32_t);
    if (bytes > UINT32_MAX - 36) {
        fprintf(stderr, "%s: %.2f GB is too large for a WAV (32-bit sizes) -- use gen --raw\n",
                path, bytes / 1e9);
        return -1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    uint32_t dbytes = (uint32_t)bytes;
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

static int wav_read(const char *path, int *ch, int *rate, int32_t **out, size_t *frames)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    char id[4]; uint32_t sz;
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4)) { fprintf(stderr, "not RIFF\n"); fclose(f); return -1; }
    fseek(f, 4, SEEK_CUR);
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4)) { fprintf(stderr, "not WAVE\n"); fclose(f); return -1; }

    int got_fmt = 0; uint16_t tag = 0, channels = 0, bits = 0; uint32_t srate = 0;
    long data_off = -1; uint32_t data_sz = 0;

    while (fread(id, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1) {
        if (!memcmp(id, "fmt ", 4)) {
            uint8_t buf[40] = {0};
            uint32_t n = sz > sizeof buf ? sizeof buf : sz;
            if (fread(buf, 1, n, f) != n) break;
            tag      = (uint16_t)(buf[0] | buf[1] << 8);
            channels = (uint16_t)(buf[2] | buf[3] << 8);
            srate    = (uint32_t)(buf[4] | buf[5] << 8 | buf[6] << 16 | (uint32_t)buf[7] << 24);
            bits     = (uint16_t)(buf[14] | buf[15] << 8);
            if (tag == 0xFFFE && sz >= 40)          /* EXTENSIBLE: real format in subformat GUID */
                tag = (uint16_t)(buf[24] | buf[25] << 8);
            got_fmt = 1;
            if (sz > n) fseek(f, sz - n, SEEK_CUR);
        } else if (!memcmp(id, "data", 4)) {
            data_off = ftell(f);
            data_sz  = sz;
            fseek(f, sz, SEEK_CUR);
        } else {
            fseek(f, sz, SEEK_CUR);
        }
        if (sz & 1) fseek(f, 1, SEEK_CUR);          /* chunks are word-aligned */
    }
    if (!got_fmt || data_off < 0) { fprintf(stderr, "missing fmt/data chunk\n"); fclose(f); return -1; }
    if (tag != 1)   { fprintf(stderr, "format tag %u is not PCM\n", tag); fclose(f); return -1; }
    if (bits != 32) { fprintf(stderr, "bits=%u, expected 32 (S32_LE)\n", bits); fclose(f); return -1; }
    if (channels == 0) { fprintf(stderr, "0 channels\n"); fclose(f); return -1; }

    size_t fr = data_sz / (channels * 4u);
    int32_t *buf = malloc(fr * channels * sizeof(int32_t));
    if (!buf) { fprintf(stderr, "oom\n"); fclose(f); return -1; }
    fseek(f, data_off, SEEK_SET);
    if (fread(buf, channels * 4u, fr, f) != fr) { fprintf(stderr, "short read\n"); free(buf); fclose(f); return -1; }
    fclose(f);
    *ch = channels; *rate = (int)srate; *out = buf; *frames = fr;
    return 0;
}

static int raw_read(const char *path, int ch, int rate, int32_t **out, size_t *frames)
{
    if (ch <= 0 || rate <= 0) { fprintf(stderr, "%s: a raw recording needs --ch and --rate\n", path); return -1; }
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    struct stat st;
    if (fstat(fileno(f), &st) != 0) { perror(path); fclose(f); return -1; }
    size_t fr = (size_t)st.st_size / (ch * 4u);
    int32_t *buf = malloc(fr * ch * sizeof(int32_t));
    if (!buf) { fprintf(stderr, "oom\n"); fclose(f); return -1; }
    if (fread(buf, ch * 4u, fr, f) != fr) { fprintf(stderr, "short read\n"); free(buf); fclose(f); return -1; }
    fclose(f);
    *out = buf; *frames = fr;
    return 0;
}

static int rec_read(const char *path, int want_ch, int want_rate,
                    int *ch, int *rate, int32_t **out, size_t *frames)
{
    size_t n = strlen(path);

    if (n > 4 && !strcmp(path + n - 4, ".raw")) {
        *ch = want_ch; *rate = want_rate;
        return raw_read(path, want_ch, want_rate, out, frames);
    }
    return wav_read(path, ch, rate, out, frames);
}

static void gen_fill(int32_t *samp, size_t n0, size_t nfr, int ch, int rate)
{
    double amp = pow(10.0, TONE_DBFS / 20.0) * FS32;
    for (int c = 0; c < ch; c++) {
        double w = 2.0 * M_PI * tone_freq(c) / rate;
        for (size_t n = 0; n < nfr; n++) {
            double v = amp * sin(w * (double)(n0 + n));
            long s = lround(v);
            if (s >  2147483647L) s =  2147483647L;
            if (s < -2147483648L) s = -2147483648L;
            samp[n * ch + c] = (int32_t)s;
        }
    }
}

static int gen_raw(int ch, int rate, size_t frames)
{
    long per = stimulus_period_frames(ch, rate);
    size_t blk = (per > 0 && per <= 10L * rate) ? (size_t)per : (size_t)rate;
    int periodic = blk == (size_t)per;
    int32_t *samp = malloc(blk * ch * sizeof(int32_t));
    if (!samp) { fprintf(stderr, "oom\n"); return -1; }

    if (periodic)
        gen_fill(samp, 0, blk, ch, rate);
    for (size_t done = 0; done < frames; ) {
        size_t n = frames - done < blk ? frames - done : blk;

        if (!periodic)
            gen_fill(samp, done, n, ch, rate);
        if (fwrite(samp, ch * sizeof(int32_t), n, stdout) != n) {
            free(samp);
            return -1;
        }
        done += n;
    }
    free(samp);
    return fflush(stdout) ? -1 : 0;
}

static void gen_banner(const char *what, int ch, int rate, double secs, long per)
{
    fprintf(stderr, "gen: %s  %dch @ %dHz  %.0fs  (ch k -> %.0f+%.0f*k Hz, %.0f dBFS)\n",
            what, ch, rate, secs, g_fbase, g_fstep, TONE_DBFS);
    fprintf(stderr, "     composite period %ld frames (%.3fs) -- timing offsets are only\n"
                    "     resolvable modulo this, so keep it >= 1s (see FBASE comment)\n",
            per, (double)per / rate);
}

static int cmd_gen(int argc, char **argv)
{
    const char *out = NULL;
    int ch = 8, rate = 48000, raw = 0; double secs = 30.0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--ch")   && i+1 < argc) ch   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i+1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--secs") && i+1 < argc) secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--raw")) raw = 1;
        else if (argv[i][0] != '-' || !strcmp(argv[i], "-")) out = argv[i];
    }
    if (!out) { fprintf(stderr, "usage: audio_check gen <out.wav> | - --raw [--ch N --rate R --secs S]\n"); return 2; }
    if (ch < 1 || ch > MAXCH) { fprintf(stderr, "bad --ch %d (max %d)\n", ch, MAXCH); return 2; }
    if (select_grid(ch, rate) != 0) return 2;
    if (raw && strcmp(out, "-")) { fprintf(stderr, "--raw writes to stdout: use '-' as the output\n"); return 2; }

    size_t frames = (size_t)(secs * rate);
    long per = stimulus_period_frames(ch, rate);
    int rc;
    if (raw) {
        gen_banner("raw stream", ch, rate, secs, per);
        rc = gen_raw(ch, rate, frames);
    } else {
        if ((uint64_t)frames * ch * sizeof(int32_t) > UINT32_MAX - 36) {
            fprintf(stderr, "%s: %.0fs x %dch is too large for a WAV (32-bit sizes) -- use gen - --raw\n",
                    out, secs, ch);
            return 1;
        }
        int32_t *samp = malloc(frames * ch * sizeof(int32_t));
        if (!samp) { fprintf(stderr, "oom (%.0fs x %dch)\n", secs, ch); return 1; }
        gen_fill(samp, 0, frames, ch, rate);
        rc = wav_write(out, ch, rate, samp, frames);
        free(samp);
        if (rc == 0)
            gen_banner(out, ch, rate, secs, per);
    }
    return rc ? 1 : 0;
}

static double goertzel(const double *x, size_t N, double f, int rate)
{
    double w = 2.0 * M_PI * f / rate;
    double cw = cos(w), coeff = 2.0 * cw;
    double s0 = 0, s1 = 0, s2 = 0;
    for (size_t n = 0; n < N; n++) { s0 = x[n] + coeff * s1 - s2; s2 = s1; s1 = s0; }
    double re = s1 - s2 * cw, im = s2 * sin(w);
    return 2.0 * sqrt(re * re + im * im) / (double)N;
}

static void goertzel_c(const double *x, size_t N, double f, int rate, size_t n0,
                       double *mag, double *phase)
{
    double w = TWO_PI * f / rate;
    double cw = cos(w), coeff = 2.0 * cw;
    double s0 = 0, s1 = 0, s2 = 0;
    for (size_t n = 0; n < N; n++) { s0 = x[n] + coeff * s1 - s2; s2 = s1; s1 = s0; }
    double re = s1 - s2 * cw, im = s2 * sin(w);

    *mag = 2.0 * sqrt(re * re + im * im) / (double)N;

    double ref = TWO_PI * fmod(f * (double)n0, (double)rate) / (double)rate;
    double p = atan2(im, re) - ref;
    p = remainder(p, TWO_PI);
    *phase = p;
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static double median_of(double *v, size_t n)
{
    if (n == 0) return 0.0;
    qsort(v, n, sizeof *v, cmp_dbl);
    return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

struct chan_metrics {
    double f_meas;
    double amp_dbfs;
    double xtalk_db;
    double noise_db;
    double dc_dbfs;
    double level_dbfs;
    double dom_dbfs;
    int    map_to;
    long   glitches;
};

static double refine_freq(const double *x, size_t N, double f0, int rate)
{
    double best_f = f0, best_m = -1;
    for (double f = f0 - 8.0; f <= f0 + 8.0; f += 0.25) {
        double m = goertzel(x, N, f, rate);
        if (m > best_m) { best_m = m; best_f = f; }
    }
    return best_f;
}

static void read_note(const char *path, int k, int n)
{
    struct stat st;
    double gb = stat(path, &st) == 0 ? st.st_size / 1e9 : 0;

    if (n > 1)
        fprintf(stderr, "   reading part %d/%d: %s (%.2f GB)\n", k, n, path, gb);
    else if (gb >= 0.5)
        fprintf(stderr, "   reading %s (%.2f GB)\n", path, gb);
}

static int cmd_check(int argc, char **argv)
{
    const char *recs[64], *baseline = NULL, *establish = NULL;
    int nrec = 0;
    int want_ch = 0, want_rate = 0, probe = 0, live_ch = 0;
    long glitch_budget = GLITCH_BUDGET_DEF;
    long splice_budget = SPLICE_BUDGET_DEF;
    for (int i = 0; i < argc; i++) {
        if      (!strcmp(argv[i], "--ch")    && i+1 < argc) want_ch   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rate")  && i+1 < argc) want_rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--baseline") && i+1 < argc) baseline = argv[++i];
        else if (!strcmp(argv[i], "--establish-baseline") && i+1 < argc) establish = argv[++i];
        else if (!strcmp(argv[i], "--glitch-budget") && i+1 < argc) glitch_budget = atol(argv[++i]);
        else if (!strcmp(argv[i], "--splice-budget") && i+1 < argc) splice_budget = atol(argv[++i]);
        else if (!strcmp(argv[i], "--live-ch") && i+1 < argc) live_ch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--probe")) probe = 1;
        else if (argv[i][0] != '-') {
            if (nrec == (int)(sizeof recs / sizeof recs[0])) { fprintf(stderr, "too many recording parts\n"); return 2; }
            recs[nrec++] = argv[i];
        }
    }
    const char *rec = nrec ? recs[0] : NULL;
    if (!rec) { fprintf(stderr, "usage: audio_check check <rec.wav> [<part2.wav> ...] [--baseline f|--establish-baseline f]"
                                " [--glitch-budget N] [--splice-budget N] [--live-ch N] [--probe]\n"); return 2; }
    if (live_ch < 0) { fprintf(stderr, "bad --live-ch %d\n", live_ch); return 2; }

    int ch, rate; int32_t *raw; size_t frames;
    if (!probe)
        read_note(rec, 1, nrec);
    if (rec_read(rec, want_ch, want_rate, &ch, &rate, &raw, &frames) != 0) return 3;
    for (int r = 1; r < nrec; r++) {
        int pch, prate; int32_t *part, *grown; size_t pfr;

        if (!probe)
            read_note(recs[r], r + 1, nrec);
        if (rec_read(recs[r], want_ch, want_rate, &pch, &prate, &part, &pfr) != 0) { free(raw); return 3; }
        if (pch != ch || prate != rate) {
            fprintf(stderr, "%s: %dch @ %dHz does not match %s (%dch @ %dHz)\n",
                    recs[r], pch, prate, rec, ch, rate);
            free(part); free(raw); return 3;
        }
        grown = realloc(raw, (frames + pfr) * ch * sizeof(int32_t));
        if (!grown) { fprintf(stderr, "oom joining %s\n", recs[r]); free(part); free(raw); return 3; }
        raw = grown;
        memcpy(raw + frames * ch, part, pfr * ch * sizeof(int32_t));
        frames += pfr;
        free(part);
    }
    if (want_ch   && ch   != want_ch)   fprintf(stderr, "warn: recorded ch=%d, expected %d\n", ch, want_ch);
    if (want_rate && rate != want_rate) fprintf(stderr, "warn: recorded rate=%d, expected %d\n", rate, want_rate);
    if (ch > MAXCH) { fprintf(stderr, "ch %d > MAXCH\n", ch); free(raw); return 3; }
    if (select_grid(ch, rate) != 0) { free(raw); return 3; }
    int live = (live_ch > 0 && live_ch < ch) ? live_ch : ch;

    double settle_s = probe ? 0.5 : SETTLE_S;
    double tail_s   = probe ? 0.2 : TAIL_S;
    size_t start = (size_t)(settle_s * rate);
    size_t avail = frames > (size_t)(tail_s * rate) ? frames - (size_t)(tail_s * rate) : 0;
    if (avail <= start) {
        fprintf(stderr, "recording too short: %.2fs (need > %.1fs)\n", (double)frames/rate, settle_s+tail_s);
        free(raw); return 3;
    }
    size_t N = avail - start;

    {
        long per = stimulus_period_frames(ch, rate);

        if (per > 0 && (size_t)per <= N) N -= N % (size_t)per;
    }

    size_t P  = 1, nb = 1;
    {
        long per = stimulus_period_frames(ch, rate);
        P  = (per > 0 && (size_t)per <= N) ? (size_t)per : N;
        nb = N / P;
    }

    double *x     = malloc(N * sizeof(double));
    double *ej    = malloc(nb * (size_t)ch * sizeof(double));
    double *tmp   = malloc(nb * sizeof(double));
    double *bnz   = malloc(nb * sizeof(double));
    double *blv   = malloc(nb * sizeof(double));
    double *theta = malloc(nb * (size_t)ch * sizeof(double));
    size_t *gfirst = malloc(nb * sizeof(size_t));
    if (!x || !ej || !tmp || !bnz || !blv || !theta || !gfirst) {
        free(x); free(ej); free(tmp); free(bnz); free(blv); free(theta); free(gfirst); free(raw);
        fprintf(stderr, "oom\n"); return 1;
    }
    for (size_t b = 0; b < nb; b++) gfirst[b] = (size_t)-1;

    struct chan_metrics m[MAXCH];
    int any_silent = 0;
    char silent[MAXCH] = { 0 };
    struct progress prog;

    progress_init(&prog);
    if (probe)
        prog.shown = -1;
    for (int c = 0; c < ch; c++) {
        if (c && prog.shown >= 0)
            progress_step(&prog, c, ch);
        double dc = 0;
        for (size_t n = 0; n < N; n++) { double v = (double)raw[(start + n) * ch + c]; x[n] = v; dc += v; }
        dc /= (double)N;
        for (size_t n = 0; n < N; n++) x[n] -= dc;

        for (size_t b = 0; b < nb; b++) {
            const double *xb = x + b * P;

            for (int j = 0; j < ch; j++) {
                if (j == c) {
                    double mag, ph;
                    goertzel_c(xb, P, tone_freq(c), rate, start + b * P, &mag, &ph);
                    ej[b * ch + j] = mag;
                    theta[b * ch + c] = ph;
                } else {
                    ej[b * ch + j] = goertzel(xb, P, tone_freq(j), rate);
                }
            }

            double sumsq = 0;
            for (size_t n = 0; n < P; n++) sumsq += xb[n] * xb[n];
            double rms = sqrt(sumsq / (double)P);
            double tone_rms = ej[b * ch + c] / sqrt(2.0);
            double resid = rms*rms - tone_rms*tone_rms; if (resid < 0) resid = 0;
            bnz[b] = sqrt(resid);
            blv[b] = rms;
        }

        double med[MAXCH];
        for (int j = 0; j < ch; j++) {
            for (size_t b = 0; b < nb; b++) tmp[b] = ej[b * ch + j];
            med[j] = median_of(tmp, nb);
        }
        double self_e = med[c];
        double worst_foreign = 0; int dominant = c; double dom_e = self_e;
        for (int j = 0; j < ch; j++) {
            if (med[j] > dom_e) { dom_e = med[j]; dominant = j; }
            if (j != c && med[j] > worst_foreign) worst_foreign = med[j];
        }
        double noise_rms = median_of(bnz, nb);

        size_t rn = N < 4 * P ? N : 4 * P;
        m[c].f_meas   = refine_freq(x, rn, tone_freq(c), rate);
        m[c].amp_dbfs = self_e > 0 ? 20.0 * log10(self_e / FS32) : -200.0;
        m[c].xtalk_db = self_e > 0 ? 20.0 * log10((worst_foreign + 1e-12) / self_e) : 0.0;
        m[c].noise_db = self_e > 0 ? 20.0 * log10((noise_rms + 1e-12) / self_e)   : 0.0;
        m[c].dc_dbfs  = 20.0 * log10((fabs(dc) + 1e-12) / FS32);
        m[c].map_to   = dominant;
        m[c].dom_dbfs = dom_e > 0 ? 20.0 * log10(dom_e / FS32) : -200.0;
        {
            double lv = median_of(blv, nb);
            double tot = sqrt(2.0 * lv * lv + dc * dc);
            m[c].level_dbfs = 20.0 * log10((tot + 1e-12) / FS32);
        }
        if (c >= live) {
            m[c].glitches = 0;
            continue;
        }
        if (self_e < FS32 * pow(10.0, AMP_SANITY_LO_DBFS/20.0)) any_silent = silent[c] = 1;

        double w = 2.0 * M_PI * m[c].f_meas / rate, coeff = 2.0 * cos(w);
        double thresh = 0.5 * self_e;
        long g = 0; int armed = 1;
        if (thresh < 1.0) thresh = 1.0;
        for (size_t n = 2; n < N; n++) {
            double r = fabs(x[n] - coeff * x[n-1] + x[n-2]);
            if (r > thresh) {
                if (armed) {
                    g++; armed = 0;
                    size_t blk = n / P;
                    if (blk < nb && n < gfirst[blk]) gfirst[blk] = n;
                }
            }
            else armed = 1;
        }
        m[c].glitches = g;
    }
    if (prog.shown > 0)
        progress_done(&prog, "analysed all channels");
    free(x); free(ej); free(tmp); free(bnz); free(blv); free(raw);

    if (probe) {
        free(theta); free(gfirst);
        if (any_silent) {
            fprintf(stderr, "PROBE: at least one of channels 0-%d is silent -- loopback not live.\n", live - 1);
            fprintf(stderr, "PROBE: silent:");
            for (int c = 0; c < live; c++) {
                int e = c;

                if (!silent[c])
                    continue;
                while (e + 1 < live && silent[e + 1])
                    e++;
                fprintf(stderr, e > c ? " ch%d-%d" : " ch%d", c, e);
                c = e;
            }
            fprintf(stderr, "\n");
            return 4;
        }
        fprintf(stderr, "PROBE: all %d live channels carry signal -- loopback live.\n", live);
        return 0;
    }

    double base_amp[MAXCH]; int have_base = 0;
    if (baseline) {
        FILE *bf = fopen(baseline, "r");
        if (bf) {
            char line[256]; int bch = 0, br = 0;
            while (fgets(line, sizeof line, bf)) {
                int c; double a;
                if (sscanf(line, "ch %d amp_dbfs %lf", &c, &a) == 2 && c >= 0 && c < MAXCH) base_amp[c] = a;
                else if (sscanf(line, "channels %d", &bch) == 1) {}
                else if (sscanf(line, "rate %d", &br) == 1) {}
            }
            fclose(bf);
            have_base = 1;
            if (bch && bch != ch) fprintf(stderr, "warn: baseline ch=%d != %d\n", bch, ch);
        } else {
            fprintf(stderr, "warn: cannot open baseline %s -- using absolute amplitude sanity band\n", baseline);
        }
    }

    if (establish) {
        FILE *bf = fopen(establish, "w");
        if (!bf) { perror(establish); return 1; }
        fprintf(bf, "# audio_check baseline\nchannels %d\nrate %d\nglitch_budget %ld\n", ch, rate, glitch_budget);
        for (int c = 0; c < live; c++) fprintf(bf, "ch %d amp_dbfs %.2f\n", c, m[c].amp_dbfs);
        fclose(bf);
        fprintf(stderr, "baseline written: %s (%d channels, +/-%.0f dB band on subsequent runs)\n",
                establish, live, AMP_BAND_DB);
    }

    int fail = 0;
    long total_glitch = 0;
    double t_before = get_time_s();
    printf("== audio loopback check: %s%s  (%dch @ %dHz, analyzed %.1fs)\n",
           rec, nrec > 1 ? " + further parts" : "", ch, rate, (double)N / rate);
    if (nrec > 1)
        printf("   joined %d recording parts\n", nrec);
    printf("   amp/xtalk/noise are medians over %zu blocks of %zu frames (%.3fs)\n",
           nb, P, (double)P / rate);
    printf("   analysis start: %.6fs\n", t_before);
    if (live < ch)
        printf("   live channels 0-%d; channels %d-%d must be silent (< %.0f dBFS)\n",
               live - 1, live, ch - 1, SILENT_MAX_DBFS);
    printf("  %-3s %-9s %-9s %-9s %-9s %-9s %-7s %s\n",
           "ch", "freqHz", "ampdBFS", "xtalkdB", "noisedB", "dcdBFS", "glitch", "verdict");
    for (int c = 0; c < ch; c++) {
        int cfail = 0;
        char why[128] = "";
        if (c >= live) {
            if (m[c].level_dbfs >= SILENT_MAX_DBFS) {
                cfail = 1;
                if (m[c].dom_dbfs >= m[c].level_dbfs - 6.0)
                    snprintf(why, sizeof why, "LEAK %.1fdBFS (ch%d tone)", m[c].level_dbfs, m[c].map_to);
                else
                    snprintf(why, sizeof why, "LEAK %.1fdBFS (broadband)", m[c].level_dbfs);
            }
            printf("  %-3d %-9s %-9.1f %-9s %-9s %-9.0f %-7s %s%s\n",
                   c, "-", m[c].level_dbfs, "-", "-", m[c].dc_dbfs, "-",
                   cfail ? "FAIL " : "ok (silent)", why);
            if (cfail) fail = 1;
            continue;
        }
        if (m[c].map_to != c) { cfail = 1; snprintf(why, sizeof why, "MAP->ch%d", m[c].map_to); }
        else if (fabs(m[c].f_meas - tone_freq(c)) > FREQ_TOL_HZ)
            { cfail = 1; snprintf(why, sizeof why, "FREQ off %.1f", m[c].f_meas - tone_freq(c)); }
        else if (m[c].xtalk_db > CROSSTALK_DB_MAX)
            { cfail = 1; snprintf(why, sizeof why, "XTALK %.0fdB", m[c].xtalk_db); }
        else if (m[c].noise_db > NOISE_FLOOR_DB)
            { cfail = 1; snprintf(why, sizeof why, "NOISE %.0fdB", m[c].noise_db); }
        else {
            double lo = AMP_SANITY_LO_DBFS, hi = AMP_SANITY_HI_DBFS;
            if (have_base) { lo = base_amp[c] - AMP_BAND_DB; hi = base_amp[c] + AMP_BAND_DB; }
            if (m[c].amp_dbfs < lo || m[c].amp_dbfs > hi)
                { cfail = 1; snprintf(why, sizeof why, "AMP %.1f want[%.1f,%.1f]", m[c].amp_dbfs, lo, hi); }
        }
        total_glitch += m[c].glitches;
        printf("  %-3d %-9.1f %-9.1f %-9.0f %-9.0f %-9.0f %-7ld %s%s\n",
               c, m[c].f_meas, m[c].amp_dbfs, m[c].xtalk_db, m[c].noise_db, m[c].dc_dbfs,
               m[c].glitches, cfail ? "FAIL " : "ok", why);
        if (cfail) fail = 1;
    }

    long nsplice = 0;
    if (nb >= 2) {
        double *tgt = malloc((size_t)ch * P * sizeof(double));
        long   *Db  = malloc(nb * sizeof(long));
        double *fitb = malloc(nb * sizeof(double));
        if (!tgt || !Db || !fitb) {
            fprintf(stderr, "oom (splice)\n");
            free(tgt); free(Db); free(fitb); free(theta); free(gfirst); return 1;
        }
        for (int c = 0; c < ch; c++) {
            double f = tone_freq(c);
            for (size_t di = 0; di < P; di++) {
                double t = TWO_PI * fmod(f * (double)di, (double)rate) / (double)rate;
                tgt[(size_t)c * P + di] = t > M_PI ? t - TWO_PI : t;
            }
        }

        for (size_t b = 0; b < nb; b++) {
            double dth[MAXCH];
            for (int c = 0; c < live; c++) {
                double d = theta[b * ch + c] - theta[c];
                if (d > M_PI) d -= TWO_PI; else if (d < -M_PI) d += TWO_PI;
                dth[c] = d;
            }
            double best = 1e300; size_t bestdi = 0;
            for (size_t di = 0; di < P; di++) {
                double e2 = 0;
                for (int c = 0; c < live; c++) {
                    double d = dth[c] - tgt[(size_t)c * P + di];
                    if (d > M_PI) d -= TWO_PI; else if (d < -M_PI) d += TWO_PI;
                    e2 += d * d;
                    if (e2 >= best) break;
                }
                if (e2 < best) { best = e2; bestdi = di; }
            }
            fitb[b] = sqrt(best / (double)live);
            Db[b]   = (long)bestdi;
            if (Db[b] > (long)(P / 2)) Db[b] -= (long)P;
        }
        free(tgt);

        char *ok = malloc(nb);
        if (!ok) { fprintf(stderr, "oom (splice)\n"); free(Db); free(fitb); free(theta); free(gfirst); return 1; }
        for (size_t b = 0; b < nb; b++) ok[b] = fitb[b] <= SPLICE_FIT_MAX;
        for (size_t b = 1; b + 1 < nb; b++)
            if (ok[b] && Db[b] != Db[b-1] && Db[b] != Db[b+1]) ok[b] = 0;

        int shown = 0;
        long prevD = 0; size_t prev_good = (size_t)-1;
        for (size_t b = 0; b < nb; b++) {
            if (!ok[b]) continue;
            if (prev_good == (size_t)-1) { prev_good = b; prevD = Db[b]; continue; }

            long dd = Db[b] - prevD;
            if (dd >  (long)(P / 2)) dd -= (long)P;
            if (dd < -(long)(P / 2)) dd += (long)P;

            if (dd != 0 || b != prev_good + 1) {
                nsplice++;
                if (shown < 12) {
                    size_t at = (size_t)-1;
                    for (size_t k = prev_good + 1; k <= b && at == (size_t)-1; k++)
                        if (gfirst[k] != (size_t)-1) at = gfirst[k];

                    char where[64];
                    if (at != (size_t)-1)
                        snprintf(where, sizeof where, "t=%.4fs", (double)(start + at) / rate);
                    else
                        snprintf(where, sizeof where, "t=%.3f..%.3fs",
                                 (double)(start + (prev_good + 1) * P) / rate,
                                 (double)(start + (b + 1) * P) / rate);

                    if (dd == 0)
                        printf("   %-22s disturbed, no net offset (%zu block(s) unfittable)\n",
                               where, b - prev_good - 1);
                    else
                        printf("   %-22s source jump %+ld frames (%+.3f ms)  fit %.3f rad\n",
                               where, dd, 1000.0 * (double)dd / rate, fitb[b]);
                    shown++;
                } else if (shown == 12) {
                    printf("   ... (further breaks suppressed)\n");
                    shown++;
                }
            }
            prev_good = b; prevD = Db[b];
        }

        free(Db); free(fitb); free(ok);
        printf("== splices: %ld (budget %ld) %s   [+D = frames skipped; offset resolvable mod %zu]\n",
               nsplice, splice_budget, nsplice <= splice_budget ? "within" : "OVER", P);
        if (nsplice > splice_budget) fail = 1;
    } else {
        printf("== splices: n/a (need >= 2 blocks of %zu frames; window is %zu)\n", P, N);
    }
    free(theta); free(gfirst);

    double t_after = get_time_s();
    printf("== glitches: %ld total (budget %ld) %s\n",
           total_glitch, glitch_budget, total_glitch <= glitch_budget ? "within" : "OVER");
    if (total_glitch > glitch_budget) fail = 1;

    if (establish) {
        printf("== BASELINE ESTABLISHED (amplitude band recorded; not a pass/fail run)\n");
        return 0;
    }
    if (!have_base)
        printf("   (no baseline: amplitude judged against absolute sanity band [%.0f,%.0f] dBFS)\n",
               AMP_SANITY_LO_DBFS, AMP_SANITY_HI_DBFS);
    printf("== VERDICT: %s\n", fail ? "FAIL -- audio regression detected" : "PASS -- no audio regression");
    printf("   analysis time: %.3fs (%zu blocks processed)\n", t_after - t_before, nb);
    return fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage:\n"
            "  audio_check gen   <out.wav> [--ch N --rate R --secs S]\n"
            "  audio_check gen   - --raw   [--ch N --rate R --secs S]   (raw S32_LE to stdout)\n"
            "  audio_check check <rec.wav|rec.raw> [<part2> ...] [--ch N --rate R]   (.raw needs --ch/--rate)\n"
            "                    [--baseline f | --establish-baseline f]\n"
            "                    [--glitch-budget N] [--splice-budget N] [--live-ch N] [--probe]\n"
            "exit: 0=PASS  1=FAIL(regression)  2=usage  3=unreadable  4=loopback-not-live(probe)\n");
        return 2;
    }
    if (!strcmp(argv[1], "gen"))   return cmd_gen(argc - 2, argv + 2);
    if (!strcmp(argv[1], "check")) return cmd_check(argc - 2, argv + 2);
    fprintf(stderr, "unknown subcommand '%s'\n", argv[1]);
    return 2;
}