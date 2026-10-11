#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#
# apply-network-config.sh
# Apply network configuration for AVB traffic separation
# 
# This script:
# 1. Detects all available Ethernet interfaces
# 2. Identifies AVB-capable NICs (Intel I210/I211)
# 3. Interactively asks user to select interfaces
# 4. Detects current network configuration
# 5. Creates backups of existing configuration files
# 6. Generates new configuration files based on user input
# 7. Restarts systemd-networkd to apply changes
#
# Usage: sudo ./apply-network-config.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKUP_DIR="/etc/systemd/network/backups"
TARGET_DIR="/etc/systemd/network"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo "=========================================="
echo "AVB Network Configuration Helper"
echo "=========================================="
echo ""

# Check if running as root
if [[ $EUID -ne 0 ]]; then
    echo -e "${RED}Error: This script must be run as root (use sudo)${NC}"
    exit 1
fi

# Create backup directory if it doesn't exist
mkdir -p "$BACKUP_DIR"

# ============================================
# NIC Detection Functions
# ============================================

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

# Get PCI device info (vendor and device)
get_pci_device_info() {
    local pci_id="$1"
    if [[ "$pci_id" != "N/A" ]]; then
        lspci -nn | grep -i "$pci_id" | awk -F'\\[|\\]' '{for(i=1;i<=NF;i++) if($i ~ /^[0-9a-f]{4}$/) vendor=$i; if($i ~ /^[0-9a-f]{4}$/ && vendor) {device=$i; break}} END{print vendor ":" device}'
        # Alternative simpler approach
        lspci -nn | grep "$pci_id" | sed 's/.*\[\([0-9a-f:]*\)\].*/\1/' | head -1
    else
        echo "N/A"
    fi
}

# Check if NIC is AVB-capable (Intel I210/I211)
is_avb_capable() {
    local pci_id="$1"
    # I210 = 8086:1533, I211 = 8086:1539
    if [[ "$pci_id" =~ "8086:1533" ]] || [[ "$pci_id" =~ "8086:1539" ]]; then
        return 0
    fi
    return 1
}

# Get interface driver
get_driver() {
    local iface="$1"
    local path="/sys/class/net/$iface/device/driver"
    if [[ -L "$path" ]]; then
        basename "$(readlink -f "$path")"
    else
        echo "N/A"
    fi
}

# Get current IP address
get_current_ip() {
    local iface="$1"
    ip -4 addr show "$iface" 2>/dev/null | grep -oP 'inet \K[0-9./]+' | head -1
}

# Get current gateway
get_current_gateway() {
    ip route show 2>/dev/null | grep "default via" | awk '{print $3}' | head -1
}

# Get current DNS
get_current_dns() {
    cat /etc/resolv.conf 2>/dev/null | grep "^nameserver" | awk '{print $2}' | head -1
}

# Get network subnet from IP
get_subnet() {
    local ip="$1"
    if [[ -n "$ip" ]]; then
        echo "$ip" | awk -F. '{print $1"."$2"."$3".0/24"}'
    else
        echo "N/A"
    fi
}

# ============================================
# Interface Selection
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
    driver=$(get_driver "$iface")
    current_ip=$(get_current_ip "$iface")
    
    # Check if AVB-capable
    avb_status="No"
    if is_avb_capable "$device_id"; then
        avb_status="Yes (AVB)"
    fi
    
    INTERFACES["${idx}_name"]="$iface"
    INTERFACES["${idx}_mac"]="$mac"
    INTERFACES["${idx}_pci"]="$pci_id"
    INTERFACES["${idx}_driver"]="$driver"
    INTERFACES["${idx}_ip"]="$current_ip"
    INTERFACES["${idx}_avb"]="$avb_status"
    
    INTERFACE_NAMES+=("$iface")
    
    echo -e "${BLUE}[$idx] $iface${NC}"
    echo "    MAC: $mac"
    echo "    PCI: $pci_id"
    echo "    Driver: $driver"
    echo "    AVB-capable: $avb_status"
    echo "    Current IP: ${current_ip:-'Not configured'}"
    echo ""
    
    ((idx++))
done < <(get_ethernet_interfaces)

num_interfaces=${#INTERFACE_NAMES[@]}

if [[ $num_interfaces -lt 2 ]]; then
    echo -e "${RED}Error: At least 2 Ethernet interfaces are required${NC}"
    echo "Found only $num_interfaces interface(s)"
    exit 1
fi

echo -e "${BLUE}=== Interface Selection ===${NC}"
echo ""

# Select AVB interface
echo "Select the interface for AVB-only traffic (gPTP, SRP, 1722 streams):"
for i in "${!INTERFACE_NAMES[@]}"; do
    key=$((i + 1))
    echo -e "  [$key] ${INTERFACE_NAMES[$i]} (${INTERFACES["${key}_avb"]})"
done
echo -n "AVB interface selection (1-$num_interfaces): "
read -r avb_selection

# Validate AVB selection
if ! [[ "$avb_selection" =~ ^[0-9]+$ ]] || [[ "$avb_selection" -lt 1 ]] || [[ "$avb_selection" -gt $num_interfaces ]]; then
    echo -e "${RED}Invalid selection. Exiting.${NC}"
    exit 1
fi

AVB_IFACE="${INTERFACES["${avb_selection}_name"]}"
echo -e "${GREEN}Selected AVB interface: $AVB_IFACE${NC}"
echo ""

# Select NON-AVB interface
echo "Select the interface for NON-AVB traffic (internet, LAN, Web Controller):"
for i in "${!INTERFACE_NAMES[@]}"; do
    if [[ "${INTERFACE_NAMES[$i]}" != "$AVB_IFACE" ]]; then
        key=$((i + 1))
        echo -e "  [$key] ${INTERFACE_NAMES[$i]}"
    fi
done
echo -n "NON-AVB interface selection: "
read -r nonavb_selection

# Validate NON-AVB selection
if ! [[ "$nonavb_selection" =~ ^[0-9]+$ ]] || [[ "$nonavb_selection" -lt 1 ]] || [[ "$nonavb_selection" -gt $num_interfaces ]]; then
    echo -e "${RED}Invalid selection. Exiting.${NC}"
    exit 1
fi

NONAVB_IFACE="${INTERFACES["${nonavb_selection}_name"]}"

# Ensure we didn't select the same interface
if [[ "$NONAVB_IFACE" == "$AVB_IFACE" ]]; then
    echo -e "${RED}Error: Cannot select the same interface for both roles${NC}"
    exit 1
fi

echo -e "${GREEN}Selected NON-AVB interface: $NONAVB_IFACE${NC}"
echo ""

# ============================================
# Network Configuration Detection
# ============================================

echo -e "${BLUE}=== Network Configuration Detection ===${NC}"
echo ""

current_gateway=$(get_current_gateway)
current_dns=$(get_current_dns)

# Get IP of NON-AVB interface for subnet detection
nonavb_ip=$(get_current_ip "$NONAVB_IFACE")
if [[ -z "$nonavb_ip" ]]; then
    echo -e "${YELLOW}Warning: $NONAVB_IFACE has no IP address configured${NC}"
    echo -n "Enter IP address for $NONAVB_IFACE (e.g., 192.168.1.100/24): "
    read -r nonavb_ip_input
    if [[ -z "$nonavb_ip_input" ]]; then
        echo -e "${RED}No IP address provided. Exiting.${NC}"
        exit 1
    fi
    nonavb_ip="$nonavb_ip_input"
fi

# Detect subnet
subnet=$(get_subnet "$nonavb_ip")
echo "Detected subnet: $subnet"

# Get gateway
if [[ -n "$current_gateway" ]]; then
    echo "Detected gateway: $current_gateway"
    gateway_input="$current_gateway"
else
    echo -n "Enter gateway IP (e.g., 192.168.1.1): "
    read -r gateway_input
    if [[ -z "$gateway_input" ]]; then
        echo -e "${RED}No gateway provided. Exiting.${NC}"
        exit 1
    fi
fi

# Get DNS
if [[ -n "$current_dns" ]]; then
    echo "Detected DNS: $current_dns"
    dns_input="$current_dns"
else
    echo -n "Enter DNS server IP (e.g., 192.168.1.1 or 8.8.8.8): "
    read -r dns_input
    if [[ -z "$dns_input" ]]; then
        echo -e "${RED}No DNS provided. Exiting.${NC}"
        exit 1
    fi
fi

echo ""

# ============================================
# IP Address Assignment
# ============================================

echo -e "${BLUE}=== IP Address Assignment ===${NC}"
echo ""

# Generate IP for AVB interface (use .44 as default)
avb_ip_base=$(echo "$nonavb_ip" | awk -F. '{print $1"."$2"."$3}')
avb_ip_default="${avb_ip_base}.44"

echo "AVB interface ($AVB_IFACE):"
echo -n "  IP address (default: $avb_ip_default/24): "
read -r avb_ip_input

if [[ -z "$avb_ip_input" ]]; then
    AVB_IP="$avb_ip_default/24"
else
    # Add /24 if not specified
    if [[ ! "$avb_ip_input" =~ / ]]; then
        AVB_IP="$avb_ip_input/24"
    else
        AVB_IP="$avb_ip_input"
    fi
fi

echo "  Using: $AVB_IP"
echo ""

# Generate IP for NON-AVB interface (use .56 as default)
nonavb_ip_base=$(echo "$nonavb_ip" | awk -F. '{print $1"."$2"."$3}')
nonavb_ip_default="${nonavb_ip_base}.56"

echo "NON-AVB interface ($NONAVB_IFACE):"
echo -n "  IP address (default: $nonavb_ip_default/24): "
read -r nonavb_ip_input2

if [[ -z "$nonavb_ip_input2" ]]; then
    NONAVB_IP="$nonavb_ip_default/24"
else
    # Add /24 if not specified
    if [[ ! "$nonavb_ip_input2" =~ / ]]; then
        NONAVB_IP="$nonavb_ip_input2/24"
    else
        NONAVB_IP="$nonavb_ip_input2"
    fi
fi

echo "  Using: $NONAVB_IP"
echo ""

# ============================================
# Configuration Generation
# ============================================

echo -e "${BLUE}=== Generating Configuration Files ===${NC}"
echo ""

# Create AVB interface configuration
cat > "$TARGET_DIR/10-static-${AVB_IFACE}.network" << EOF
[Match]
Name=$AVB_IFACE

[Network]
# ============================================
# AVB-ONLY INTERFACE
# ============================================
# This interface is for AVB traffic only (gPTP, SRP, 1722 streams)
# NO gateway or DNS configured - prevents routing through this interface
#
# IP: $AVB_IP
Address=$AVB_IP

IPv6AcceptRA=no
LinkLocalAddressing=no
LLDP=no
EmitLLDP=no

# AVB traffic only - NO gateway, NO DNS

# Disable DHCP
[DHCPv4]
dhcp=none

[DHCPv6]
dhcp=none
EOF

# Create NON-AVB interface configuration
cat > "$TARGET_DIR/20-static-${NONAVB_IFACE}.network" << EOF
[Match]
Name=$NONAVB_IFACE

[Network]
# ============================================
# NON-AVB INTERFACE
# ============================================
# This interface handles all non-AVB traffic:
# - Internet access
# - LAN communication
# - Web Controller access
# - Firmware updates
#
# IP: $NONAVB_IP
# Gateway: $gateway_input
# DNS: $dns_input
Address=$NONAVB_IP
Gateway=$gateway_input
DNS=$dns_input

IPv6AcceptRA=yes
LinkLocalAddressing=no
LLDP=no
EmitLLDP=no

# Disable DHCP (using static IP)
[DHCPv4]
dhcp=no

[DHCPv6]
dhcp=no
EOF

echo "Created: $TARGET_DIR/10-static-${AVB_IFACE}.network"
echo "Created: $TARGET_DIR/20-static-${NONAVB_IFACE}.network"
echo ""

# ============================================
# Backup and Apply
# ============================================

echo -e "${BLUE}=== Creating Backups ===${NC}"
echo ""

# Backup existing files if they exist
for file in "$TARGET_DIR"/*.network; do
    if [[ -f "$file" ]]; then
        basename_file=$(basename "$file")
        timestamp=$(date +%Y%m%d_%H%M%S)
        cp "$file" "$BACKUP_DIR/${basename_file}.${timestamp}.backup"
        echo "  Backed up: $file -> $BACKUP_DIR/${basename_file}.${timestamp}.backup"
    fi
done
echo ""

# ============================================
# Apply Configuration
# ============================================

echo -e "${BLUE}=== Applying Configuration ===${NC}"
echo ""

# Restart systemd-networkd
echo "Restarting systemd-networkd..."
systemctl restart systemd-networkd
echo "  systemd-networkd restarted"
echo ""

# ============================================
# Verification
# ============================================

echo -e "${BLUE}=== Verification ===${NC}"
echo ""

echo "Current network configuration:"
echo "----------------------------------------"
echo "$NONAVB_IFACE interface:"
ip -4 addr show "$NONAVB_IFACE" 2>/dev/null | grep -E "inet " || echo "  No IPv4 address configured"
echo ""
echo "$AVB_IFACE interface:"
ip -4 addr show "$AVB_IFACE" 2>/dev/null | grep -E "inet " || echo "  No IPv4 address configured"
echo ""

echo "Routing table:"
ip route show 2>/dev/null | grep -E "default via" || echo "  No default routes"
echo ""

echo "=========================================="
echo -e "${GREEN}Configuration applied successfully!${NC}"
echo "=========================================="
echo ""
echo "Summary:"
echo "  - $NONAVB_IFACE: NON-AVB traffic (internet, LAN, Web Controller)"
echo "  - $AVB_IFACE: AVB-only traffic (gPTP, SRP, 1722 streams)"
echo "  - Subnet: $subnet"
echo "  - Gateway: $gateway_input (only on $NONAVB_IFACE)"
echo "  - DNS: $dns_input"
echo ""
echo "To verify the configuration, run: ./test-network-config.sh"