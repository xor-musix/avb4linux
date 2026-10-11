# snd-igb-avb — Known Issues

## Kernel backtrace in `igb_remove()` during `avb_down.sh` (module unload)

### Symptom
When `avb_down.sh` unloads `snd_igb_avb` (to hand the NIC back to the stock
`igb` driver), `dmesg` shows a backtrace frame in the PCI removal path:

```
snd_igb_avb 0000:05:00.0 enp5s0: shutting down AVB device
snd_igb_avb 0000:05:00.0 enp5s0: freeing ALSA card
snd_igb_avb 0000:05:00.0 enp5s0: AVB device shutdown complete
igb_avb 0000:05:00.0: removed PHC on enp5s0
 igb_remove+0x69/0x1e0 [snd_igb_avb ...]
igb: Intel(R) Gigabit Ethernet Network Driver        <- stock igb reloads
```

### Impact
**Non-fatal.** The module unloads, the stock `igb` driver reloads, and the
interface comes back up (`NIC Link is Up 1000 Mbps Full Duplex`). No hang, no
panic. Observed on kernel 7.0.10 (Manjaro) with the out-of-tree igb_avb base.

### Assessment (why this is not patched yet)
- `igb_remove()` is **not** the stock igb_avb routine. `patches/0011-igb_main.patch`
  replaces its three `cancel_work_sync()` calls (`reset_task`, `dma_err_task`,
  `watchdog_task`) with a single `flush_scheduled_work()` and drops
  `pci_disable_pcie_error_reporting()`.
- `flush_scheduled_work()` is deprecated: the build already warns about it
  (`__warn_flushing_systemwide_wq`, accepted in `warning-baseline.txt`). It runs
  right after `igb_ptp_stop()` (which logs "removed PHC") and the
  `timer_delete_sync()` calls, which matches the `+0x69` offset. It is the prime
  suspect.
- The captured log is a **single frame with no `WARNING:`/`Call Trace:`
  header**, which is not enough to confirm the check that fired.

The likely fix is restoring the three stock `cancel_work_sync()` calls. It is
deferred until the full trace confirms the suspect.

### To diagnose properly (next occurrence)
Capture the complete backtrace, not just the one frame:

```sh
sudo dmesg -wH &                 # or: journalctl -k -f
sudo ./avb_down.sh enp5s0
# then grab the whole block around the trace:
sudo dmesg | grep -nE -B2 -A30 'WARNING:|BUG:|Call Trace:|igb_remove'
```

The header line (`WARNING: CPU... at <file>:<line>`) and the `RIP`/top frame
identify the exact `WARN_ON`/check. Besides `flush_scheduled_work()`, the
other candidates are `igb_ptp_stop()` and `unregister_netdev()` ordering vs. the
ALSA card teardown in `igb_remove()`.
