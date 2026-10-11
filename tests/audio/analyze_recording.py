#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
"""
Analyze the AVB loopback WAV file to identify distortion patterns.
Maps each recorded frame back to a stimulus phase to locate splices.
"""

import wave
import struct
import math
import sys
import numpy as np
from pathlib import Path

REC_FILE = str(Path.home() / "tmp" / "avb-loopback.wav")
RATE = 48000
CH = 8
# Must match FBASE/FSTEP in audio_check.c.  These are deliberately coprime with
# the sample rate so the composite stimulus period is a full second (48000
# frames) rather than 800 -- see the FBASE comment in audio_check.c for why that
# matters when mapping recorded audio back to source frame positions.
BASE_FREQ = 661.0
STEP_FREQ = 181.0

def read_wav(path):
    """Read WAV file and return samples, rate, channels."""
    with wave.open(path, 'rb') as wf:
        rate = wf.getframerate()
        nch = wf.getnchannels()
        samples = wf.getnframes()
        data = wf.readframes(samples * nch * 4)  # S32_LE = 4 bytes per sample
        # Parse as 32-bit signed integers
        fmt = f'{samples * nch}i'
        raw = struct.unpack(fmt, data)
        # Reshape to (samples, channels)
        arr = np.array(raw, dtype=np.int32).reshape((samples, nch))
        return arr, rate, nch

def extract_channel(data, channel):
    """Extract a single channel from interleaved data."""
    return data[:, channel]

def compute_goertzel(x, n, freq, rate):
    """Goertzel algorithm to detect frequency magnitude."""
    w = 2.0 * math.pi * freq / rate
    cw = math.cos(w)
    coeff = 2.0 * cw
    s0 = s1 = s2 = 0.0
    for i in range(n):
        s0 = x[i] + coeff * s1 - s2
        s2, s1 = s1, s0
    re = s1 - s2 * cw
    im = s2 * math.sin(w)
    return 2.0 * math.sqrt(re * re + im * im) / n

def find_distortion_region(data, rate, ch):
    """Analyze the recording to find where distortion starts."""
    print(f"Analyzing {data.shape[0]} frames @ {rate}Hz, {ch} channels")
    print(f"Duration: {data.shape[0] / rate:.2f} seconds")
    
    # Calculate expected frequencies
    expected_freqs = [BASE_FREQ + STEP_FREQ * c for c in range(ch)]
    print(f"Expected frequencies: {expected_freqs}")
    
    # Analyze in 1-second windows
    window_size = rate  # 1 second of samples
    step = window_size
    
    # Track amplitude and frequency stability per channel
    for c in range(ch):
        channel_data = extract_channel(data, c)
        print(f"\n=== Channel {c} (expected: {expected_freqs[c]:.1f} Hz) ===")
        
        prev_amp = None
        prev_freq = None
        
        for start in range(0, len(channel_data) - window_size, step):
            window = channel_data[start:start + window_size]
            
            # Compute DC offset
            dc = np.mean(window)
            centered = window - dc
            
            # Find dominant frequency
            best_freq = expected_freqs[c]
            best_mag = compute_goertzel(centered, window_size, expected_freqs[c], rate)
            
            # Check neighboring frequencies
            for df in np.arange(-8, 9, 0.5):
                f = expected_freqs[c] + df
                mag = compute_goertzel(centered, window_size, f, rate)
                if mag > best_mag:
                    best_mag = mag
                    best_freq = f
            
            # Calculate amplitude
            amplitude = best_mag / (2**31)  # Normalize to full scale
            amp_dbfs = 20 * math.log10(amplitude + 1e-12)
            
            # Check for overtones (energy at harmonics)
            fundamental = best_freq
            harmonic_energy = 0
            for harmonic in range(2, 5):
                harm_freq = fundamental * harmonic
                if harm_freq < rate / 2:
                    harm_mag = compute_goertzel(centered, window_size, harm_freq, rate)
                    harmonic_energy += harm_mag
            
            # Noise floor (all energy not at fundamental)
            total_energy = np.sum(centered**2) / len(centered)
            fundamental_energy = (best_mag / math.sqrt(2)) ** 2
            noise_energy = max(total_energy - fundamental_energy, 1e-12)
            noise_db = 20 * math.log10(math.sqrt(noise_energy) / (best_mag + 1e-12))
            
            # Detect anomalies
            anomalies = []
            if abs(amp_dbfs + 6.0) > 1.0:  # Amplitude should be around -6 dBFS
                anomalies.append(f"AMP={amp_dbfs:.1f}dBFS")
            if abs(best_freq - expected_freqs[c]) > 1.0:
                anomalies.append(f"FREQ={best_freq:.1f}Hz")
            if noise_db > -100:  # Noise should be very low
                anomalies.append(f"NOISE={noise_db:.1f}dB")
            
            if anomalies:
                sec = start / rate
                print(f"  {sec:6.1f}s: {' | '.join(anomalies)}")
                
                # Also check if this is around 71 seconds
                if 69 <= sec <= 73:
                    print(f"    ^^^ DISTORTION START REGION")

if __name__ == "__main__":
    data, rate, ch = read_wav(sys.argv[1] if len(sys.argv) > 1 else REC_FILE)
    find_distortion_region(data, rate, ch)