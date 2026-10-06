# Bifrost Network Protocol - System Requirements & Tuning Guide

## 📋 Overview

Bifrost provides two distinct communication paradigms:

| Layer | Technology | Latency | Use Case |
|-------|------------|---------|----------|
| **Local** | Shared Memory | Sub-microsecond | Intra-system IPC |
| **Network** | Raw Socket / AF_XDP | Hardware dependent | Cross-system networking |
| **Gateway** | End-to-End Pipeline | Network + Local | Full distributed system |

---

## 🏗️ System Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        Bifrost Architecture                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│    ┌─────────────┐         ┌─────────────┐         ┌─────────────┐          │
│    │  Process A  │◀───────▶│ Bifrost  │◀───────▶│  Process B  │          │
│    │   Writer    │   SHM   │    Core     │   SHM   │   Reader    │          │
│    └─────────────┘         └──────┬──────┘         └─────────────┘          │
│                                   │                                         │
│                            ┌──────┴──────┐                                  │
│                            │   Gateway   │                                  │
│                            └──────┬──────┘                                  │
│                                   │ Raw Socket                              │
│                            ┌──────┴──────┐                                  │
│                            │   Network   │                                  │
│                            │  (AF_XDP)   │                                  │
│                            └─────────────┘                                  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 🖥️ System Requirements

### Minimum Requirements

| Component | Requirement |
|-----------|-------------|
| **OS** | Linux 4.18+ (Ubuntu 20.04+, RHEL 8+) |
| **Kernel** | 5.x recommended for best AF_XDP support |
| **CPU** | x86_64 with SSE4.2, 2+ cores |
| **Memory** | 4GB RAM minimum |
| **Network** | Any Ethernet NIC (1Gbps+) |

### Recommended for Production

| Component | Requirement |
|-----------|-------------|
| **OS** | Ubuntu 22.04 LTS or RHEL 8.6+ |
| **Kernel** | Linux 5.15+ |
| **CPU** | Intel Xeon / AMD EPYC, 4+ cores, isolated cores |
| **Memory** | 16GB+ with huge pages enabled |
| **Network** | Intel i40e, ixgbe, mlx5 (for XDP native/zero-copy) |

### Required Packages

```bash
# Ubuntu/Debian
sudo apt install build-essential meson ninja-build libbpf-dev libxdp-dev

# RHEL/CentOS
sudo dnf install gcc meson ninja-build libbpf-devel libxdp-devel
```

---

## ⚡ Performance Tuning

### 1. CPU Isolation

Reserve CPUs for Bifrost to prevent interference:

```bash
# /etc/default/grub
GRUB_CMDLINE_LINUX="isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3"
sudo update-grub && reboot
```

### 2. Huge Pages

Enable huge pages for reduced TLB misses:

```bash
# Allocate 1024 x 2MB huge pages
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages

# Permanent (add to /etc/sysctl.conf)
vm.nr_hugepages=1024
vm.hugetlb_shm_group=1000  # Your group ID
```

### 3. Network Buffer Tuning

```bash
# Increase socket buffer sizes
sudo sysctl -w net.core.rmem_max=67108864
sudo sysctl -w net.core.wmem_max=67108864
sudo sysctl -w net.core.rmem_default=16777216
sudo sysctl -w net.core.wmem_default=16777216

# Increase backlog
sudo sysctl -w net.core.netdev_max_backlog=300000

# Disable reverse path filtering for raw sockets
sudo sysctl -w net.ipv4.conf.all.rp_filter=0
```

### 4. IRQ Affinity

Dedicate NIC interrupts to specific CPUs:

```bash
# Find NIC IRQs
cat /proc/interrupts | grep eth0

# Set affinity (example: IRQ 32 to CPU 1)
echo 2 | sudo tee /proc/irq/32/smp_affinity
```

### 5. Disable Power Saving

```bash
# Disable CPU frequency scaling
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance | sudo tee $cpu
done

# Disable C-states in BIOS or via kernel
# GRUB: intel_idle.max_cstate=0 processor.max_cstate=0
```

### 6. QDISC Bypass

Bifrost automatically enables this, but verify:

```bash
# Check if enabled
tc qdisc show dev eth0

# Manual bypass (if needed)
sudo tc qdisc add dev eth0 root pfifo_fast
```

---

## 📊 Performance Expectations

Since Bifrost supports AF_XDP (kernel bypass) and raw UDP sockets, network performance is highly dependent on your physical infrastructure rather than software overhead.

**Factors affecting real-world network performance:**
1. **NIC Capabilities**: Cards with native AF_XDP support (e.g., Intel i40e, Mellanox ConnectX) will provide significantly lower latency than standard adapters.
2. **Physical Medium**: Direct Attach Copper (DAC) or fiber optic connections over short distances (same rack) will minimize propagation delay.
3. **Switch Overhead**: If routing through switches, enterprise cut-through switches are required for ultra-low latency.
4. **Kernel Tuning**: Proper CPU isolation and IRQ affinity are critical for stable latency percentiles.

*Note: For the most accurate metrics, you should benchmark the `bifrost_xdp_sender` and `bifrost_xdp_receiver` examples on your specific target hardware and network topology.*

---

## 🔧 Component Reference

### Files

| File | Description |
|------|-------------|
| `include/bifrost_gateway.h` | Gateway API header |
| `src/bifrost_gateway.c` | Gateway implementation |
| `include/bifrost_xdp.h` | AF_XDP kernel bypass header |
| `src/bifrost_xdp.c` | AF_XDP implementation |
| `include/bifrost_netproto.h` | UDP network protocol header |
| `src/bifrost_netproto.c` | UDP network protocol implementation |
| `examples/bifrost_xdp_sender.c` | High-performance network sender |
| `examples/bifrost_xdp_receiver.c` | High-performance network receiver |
| `examples/bifrost_gateway_example.c` | Gateway usage example |

### Gateway Usage

Using the Gateway API in your code:

```c
#include "bifrost_gateway.h"

// Create configuration
bifrost_gateway_config_t config = bifrost_gateway_config_default();
config.interface = "enp0s31f6";
config.remote_mac = "aa:bb:cc:dd:ee:ff";
config.remote_ip = "192.168.1.100";
config.pool_name = "my_pool";
config.mode = BIFROST_GATEWAY_BRIDGE;

// Create and run gateway
bifrost_gateway_t* gw = bifrost_gateway_create(&config);
bifrost_gateway_run(gw);  // Blocking, or use bifrost_gateway_process() in a loop
bifrost_gateway_destroy(gw);
```

Or run the example:

```bash
# Build
meson compile -C build bifrost_gateway_example
# Or: ninja -C build bifrost_gateway_example

# Run (requires root)
sudo ./build/examples/bifrost_gateway_example lo 00:00:00:00:00:00 127.0.0.1 shm2net
```

---

## 🐛 Troubleshooting

### Permission Denied

```bash
# Raw sockets require root
sudo ./bifrost_xdp_sender ...

# Or add capabilities
sudo setcap cap_net_raw,cap_net_admin+ep ./bifrost_xdp_sender
```

### Interface Not Found

```bash
# List available interfaces
ip link show

# Use correct name (e.g., enp0s31f6, not eth0)
```

### No Packets Received

1. Check firewall: `sudo iptables -L`
2. Check interface is UP: `ip link show <interface>`
3. Verify MAC/IP addresses are correct
4. Run receiver BEFORE sender

### High Latency

1. Enable CPU isolation
2. Disable hyper-threading (BIOS)
3. Set CPU governor to `performance`
4. Use dedicated network interface

---

## 📈 Monitoring

### Real-time Statistics

Both sender and receiver display live statistics:

```
📊 Pkts: 6 M | Rate: 0.63 M/s | Lat: min=4148 avg=56078 p99=164200 ns
```

### Per-Symbol Statistics

The receiver tracks per-symbol latency:

```
│ Symbol   │    Count   │  Avg Lat   │  Min Lat   │  Max Lat   │
│ AAPL     │     782102 │  51131.9 ns│   4148.0 ns│ 2462962.0 ns│
```

---

## 🔐 Security Considerations

1. **Run as dedicated user** with minimal privileges
2. **Use network namespaces** for isolation
3. **Firewall rules** to restrict access
4. **TLS/Encryption** should be added for production (not included in base protocol)

---

## 📚 Related Documentation

- [Bifrost Core README](README.md)
- [Linux AF_XDP Documentation](https://www.kernel.org/doc/html/latest/networking/af_xdp.html)
- [High-Performance Networking Best Practices](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/8/html/configuring_and_managing_networking/index)
