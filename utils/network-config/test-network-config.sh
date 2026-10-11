#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#
# test-network-config.sh
# Test script to verify AVB network configuration
#
# This script verifies:
# 1. Both interfaces are UP
# 2. Both interfaces are in the same subnet
# 3. Only the NON-AVB interface has gateway (no routing through AVB interface)
# 4. AVB interface has no DNS configured
# 5. Connectivity tests
#
# Usage: ./test-network-config.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo "=========================================="
echo "AVB Network Configuration Test"
echo "=========================================="
echo ""

# Function to print test result
print_result() {
    local test_name="$1"
    local result="$2"
    
    if [[ "$result" == "PASS" ]]; then
        echo -e "${GREEN}[PASS]${NC} $test_name"
    elif [[ "$result" == "WARN" ]]; then
        echo -e "${YELLOW}[WARN]${NC} $test_name"
    else
        echo -e "${RED}[FAIL]${NC} $test_name"
    fi
}

# Get all Ethernet interfaces (excluding loopback)
get_ethernet_interfaces() {
    ip -o link show | grep -E "^[0-9]+: (en|eth)" | awk -F: '{print $2}' | sed 's/^[ \t]*//'
}

# Get interface MAC address
get_mac_address() {
    local iface="$1"
    ip link show "$iface" 2>/dev/null | grep -oP 'link/ether \K[0-9a-f:]+' | head -1
}

# Get interface PCI ID (full path format)
get_pci_id() {
    local iface="$1"
    local path="/sys/class/net/$iface/device"
    if [[ -d "$path" ]]; then
        local id=$(basename "$(readlink -f "$path")" 2>/dev/null || echo "")
        if [[ "$id" =~ ^[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]$ ]]; then
            echo "$id"
        else
            echo "N/A"
        fi
    else
        echo "N/A"
    fi
}

# Get device ID from lspci (e.g., 8086:1533)
get_device_id() {
    local iface="$1"
    local pci_id=$(get_pci_id "$iface")
    if [[ "$pci_id" != "N/A" ]]; then
        # Try to match the PCI ID in lspci output
        # lspci shows shorter format (e.g., 05:00.0) while sysfs shows full (0000:05:00.0)
        local short_pci_id=$(echo "$pci_id" | sed 's/^0000://')
        local result=$(lspci -nn | grep -E "$pci_id|$short_pci_id" | grep -oP '\[[0-9a-f:]+\]' | tail -1)
        if [[ -n "$result" ]]; then
            echo "$result" | tr -d '[]'
        else
            echo "N/A"
        fi
    else
        echo "N/A"
    fi
}

# Check if NIC is AVB-capable (Intel I210/I211)
is_avb_capable() {
    local pci_id="$1"
    # PCI ID format can be 0000:05:00.0 or 05:00.0
    # I210 = 8086:1533, I211 = 8086:1539
    if [[ "$pci_id" == *"8086:1533"* ]] || [[ "$pci_id" == *"8086:1539"* ]]; then
        return 0
    fi
    return 1
}

# Get current IP address (without subnet mask)
get_current_ip() {
    local iface="$1"
    ip -4 addr show "$iface" 2>/dev/null | grep -oP 'inet \K[0-9.]+' | head -1
}

# Get current gateway
get_current_gateway() {
    ip route show 2>/dev/null | grep "default via" | awk '{print $3}' | head -1
}

# ============================================
# Interface Detection
# ============================================

echo -e "${BLUE}=== Detecting Network Interfaces ===${NC}"
echo ""

declare -A INTERFACES
declare -a INTERFACE_NAMES

idx=1
while IFS= read -r iface; do
    [[ -z "$iface" ]] && continue
    
    mac=$(get_mac_address "$iface")
    pci_id=$(get_pci_id "$iface")
    device_id=$(get_device_id "$iface")
    
    # Check if AVB-capable
    avb_status="No"
    if is_avb_capable "$device_id"; then
        avb_status="Yes (AVB)"
    fi
    
    INTERFACES["${idx}_name"]="$iface"
    INTERFACES["${idx}_mac"]="$mac"
    INTERFACES["${idx}_pci"]="$pci_id"
    INTERFACES["${idx}_avb"]="$avb_status"
    
    INTERFACE_NAMES+=("$iface")
    
    echo -e "${BLUE}[$idx] $iface${NC}"
    echo "    MAC: $mac"
    echo "    PCI: $pci_id"
    echo "    AVB-capable: $avb_status"
    echo ""
    
    ((idx++))
done < <(get_ethernet_interfaces)

num_interfaces=${#INTERFACE_NAMES[@]}

if [[ $num_interfaces -lt 2 ]]; then
    echo -e "${RED}Error: At least 2 Ethernet interfaces are required${NC}"
    echo "Found only $num_interfaces interface(s)"
    exit 1
fi

# Identify AVB and NON-AVB interfaces
AVB_IFACE=""
NONAVB_IFACE=""

for i in "${!INTERFACE_NAMES[@]}"; do
    iface="${INTERFACE_NAMES[$i]}"
    key=$((i + 1))
    if [[ "${INTERFACES["${key}_avb"]}" == "Yes (AVB)" ]]; then
        AVB_IFACE="$iface"
    else
        NONAVB_IFACE="$iface"
    fi
done

# If we couldn't identify by AVB capability, use first as AVB, second as NON-AVB
if [[ -z "$AVB_IFACE" ]]; then
    AVB_IFACE="${INTERFACE_NAMES[0]}"
fi
if [[ -z "$NONAVB_IFACE" ]]; then
    NONAVB_IFACE="${INTERFACE_NAMES[1]}"
fi

echo -e "${BLUE}Detected Interfaces:${NC}"
echo "  AVB interface: $AVB_IFACE"
echo "  NON-AVB interface: $NONAVB_IFACE"
echo ""

# ============================================
# Interface Status Tests
# ============================================

echo "=== Interface Status ==="

echo "Checking $NONAVB_IFACE (NON-AVB traffic interface)..."
if ip link show "$NONAVB_IFACE" 2>/dev/null | grep -q "state UP"; then
    print_result "$NONAVB_IFACE is UP" "PASS"
else
    print_result "$NONAVB_IFACE is UP" "FAIL"
fi

echo ""
echo "Checking $AVB_IFACE (AVB-only traffic interface)..."
if ip link show "$AVB_IFACE" 2>/dev/null | grep -q "state UP"; then
    print_result "$AVB_IFACE is UP" "PASS"
else
    print_result "$AVB_IFACE is UP" "FAIL"
fi

# ============================================
# IP Address Configuration Tests
# ============================================

echo ""
echo "=== IP Address Configuration ==="

# Get IP addresses
NONAVB_IP=$(get_current_ip "$NONAVB_IFACE")
AVB_IP=$(get_current_ip "$AVB_IFACE")

echo "$NONAVB_IFACE IP: ${NONAVB_IP:-'Not configured'}"
echo "$AVB_IFACE IP: ${AVB_IP:-'Not configured'}"

# Check both are in same subnet
if [[ -n "$NONAVB_IP" && -n "$AVB_IP" ]]; then
    NONAVB_SUBNET=$(echo "$NONAVB_IP" | cut -d. -f1-3)
    AVB_SUBNET=$(echo "$AVB_IP" | cut -d. -f1-3)
    
    if [[ "$NONAVB_SUBNET" == "$AVB_SUBNET" ]]; then
        print_result "Both interfaces in same subnet ($NONAVB_SUBNET.x)" "PASS"
    else
        print_result "Both interfaces in same subnet" "FAIL"
    fi
else
    print_result "Both interfaces have IP addresses" "WARN"
fi

# ============================================
# Gateway Configuration Tests
# ============================================

echo ""
echo "=== Gateway Configuration ==="

# Check which interface has gateway
DEFAULT_ROUTE_IFACE=$(ip route show 2>/dev/null | grep "default via" | awk '{print $5}' | head -1)
echo "Default route interface: ${DEFAULT_ROUTE_IFACE:-'None'}"

# NON-AVB interface should have gateway
if [[ "$DEFAULT_ROUTE_IFACE" == "$NONAVB_IFACE" ]]; then
    print_result "Gateway configured on $NONAVB_IFACE (NON-AVB)" "PASS"
else
    print_result "Gateway configured on $NONAVB_IFACE (NON-AVB)" "FAIL"
fi

# AVB interface should NOT have gateway
AVB_GATEWAY=$(ip route show dev "$AVB_IFACE" 2>/dev/null | grep "default" | wc -l)
if [[ "$AVB_GATEWAY" -eq 0 ]]; then
    print_result "No gateway on $AVB_IFACE (AVB-only)" "PASS"
else
    print_result "No gateway on $AVB_IFACE (AVB-only)" "FAIL"
fi

# ============================================
# DNS Configuration Tests
# ============================================

echo ""
echo "=== DNS Configuration ==="

if command -v networkctl &> /dev/null; then
    AVB_DNS=$(networkctl status "$AVB_IFACE" 2>/dev/null | grep -i "dns" | wc -l)
    if [[ "$AVB_DNS" -eq 0 ]]; then
        print_result "No DNS configured on $AVB_IFACE (AVB-only)" "PASS"
    else
        print_result "No DNS configured on $AVB_IFACE (AVB-only)" "WARN"
    fi
fi

# ============================================
# Connectivity Tests
# ============================================

echo ""
echo "=== Connectivity Tests ==="

# Test 1: Can we reach the gateway?
GATEWAY_IP=$(get_current_gateway)
if [[ -n "$GATEWAY_IP" ]]; then
    echo "Testing connectivity to gateway ($GATEWAY_IP)..."
    if ping -c 1 -W 2 "$GATEWAY_IP" &> /dev/null; then
        print_result "Gateway is reachable" "PASS"
    else
        print_result "Gateway is reachable" "WARN"
    fi
else
    print_result "Gateway is reachable" "WARN"
fi

# Test 2: Can we reach AVB interface from itself?
if [[ -n "$AVB_IP" ]]; then
    echo "Testing localhost connectivity on $AVB_IFACE..."
    if ping -c 1 -W 1 "$AVB_IP" &> /dev/null; then
        print_result "$AVB_IFACE localhost connectivity" "PASS"
    else
        print_result "$AVB_IFACE localhost connectivity" "WARN"
    fi
fi

# Test 3: Can we reach NON-AVB interface from itself?
if [[ -n "$NONAVB_IP" ]]; then
    echo "Testing localhost connectivity on $NONAVB_IFACE..."
    if ping -c 1 -W 1 "$NONAVB_IP" &> /dev/null; then
        print_result "$NONAVB_IFACE localhost connectivity" "PASS"
    else
        print_result "$NONAVB_IFACE localhost connectivity" "WARN"
    fi
fi

# ============================================
# AVB Traffic Isolation Tests
# ============================================

echo ""
echo "=== AVB Traffic Isolation ==="

# Check that AVB interface has no default route
AVB_ROUTES=$(ip route show dev "$AVB_IFACE" 2>/dev/null | grep -c "default" || true)
if [[ "$AVB_ROUTES" -eq 0 ]]; then
    print_result "No default route on $AVB_IFACE (AVB-only)" "PASS"
else
    print_result "No default route on $AVB_IFACE (AVB-only)" "FAIL"
fi

# Check that NON-AVB interface has default route
NONAVB_ROUTES=$(ip route show dev "$NONAVB_IFACE" 2>/dev/null | grep -c "default" || true)
if [[ "$NONAVB_ROUTES" -gt 0 ]]; then
    print_result "Default route on $NONAVB_IFACE (NON-AVB)" "PASS"
else
    print_result "Default route on $NONAVB_IFACE (NON-AVB)" "FAIL"
fi

# ============================================
# systemd-networkd Status
# ============================================

echo ""
echo "=== systemd-networkd Status ==="

if command -v networkctl &> /dev/null; then
    echo "$NONAVB_IFACE status:"
    networkctl status "$NONAVB_IFACE" 2>/dev/null | grep -E "State|Address|Gateway|DNS" | head -10
    echo ""
    echo "$AVB_IFACE status:"
    networkctl status "$AVB_IFACE" 2>/dev/null | grep -E "State|Address|Gateway|DNS" | head -10
fi

# ============================================
# Summary
# ============================================

echo ""
echo "=========================================="
echo "Test Summary"
echo "=========================================="
echo ""
echo "Configuration Summary:"
echo "  - $NONAVB_IFACE: NON-AVB traffic (internet, LAN, Web Controller)"
echo "  - $AVB_IFACE: AVB-only traffic (gPTP, SRP, 1722 streams)"
echo "  - Gateway only on $NONAVB_IFACE (no routing through $AVB_IFACE)"
echo ""
echo "To apply new configuration, run: sudo ./apply-network-config.sh"
echo ""