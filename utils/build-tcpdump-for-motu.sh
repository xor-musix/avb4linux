#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# build-tcpdump-for-motu.sh
# Cross-compile tcpdump for MOTU devices (ARMv5, static linking)

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Directories
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TOOLCHAIN_DIR="/tmp/toolchain"
TOOLCHAIN_URL="https://musl.cc/arm-linux-musleabi-cross.tgz"

echo -e "${GREEN}=== Building tcpdump for MOTU devices ===${NC}"

# Step 1: Download musl toolchain
echo -e "${YELLOW}[1/5] Checking/downloading musl toolchain...${NC}"
if [ ! -d "$TOOLCHAIN_DIR/arm-linux-musleabi-cross" ]; then
    mkdir -p "$TOOLCHAIN_DIR"
    cd "$TOOLCHAIN_DIR"
    echo "Downloading $TOOLCHAIN_URL"
    wget "$TOOLCHAIN_URL" -O arm-linux-musleabi-cross.tgz
    tar -xf arm-linux-musleabi-cross.tgz
    rm arm-linux-musleabi-cross.tgz
fi
echo -e "${GREEN}Musl toolchain ready at $TOOLCHAIN_DIR/arm-linux-musleabi-cross${NC}"

# Step 2: Initialize submodules and apply patches
echo -e "${YELLOW}[2/5] Initializing git submodules...${NC}"
cd "$PROJECT_DIR"
git submodule update --init utils/tcpdump utils/libpcap
echo -e "${GREEN}Submodules initialized${NC}"

# Apply the SIOCGSTAMP -> gettimeofday fallback patch (idempotent).
# Required because musl's 64-bit time_t makes SIOCGSTAMP resolve to the
# y2038 _NEW ioctl (0x80108906), which the MOTU devices's 2.6.32 kernel rejects
# with EINVAL.
LIBPCAP_PATCH="$SCRIPT_DIR/patches/libpcap-0001-siocgstamp-gettimeofday-fallback.patch"
if git -C "$PROJECT_DIR/utils/libpcap" apply --reverse --check "$LIBPCAP_PATCH" >/dev/null 2>&1; then
    echo -e "${GREEN}libpcap patch already applied${NC}"
elif git -C "$PROJECT_DIR/utils/libpcap" apply --check "$LIBPCAP_PATCH" >/dev/null 2>&1; then
    git -C "$PROJECT_DIR/utils/libpcap" apply "$LIBPCAP_PATCH"
    echo -e "${GREEN}Applied libpcap patch: $(basename "$LIBPCAP_PATCH")${NC}"
else
    echo -e "${RED}Error: libpcap patch does not apply cleanly${NC}"
    exit 1
fi

# Refresh autotools config.sub/config.guess for submodules whose shipped copy
# predates musl support. libpcap 1.5.3 ships a 2009 config.sub that rejects the
# 'arm-linux-musleabi' triple ("system musleabi not recognized"). We copy a
# musl-aware pair from the build machine's autotools install (working-tree only;
# the submodule gitlink stays pinned).
refresh_config_sub() {
    local target_dir="$1"
    if grep -q musl "$target_dir/config.sub" 2>/dev/null; then
        return 0  # already musl-aware
    fi
    local src
    for src in /usr/share/libtool/build-aux /usr/share/automake-*/. \
               /usr/share/autoconf/build-aux; do
        if [ -f "$src/config.sub" ] && [ -f "$src/config.guess" ] \
           && grep -q musl "$src/config.sub"; then
            cp -f "$src/config.sub" "$target_dir/config.sub"
            cp -f "$src/config.guess" "$target_dir/config.guess"
            echo -e "${GREEN}Refreshed config.sub/config.guess in $(basename "$target_dir") from $src${NC}"
            return 0
        fi
    done
    echo -e "${RED}Error: no musl-aware config.sub found on this machine (install autoconf/automake/libtool)${NC}"
    return 1
}

# Step 3: Build libpcap
echo -e "${YELLOW}[3/5] Building libpcap...${NC}"
export PATH="$TOOLCHAIN_DIR/arm-linux-musleabi-cross/bin:$PATH"
cd "$PROJECT_DIR/utils/libpcap"
refresh_config_sub "$PROJECT_DIR/utils/libpcap"
# --with-pcap=linux: capture backend can't be auto-probed when cross-compiling.
./configure --host=arm-linux-musleabi --with-pcap=linux
make -j$(nproc)
echo -e "${GREEN}libpcap built successfully${NC}"

# Step 4: Build tcpdump
echo -e "${YELLOW}[4/5] Building tcpdump...${NC}"
cd "$PROJECT_DIR/utils/tcpdump"
refresh_config_sub "$PROJECT_DIR/utils/tcpdump"
# LDFLAGS=-static: the MOTU has no musl loader, so the binary must be fully
# static (libc + libpcap.a). Without this a fresh configure links dynamically.
./configure --host=arm-linux-musleabi LDFLAGS=-static
rm -f tcpdump   # force a relink so a changed LDFLAGS (e.g. -static) takes effect
make -j$(nproc)
echo -e "${GREEN}tcpdump built successfully${NC}"

# Step 5: Verify binary
echo -e "${YELLOW}[5/5] Verifying binary...${NC}"
BINARY="$PROJECT_DIR/utils/tcpdump/tcpdump"
if [ -f "$BINARY" ]; then
    # Check if it's statically linked
    if file "$BINARY" | grep -q "static"; then
        echo -e "${GREEN}tcpdump binary verified: $BINARY${NC}"
        echo -e "${GREEN}Binary is statically linked (no runtime dependencies)${NC}"
        echo ""
        echo "Binary info:"
        file "$BINARY"
        ls -la "$BINARY"
    else
        echo -e "${RED}Warning: Binary may not be statically linked${NC}"
    fi
else
    echo -e "${RED}Error: tcpdump binary not found${NC}"
    exit 1
fi

echo ""
echo -e "${GREEN}=== Build complete! ===${NC}"
echo "Copy $BINARY to your MOTU device and run:"
echo "  ./tcpdump -i eth0"