#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# check-prerequisites.sh - Enhanced AVB4Linux system check with scenarios
# Exit codes: 0=ready, 1=critical, 2=optional, 3=both

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
AVBHOME="${AVBHOME:-$HOME/.avb}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'

SCENARIO="development"
CRITICAL_FAILED=0; OPTIONAL_FAILED=0; PASSED=0; WARNINGS=0

show_help() {
    echo "Usage: $0 [--scenario=TYPE] [--output=FORMAT]"
    echo "Scenarios: development, testing, production"
    echo "Exit codes: 0=ready, 1=critical, 2=optional"
}

for arg in "$@"; do
    case $arg in
        --scenario=*) SCENARIO="${arg#*=}"; [[ ! "$SCENARIO" =~ ^(development|testing|production)$ ]] && { echo "Invalid: $SCENARIO" >&2; exit 1; }; shift ;;
        --output=*) shift ;;
        --help|-h) show_help; exit 0 ;;
        *) echo "Unknown: $arg" >&2; exit 1 ;;
    esac
done

if [[ ! "${1:-}" =~ ^--scenario= ]]; then
    echo "============================================"
    echo "AVB4Linux System Prerequisites Check"
    echo "============================================"
    echo ""
    echo "Select scenario:"
    echo "  1) development - Building and modifying code"
    echo "  2) testing     - Running audio tests"
    echo "  3) production  - Music production"
    echo ""
    while true; do
        read -p "Enter choice (1-3) or name: " choice
        case "$choice" in
            1|development) SCENARIO="development"; break ;;
            2|testing) SCENARIO="testing"; break ;;
            3|production) SCENARIO="production"; break ;;
            *) echo "Invalid. Enter 1, 2, 3, or scenario name." ;;
        esac
    done
    echo ""
fi

echo "Scenario: $SCENARIO"
echo "Checking: $(hostname) - $(uname -r)"
echo "Directory: $ROOT_DIR"
echo "AVBHOME: $AVBHOME"
echo ""

output() {
    local status="$1" category="$2" message="$3" fix="${4:-}"
    case $status in
        PASS) echo -e "${GREEN}[PASS]${NC}  $message"; ((PASSED++)) ;;
        WARN) echo -e "${YELLOW}[WARN]${NC}  $message"; [[ -n "$fix" ]] && echo -e "        ${YELLOW}Fix:${NC} $fix"; ((WARNINGS++)) ;;
        FAIL)
            if [[ "$category" == "CRITICAL" ]]; then
                echo -e "${RED}[FAIL]${NC}  $message"
                [[ -n "$fix" ]] && echo -e "        ${RED}Fix:${NC} $fix"
                ((CRITICAL_FAILED++))
            else
                echo -e "${YELLOW}[WARN]${NC}  $message"
                [[ -n "$fix" ]] && echo -e "        ${YELLOW}Fix:${NC} $fix"
                ((OPTIONAL_FAILED++))
            fi ;;
    esac
}

check_hardware() {
    echo "=== Hardware ==="
    local nic_found=false nic_info=""
    if lspci_output=$(lspci -nn 2>/dev/null) && echo "$lspci_output" | grep -qE "8086:1533|8086:1539"; then
        nic_found=true
        nic_info=$(lspci -nn | grep -E "8086:1533|8086:1539" | head -1)
    fi
    
    if [[ "$nic_found" == true ]]; then
        output "PASS" "CRITICAL" "AVB-capable NIC found" "$nic_info"
    else
        output "FAIL" "CRITICAL" "Intel i210 or i211 NIC not found" \
            "Install Intel i210 (8086:1533) or i211 (8086:1539) NIC."
        return 1
    fi
    
    local arch=$(uname -m)
    if [[ "$arch" == "x86_64" ]]; then
        output "PASS" "CRITICAL" "Compatible CPU architecture" "$arch"
    else
        output "FAIL" "CRITICAL" "Unsupported CPU architecture" "$arch (x86_64 required)"
        return 1
    fi
    return 0
}

check_kernel() {
    echo ""
    echo "=== Kernel ==="
    output "PASS" "CRITICAL" "Kernel available" "$(uname -r)"
    
    if grep -q "threadirqs" /proc/cmdline 2>/dev/null; then
        output "PASS" "CRITICAL" "threadirqs boot parameter present"
    else
        output "FAIL" "CRITICAL" "threadirqs boot parameter missing" \
            "Add 'threadirqs' to kernel command line.\n"\
            "  GRUB: Edit /etc/default/grub -> GRUB_CMDLINE_LINUX, then sudo update-grub"
        return 1
    fi
    
    if ls /sys/class/ptp/ >/dev/null 2>&1; then
        output "PASS" "CRITICAL" "PTP 1588 clock support" "$(ls /sys/class/ptp/ | wc -l) device(s)"
    else
        output "FAIL" "CRITICAL" "PTP 1588 clock support not found" \
            "Enable CONFIG_PTP_1588_CLOCK in kernel."
        return 1
    fi
    
    if lsmod_output=$(lsmod 2>/dev/null) && echo "$lsmod_output" | grep -q "i2c_algo_bit"; then
        output "PASS" "CRITICAL" "i2c-algo-bit module available"
    else
        output "WARN" "CRITICAL" "i2c-algo-bit not loaded" \
            "Load with: sudo modprobe i2c_algo_bit"
        ((WARNINGS++))
    fi
    return 0
}

check_build_deps() {
    echo ""
    echo "=== Build Dependencies ==="
    local missing=0
    for cmd in gcc g++ make cmake patch; do
        if command -v "$cmd" >/dev/null 2>&1; then
            output "PASS" "CRITICAL" "$cmd available" "$($cmd --version 2>&1 | head -1)"
        else
            if [[ "$SCENARIO" == "testing" ]]; then
                output "WARN" "OPTIONAL" "$cmd not found" \
                    "Not needed if binaries exist. Install: sudo pacman -S $cmd"
                ((WARNINGS++))
            else
                output "FAIL" "CRITICAL" "$cmd not found" \
                    "Install: sudo pacman -S $cmd  # Arch\n"\
                    "       sudo apt install $cmd   # Debian/Ubuntu"
                missing=1
            fi
        fi
    done
    
    if [[ -d "/lib/modules/$(uname -r)/build" ]]; then
        output "PASS" "CRITICAL" "Kernel headers available"
    else
        output "FAIL" "CRITICAL" "Kernel headers not found" \
            "Install: sudo pacman -S linux-headers  # Arch"
        missing=1
    fi
    return $missing
}



check_runtime_deps() {
    echo ""
    echo "=== Runtime Dependencies ==="
    local missing=0
    
    # ethtool - Used by avb_up.sh to display driver info and in tests
    if command -v ethtool >/dev/null 2>&1; then
        output "PASS" "CRITICAL" "ethtool available"
    else
        output "FAIL" "CRITICAL" "ethtool not found" \
            "Install: sudo pacman -S ethtool  # Arch\n"\
            "       sudo apt install ethtool   # Debian/Ubuntu\n"\
            "Needed by: avb_up.sh (driver info), tests"
        missing=1
    fi
    
    # ifconfig (net-tools) - Used by avb_up.sh and avb_down.sh
    if command -v ifconfig >/dev/null 2>&1; then
        output "PASS" "CRITICAL" "ifconfig available (net-tools)"
    else
        output "FAIL" "CRITICAL" "ifconfig not found" \
            "Install: sudo pacman -S net-tools  # Arch\n"\
            "       sudo apt install net-tools   # Debian/Ubuntu\n"\
            "Needed by: avb_up.sh/avb_down.sh (interface config)"
        missing=1
    fi
    
    # killall (psmisc) - Used to stop daemons
    if command -v killall >/dev/null 2>&1; then
        output "PASS" "CRITICAL" "killall available (psmisc)"
    else
        output "FAIL" "CRITICAL" "killall not found" \
            "Install: sudo pacman -S psmisc  # Arch\n"\
            "       sudo apt install psmisc   # Debian/Ubuntu\n"\
            "Needed by: avb_down.sh (stop daemons)"
        missing=1
    fi
    
    for cmd in chrt renice logger; do
        if command -v "$cmd" >/dev/null 2>&1; then
            output "PASS" "CRITICAL" "$cmd available"
        else
            output "FAIL" "CRITICAL" "$cmd not found" \
                "Install: sudo pacman -S util-linux  # Arch\n"\
                "       sudo apt install util-linux   # Debian/Ubuntu\n"\
                "Needed by: avb_up.sh (RT priorities/logging)"
            missing=1
        fi
    done
    
    # Optional tools
    for cmd in tshark curl jq; do
        if command -v "$cmd" >/dev/null 2>&1; then
            output "PASS" "OPTIONAL" "$cmd available"
            ((PASSED++))
        else
            output "WARN" "OPTIONAL" "$cmd not found"
            ((WARNINGS++))
        fi
    done
    return $missing
}


check_submodules() {
    echo ""
    echo "=== Git Submodules ==="
    
    [[ ! -d "$ROOT_DIR/.git" ]] && { output "FAIL" "CRITICAL" "Not a git repository"; return 1; }
    [[ ! -f "$ROOT_DIR/.gitmodules" ]] && { output "FAIL" "CRITICAL" ".gitmodules not found"; return 1; }
    
    # Scenario-specific submodule requirements
    # All submodules defined in .gitmodules
    local all_submodules
    mapfile -t all_submodules < <(git config -f "$ROOT_DIR/.gitmodules" --get-regexp '^submodule\..*\.path$' 2>/dev/null | cut -d' ' -f2)
    local total_count=${#all_submodules[@]}
    
    # Required submodules by scenario
    # Core submodules needed for main build (kernel module, daemons, gptp, jdksavdecc-c)
    local core_submodules=(
        "jdksavdecc-c"
        "kernel-module/igb_avb"
        "daemons/gptp"
        "daemons/OpenAvnu"
    )
    
    # Utils submodules (only needed for development/MOTU tcpdump helper)
    local utils_submodules=(
        "utils/tcpdump"
        "utils/libpcap"
    )
    
    # Determine required submodules based on scenario
    local required_submodules=()
    if [[ "$SCENARIO" == "development" ]]; then
        required_submodules=(${core_submodules[@]} ${utils_submodules[@]})
        output "PASS" "CRITICAL" "Scenario: development" "All submodules required"
    else
        required_submodules=(${core_submodules[@]})
        output "PASS" "CRITICAL" "Scenario: $SCENARIO" "Core submodules required (utils submodules optional)"
    fi
    
    output "PASS" "CRITICAL" "Submodule configuration found" "$total_count submodule(s) in .gitmodules"
    
    local missing=0
    for path in "${required_submodules[@]}"; do
        if git -C "$ROOT_DIR" ls-files --error-unmatch "$path" >/dev/null 2>&1; then
            if [[ -d "$ROOT_DIR/$path" ]] && [[ -n "$(ls -A "$ROOT_DIR/$path" 2>/dev/null)" ]]; then
                output "PASS" "CRITICAL" "Submodule initialized" "$path"
            else
                # Check if it's a utils submodule (optional for non-development)
                local is_utils=0
                for u in "${utils_submodules[@]}"; do
                    [[ "$u" == "$path" ]] && is_utils=1 && break
                done
                
                if [[ "$is_utils" -eq 1 && "$SCENARIO" != "development" ]]; then
                    output "WARN" "OPTIONAL" "Utils submodule not initialized (optional for $SCENARIO)" "$path"
                    ((WARNINGS++))
                else
                    output "WARN" "CRITICAL" "Submodule exists but empty" "$path"
                    missing=1
                fi
            fi
        else
            output "FAIL" "CRITICAL" "Submodule not registered" "$path"
            missing=1
        fi
    done
    
    # Check for optional utils submodules in non-development scenarios
    if [[ "$SCENARIO" != "development" ]]; then
        for path in "${utils_submodules[@]}"; do
            if git -C "$ROOT_DIR" ls-files --error-unmatch "$path" >/dev/null 2>&1; then
                if [[ ! -d "$ROOT_DIR/$path" ]] || [[ -z "$(ls -A "$ROOT_DIR/$path" 2>/dev/null)" ]]; then
                    output "WARN" "OPTIONAL" "Utils submodule not initialized (optional for $SCENARIO)" "$path"
                    ((WARNINGS++))
                fi
            fi
        done
    fi
    
    return $missing
}

check_memory() {
    echo ""
    echo "=== System Resources ==="
    local mem_gb=$(awk '/MemTotal/ {printf "%.0f", $2/1024/1024}' /proc/meminfo)
    
    if [[ $mem_gb -ge 4 ]]; then
        output "PASS" "CRITICAL" "Sufficient RAM" "$mem_gb GB"
    elif [[ $mem_gb -ge 2 ]]; then
        output "WARN" "CRITICAL" "Limited RAM" "$mem_gb GB (4GB+ recommended)"
        ((WARNINGS++))
    else
        output "FAIL" "CRITICAL" "Insufficient RAM" "$mem_gb GB"
        return 1
    fi
    
    local cpu=$(nproc 2>/dev/null || echo "unknown")
    if [[ "$cpu" != "unknown" && $cpu -ge 4 ]]; then
        output "PASS" "CRITICAL" "Sufficient CPU cores" "$cpu cores"
    else
        output "WARN" "CRITICAL" "Limited CPU cores" "$cpu (4+ recommended)"
        ((WARNINGS++))
    fi
    return 0
}

check_avbhome() {
    echo ""
    echo "=== AVB Configuration Directory ==="
    
    if [[ ! -d "$AVBHOME" ]]; then
        output "WARN" "OPTIONAL" "Directory not created" \
            "Will be created by 'sudo make install'. Location: $AVBHOME"
        ((WARNINGS++))
    else
        output "PASS" "OPTIONAL" "AVB configuration directory exists" "$AVBHOME"
        ((PASSED++))
    fi
    return 0
}

print_summary() {
    echo ""
    echo "============================================"
    echo "SUMMARY"
    echo "============================================"
    echo ""
    
    local total_passed=$((PASSED - OPTIONAL_FAILED))
    echo "Critical checks:  $total_passed passed, $CRITICAL_FAILED failed"
    echo "Optional checks:  $((PASSED + CRITICAL_FAILED - total_passed)) passed, $WARNINGS warnings"
    echo ""
    
    if [[ $CRITICAL_FAILED -gt 0 ]]; then
        echo -e "${RED}CRITICAL ISSUES FOUND${NC}"
        echo "The system is NOT ready for $SCENARIO use."
        echo "Fix the critical issues above before proceeding."
        return 1
    elif [[ $OPTIONAL_FAILED -gt 0 || $WARNINGS -gt 0 ]]; then
        echo -e "${YELLOW}MINOR ISSUES FOUND${NC}"
        echo "The system is ready, but some optional features may not work."
        [[ "$SCENARIO" == "production" ]] && echo "Review warnings for full production functionality."
        return 2
    else
        echo -e "${GREEN}ALL CHECKS PASSED${NC}"
        echo "The system is READY for $SCENARIO use!"
        echo ""
        echo "Next steps:"
        if [[ "$SCENARIO" == "development" ]]; then
            echo "  cd $ROOT_DIR && git submodule update --init --recursive"
            echo "  make && sudo make install"
        elif [[ "$SCENARIO" == "testing" ]]; then
            echo "  cd $ROOT_DIR && git submodule update --init jdksavdecc-c kernel-module/igb_avb daemons/gptp daemons/OpenAvnu"
            echo "  make && sudo make install"
            echo "  cd tests/audio && ./test_loopback.sh"
        else
            echo "  cd $ROOT_DIR && git submodule update --init jdksavdecc-c kernel-module/igb_avb daemons/gptp daemons/OpenAvnu"
            echo "  make && sudo make install"
            echo "  cd utils && ./avb_autoconfig.sh <interface>"
            echo "  sudo avb_up.sh <interface> 48000"
        fi
        return 0
    fi
}

main() {
    local any_failed=0
    check_hardware || any_failed=1
    check_kernel || any_failed=1
    check_build_deps || any_failed=1
    check_runtime_deps || any_failed=1
    check_submodules || any_failed=1
    check_memory || any_failed=1
    check_avbhome || any_failed=1
    print_summary
    local result=$?
    [[ $result -eq 1 ]] && exit 1
    [[ $result -eq 2 ]] && exit 2
    exit 0
}

main "$@"
