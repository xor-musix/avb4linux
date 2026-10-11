#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
"""
Analyze WAV file to detect audio quality issues.

This script reads a WAV file and analyzes it for:
- Periodic distortion patterns
- Sample dropouts/duplications
- Buffer underrun artifacts
"""

import struct
import sys
import numpy as np

def read_wav(filepath):
    """Read WAV file and return audio data and metadata."""
    with open(filepath, 'rb') as f:
        # Read RIFF header
        riff = f.read(4)
        if riff != b'RIFF':
            raise ValueError("Not a WAV file")
        
        # Read file size
        file_size = struct.unpack('<I', f.read(4))[0]
        
        # Read WAVE header
        wave = f.read(4)
        if wave != b'WAVE':
            raise ValueError("Not a WAVE file")
        
        # Read format chunk
        fmt = f.read(4)
        if fmt != b'fmt ':
            raise ValueError("Invalid WAV format")
        
        fmt_size = struct.unpack('<I', f.read(4))[0]
        audio_format = struct.unpack('<H', f.read(2))[0]
        num_channels = struct.unpack('<H', f.read(2))[0]
        sample_rate = struct.unpack('<I', f.read(4))[0]
        byte_rate = struct.unpack('<I', f.read(4))[0]
        block_align = struct.unpack('<H', f.read(2))[0]
        bits_per_sample = struct.unpack('<H', f.read(2))[0]
        
        # Skip extra bytes if any
        if fmt_size > 16:
            f.read(fmt_size - 16)
        
        # Read data chunk
        data_header = f.read(4)
        if data_header != b'data':
            raise ValueError("Invalid data chunk")
        
        data_size = struct.unpack('<I', f.read(4))[0]
        data = f.read(data_size)
        
        # Parse audio data
        if bits_per_sample == 32:
            samples = np.frombuffer(data, dtype=np.int32)
        elif bits_per_sample == 24:
            # Convert 24-bit to 32-bit
            samples = np.zeros(len(data) // 3, dtype=np.int32)
            for i in range(len(samples)):
                samples[i] = int.from_bytes(data[i*3:(i+1)*3], 'little', signed=True)
        else:
            raise ValueError(f"Unsupported bits per sample: {bits_per_sample}")
        
        # Reshape to (samples, channels)
        samples = samples.reshape(-1, num_channels)
        
        return {
            'num_channels': num_channels,
            'sample_rate': sample_rate,
            'bits_per_sample': bits_per_sample,
            'samples': samples,
            'duration': len(samples) / sample_rate
        }

def analyze_audio(data):
    """Analyze audio data for quality issues."""
    samples = data['samples']
    sample_rate = data['sample_rate']
    num_channels = data['num_channels']
    
    print(f"Audio file: {len(samples):,} samples, {data['duration']:.1f}s")
    print(f"Channels: {num_channels}, Sample rate: {sample_rate} Hz")
    print()
    
    # Analyze each channel
    for ch in range(num_channels):
        ch_data = samples[:, ch].astype(np.float64)
        
        # Find zero crossings (test tone at 216 Hz)
        # At 48kHz, one cycle is ~222 samples
        # 216 Hz = 48000/216 ≈ 222.22 samples per cycle
        cycle_length = sample_rate / 216
        
        # Look for consistent sample patterns in sine wave
        # A perfect sine at 216 Hz should have repeating patterns
        
        # Find peaks
        peaks = []
        for i in range(1, len(ch_data)-1):
            if ch_data[i] > ch_data[i-1] and ch_data[i] > ch_data[i+1]:
                peaks.append(i)
        
        if len(peaks) > 10:
            # Analyze peak intervals
            peak_intervals = [peaks[i+1] - peaks[i] for i in range(len(peaks)-1)]
            avg_interval = np.mean(peak_intervals)
            
            print(f"Channel {ch}:")
            print(f"  Found {len(peaks)} peaks")
            print(f"  Average peak interval: {avg_interval:.1f} samples (expected ~{cycle_length:.1f})")
            
            # Check for irregularities
            if avg_interval > cycle_length * 1.1:
                print(f"  WARNING: Peak interval too large - possible sample dropping")
            elif avg_interval < cycle_length * 0.9:
                print(f"  WARNING: Peak interval too small - possible sample duplication")
            
            # Check interval consistency
            interval_std = np.std(peak_intervals)
            if interval_std > cycle_length * 0.1:
                print(f"  WARNING: High interval variation (std={interval_std:.1f})")
            else:
                print(f"  Peak intervals consistent (std={interval_std:.1f})")
        
        # Check for sample value consistency
        # At peak values, samples should follow sine pattern
        print(f"  Sample range: {np.min(ch_data):.0f} to {np.max(ch_data):.0f}")
        
        # Find consecutive identical values (sample duplication)
        dup_count = 0
        for i in range(1, len(ch_data)):
            if ch_data[i] == ch_data[i-1]:
                dup_count += 1
        
        if dup_count > 0:
            print(f"  WARNING: Found {dup_count} consecutive identical samples (duplication)")
        
        # Find zero samples (dropouts)
        zero_count = np.sum(ch_data == 0)
        if zero_count > 0:
            print(f"  WARNING: Found {zero_count} zero samples (dropouts)")
        
        print()

def detect_periodic_pattern(data):
    """Detect periodic patterns in audio that might indicate clock issues."""
    samples = data['samples']
    sample_rate = data['sample_rate']
    
    # Look for repeating patterns every few samples
    # If there's a clock issue, we might see repeating sequences
    
    # Analyze first channel
    ch0 = samples[:, 0].astype(np.float64)
    
    # Look for repeating pattern every N samples
    print("=== Periodic Pattern Analysis ===")
    
    # Check for repeating patterns (sample duplication)
    pattern_length = 6  # 6 samples per packet at 48kHz / 8000 Hz
    pattern_repeats = 0
    
    for i in range(0, len(ch0) - pattern_length * 2, pattern_length):
        if np.array_equal(ch0[i:i+pattern_length], ch0[i+pattern_length:i+pattern_length*2]):
            pattern_repeats += 1
    
    if pattern_repeats > 100:
        print(f"WARNING: Found {pattern_repeats} repeating {pattern_length}-sample patterns")
        print("This may indicate sample duplication due to clock mismatch")
    
    # Look for pattern of increases/decreases
    diff = np.diff(ch0)
    
    # Count zero differences (duplicates)
    zero_diffs = np.sum(diff == 0)
    if zero_diffs > 0:
        print(f"Found {zero_diffs} consecutive identical samples (duplicates)")
    
    # Count negative differences during rising phase (dropouts)
    # This is a simplified check - look for unexpected value changes
    
    # Check sample rate consistency by looking at time between zero crossings
    zero_crossings = []
    for i in range(1, len(ch0)):
        if (ch0[i-1] < 0 and ch0[i] >= 0) or (ch0[i-1] >= 0 and ch0[i] < 0):
            zero_crossings.append(i)
    
    if len(zero_crossings) > 10:
        zc_intervals = [zero_crossings[i+1] - zero_crossings[i] for i in range(len(zero_crossings)-1)]
        avg_zc = np.mean(zc_intervals)
        expected_zc = sample_rate / 216  # 216 Hz tone
        
        print(f"\nZero crossing analysis:")
        print(f"  Average zero crossing interval: {avg_zc:.1f} samples")
        print(f"  Expected (216 Hz @ {sample_rate} Hz): {expected_zc:.1f}")
        
        if abs(avg_zc - expected_zc) > expected_zc * 0.05:
            print(f"  WARNING: Zero crossing interval deviates from expected by {abs(avg_zc - expected_zc)/expected_zc*100:.1f}%")

def main():
    if len(sys.argv) < 2:
        print("Usage: analyze_wav.py <file.wav>")
        sys.exit(1)
    
    filepath = sys.argv[1]
    
    print(f"Analyzing {filepath}...")
    print()
    
    try:
        data = read_wav(filepath)
        analyze_audio(data)
        detect_periodic_pattern(data)
    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()

if __name__ == '__main__':
    main()