#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# start-http-server.sh
# Start a simple HTTP server for downloading tcpdump to MOTU device

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

PORT=8080

# Get local IP for easy reference
LOCAL_IP=$(ip route get 1.1.1.1 2>/dev/null | awk '{print $7}' || echo "unknown")

echo "Starting HTTP server on port $PORT"
echo "Serving files from: $PROJECT_DIR"
echo ""
echo "From your MOTU device, run:"
echo "  cd /tmp"
echo "  wget http://$LOCAL_IP:$PORT/utils/tcpdump/tcpdump -O tcpdump"
echo "  chmod +x tcpdump"
echo "  ./tcpdump -i eth0"
echo ""
echo "Press Ctrl+C to stop the server"

cd "$PROJECT_DIR"
exec python3 -m http.server "$PORT"