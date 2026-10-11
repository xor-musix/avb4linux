# AVB Network Configuration

This directory contains configuration files and scripts for separating AVB traffic from non-AVB traffic on two network interfaces.

## 🚀 Quick Start (Recommended)

The easiest way to configure your network is to use the interactive script:

```bash
cd utils/network-config
sudo ./apply-network-config.sh
```

This script will:
1. **Detect all available Ethernet interfaces**
2. **Identify AVB-capable NICs** (Intel I210/I211)
3. **Ask you to select** which interface is for AVB-only traffic
4. **Detect your network settings** (subnet, gateway, DNS)
5. **Generate configuration files** based on your selections
6. **Create backups** of existing configuration
7. **Apply the new configuration**

## 📋 Manual Configuration (Alternative)

If you prefer to manually configure the network, you can edit the configuration files directly:

### What You Need to Know

1. **Your Network Subnet** - What is your LAN subnet? (e.g., 192.168.1.0/24, 192.168.0.0/24, 10.0.0.0/24)
2. **Your Gateway** - What is your router/gateway IP? (usually .1 or .254)
3. **Your DNS** - What is your DNS server? (usually same as gateway, or 8.8.8.8)
4. **Available IP Addresses** - Choose IPs that are not in use by other devices

### Example: If Your Network is 192.168.1.0/24

| Interface | Purpose | IP Address | Gateway | DNS |
|-----------|---------|------------|---------|-----|
| eno1 | NON-AVB traffic | 192.168.1.100/24 | 192.168.1.1 | 192.168.1.1 |
| enp5s0 | AVB-only traffic | 192.168.1.101/24 | Not configured | Not configured |

## Interface Configuration

| Interface | Purpose | IP Address | Gateway | DNS |
|-----------|---------|------------|---------|-----|
| eno1 | NON-AVB traffic (internet, LAN, Web Controller) | 192.168.1.100/24 | 192.168.1.1 | 192.168.1.1 |
| enp5s0 | AVB-only traffic (gPTP, SRP, 1722 streams) | 192.168.1.101/24 | Not configured | Not configured |

## Key Design Decisions

1. **Same Subnet**: Both interfaces are in the same subnet to allow direct communication with AVB devices and the router.

2. **No Gateway on AVB Interface**: The AVB interface has no gateway configured, preventing routing through this interface. This ensures only control traffic crosses the LAN while AVB multicast stays local.

3. **No Bridging**: The interfaces are NOT bridged. Each interface operates independently with its own IP address.

4. **No NAT**: No NAT is configured. All devices are on the same flat LAN.

## Files

- `apply-network-config.sh` - **Interactive script to apply configuration** (recommended)
- `test-network-config.sh` - Script to verify the configuration
- `10-static-enp5s0.network` - Example systemd-networkd configuration for AVB interface (reference only)
- `20-static-eno1.network` - Example systemd-networkd configuration for non-AVB interface (reference only)

**Note:** The example `.network` files are for reference only. The `apply-network-config.sh` script generates new configuration files dynamically based on your interface names and network settings.

## Usage

### Apply Configuration (Interactive)

```bash
cd utils/network-config
sudo ./apply-network-config.sh
```

This will guide you through:
1. Interface detection and selection
2. Network configuration detection
3. IP address assignment
4. Configuration file generation
5. Backup creation
6. Configuration application

### Test Configuration

```bash
cd utils/network-config
./test-network-config.sh
```

This will verify:
- Both interfaces are UP
- Both interfaces are in the same subnet
- Only the NON-AVB interface has gateway
- AVB interface has no DNS configured
- Connectivity tests

### Restore Original Configuration

If you need to restore the original configuration:

```bash
# List available backups
ls -la /etc/systemd/network/backups/

# Restore a specific backup (replace timestamp with actual backup file)
sudo cp /etc/systemd/network/backups/*.network.<timestamp>.backup /etc/systemd/network/

# Restart systemd-networkd
sudo systemctl restart systemd-networkd
```

## AVB Traffic Isolation

The configuration ensures:

1. **AVB traffic stays local**: Multicast traffic (gPTP, SRP, 1722) stays on the AVB switch and doesn't cross to the main LAN.

2. **Control traffic crosses LAN**: Only control traffic (Web Controller, firmware updates) crosses to the main LAN via the NON-AVB interface.

3. **No routing conflicts**: Since the AVB interface has no gateway, the system won't try to route traffic through it.

## Hardware

- **eno1**: Intel I218-LM (onboard) - NON-AVB traffic
- **enp5s0**: Intel I210 - AVB-capable, handles gPTP, SRP, 1722 streams

## Troubleshooting

### Interface not detected
Run `ip link show` to see all available interfaces.

### Configuration not applied
Check `systemctl status systemd-networkd` for errors.

### No connectivity
Verify the gateway and DNS settings match your network configuration.