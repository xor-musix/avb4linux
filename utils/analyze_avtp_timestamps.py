#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
"""
Analyze AVTP timestamps from PCAP file to diagnose audio clock issues.

This script extracts AVTP timestamps from the raw pcap file and analyzes
for timing irregularities that cause audio distortion.
"""

import sys
import struct
from collections import defaultdict

def parse_pcap_raw(pcap_file):
    """Parse PCAP file and extract AVTP packets with timestamps."""
    packets = []
    
    with open(pcap_file, 'rb') as f:
        # PCAP header (24 bytes)
        magic = f.read(4)
        if magic == b'\xd4\xc3\xb2\xa1':
            endian = '<'  # Little endian
        elif magic == b'\xa1\xb2\xc3\xd4':
            endian = '>'  # Big endian
        else:
            print(f"Invalid PCAP magic: {magic.hex()}")
            return packets
        
        f.read(20)  # rest of header
        
        while True:
            # Packet header (16 bytes)
            hdr = f.read(16)
            if len(hdr) < 16:
                break
            
            ts_sec, ts_usec, incl_len, _ = struct.unpack(f'{endian}IIII', hdr)
            ts = ts_sec + ts_usec / 1_000_000.0
            
            # Read packet data
            data = f.read(incl_len)
            if len(data) < 40:
                continue
            
            # Parse Ethernet header (14 bytes)
            if data[12:14] != b'\x81\x00':  # VLAN
                continue
            
            # AVTP header starts after VLAN (bytes 14-17):
            # Bytes 14-15: inner VLAN type (0x6002)
            # Bytes 16-17: IEC 61883 subtype (0x22f0)
            # Byte 18: AVTP control
            # Byte 19: Stream format (bit 2 = Timestamp Valid)
            # Bytes 32-35: AVTP Timestamp
            
            # Check for IEC 61883 subtype (0x22f0 at byte 16-17)
            if data[16:18] != b'\x22\xf0':
                continue
            
            stream_format = data[19]
            if not (stream_format & 0x01):  # Timestamp Valid bit (bit 0)
                continue
            
            # AVTP timestamp is at bytes 30-33 (after Stream ID at 22-29)
            # But we need to check if it's at bytes 30-33 or 32-35
            # Let's look at bytes 30-33
            timestamp = struct.unpack('>I', data[30:34])[0]
            
            if timestamp == 0:
                continue
            
            packets.append({
                'ts': ts,
                'timestamp': timestamp
            })
    
    return packets

def analyze_timestamps(packets):
    """Analyze AVTP timestamps for clock issues."""
    if not packets:
        print("No packets found")
        return
    
    print(f"Found {len(packets)} valid AVTP packets")
    
    # Sort by packet timestamp
    packets.sort(key=lambda x: x['ts'])
    
    # Analyze timestamps
    print("\n=== Timestamp Analysis ===")
    
    last_ts = None
    deltas = []
    
    for i, pkt in enumerate(packets):
        ts = pkt['timestamp']
        
        if last_ts is not None:
            expected_diff = 125000  # ns (125 µs for 6 samples @ 48kHz)
            actual_diff = (ts - last_ts) & 0xFFFFFFFF
            jitter = actual_diff - expected_diff
            deltas.append(jitter)
        
        last_ts = ts
    
    if deltas:
        min_jitter = min(deltas)
        max_jitter = max(deltas)
        avg_jitter = sum(deltas) / len(deltas)
        
        print(f"Timestamp jitter (vs expected 125000ns):")
        print(f"  Min: {min_jitter:+.1f} ns")
        print(f"  Max: {max_jitter:+.1f} ns")
        print(f"  Avg: {avg_jitter:+.1f} ns")
        
        # Check for periodic pattern (hiss intervals)
        print("\n=== Periodicity Analysis ===")
        
        # Look for clusters of large jitter (hiss periods)
        LARGE_JITTER_THRESHOLD = 50000  # 50µs is very large for 48kHz
        
        # Find consecutive large jitter
        large_jitter_starts = []
        in_hiss = False
        hiss_start_idx = 0
        
        for i, j in enumerate(deltas):
            if abs(j) > LARGE_JITTER_THRESHOLD:
                if not in_hiss:
                    in_hiss = True
                    hiss_start_idx = i
            else:
                if in_hiss:
                    hiss_duration = i - hiss_start_idx
                    if hiss_duration > 10:  # At least 10 consecutive packets
                        hiss_start_time = packets[hiss_start_idx]['ts']
                        hiss_end_time = packets[i]['ts']
                        hiss_duration_sec = hiss_end_time - hiss_start_time
                        print(f"  Hiss period #{len(large_jitter_starts)+1}: "
                              f"t={hiss_start_time:.2f}s..{hiss_end_time:.2f}s "
                              f"duration={hiss_duration_sec:.1f}s "
                              f"packets={hiss_duration}")
                        large_jitter_starts.append((hiss_start_idx, i))
                    in_hiss = False
        
        # Check if there's a periodic pattern in hiss intervals
        if len(large_jitter_starts) >= 2:
            intervals = []
            for i in range(1, len(large_jitter_starts)):
                prev_end = large_jitter_starts[i-1][1]
                curr_start = large_jitter_starts[i][0]
                interval_time = packets[curr_start]['ts'] - packets[prev_end]['ts']
                intervals.append(interval_time)
            
            if intervals:
                print(f"\nHiss interval pattern (between hiss periods):")
                for i, iv in enumerate(intervals):
                    print(f"  Interval {i+1}: {iv:.1f}s")
                avg_interval = sum(intervals) / len(intervals)
                print(f"  Average interval: {avg_interval:.1f}s")
    
    # Summary
    print("\n=== Summary ===")
    if deltas:
        if abs(avg_jitter) > 10000:  # 10µs average offset
            print("WARNING: Large average timestamp offset - clock is drifting")
        if max_jitter - min_jitter > 100000:  # 100µs peak-to-peak
            print("WARNING: Large timestamp variation - periodic clock issue")
    else:
        print("Could not analyze timestamps (no valid SYT values)")

def main():
    if len(sys.argv) < 2:
        print("Usage: analyze_avtp_timestamps.py <pcap_file>")
        sys.exit(1)
    
    pcap_file = sys.argv[1]
    
    print(f"Analyzing {pcap_file}...")
    
    # Parse packets
    packets = parse_pcap_raw(pcap_file)
    print(f"Parsed {len(packets)} packets from PCAP")
    
    # Analyze
    analyze_timestamps(packets)

if __name__ == '__main__':
    main()