# ULAMA OEM Delivery — Fast Iteration Cycle

This document explains how ULAMA integrates with the LuckFox **OEM Development System** for fast, accurate updates to the device.

---

## Two Deployment Strategies

| Strategy | Time | Use Case | Command |
|----------|------|----------|---------|
| **OEM Sync** | ~2-4 sec | Development & testing | `./build.sh sync` |
| **Full Reflash** | ~15-20 min | Production & critical changes | `./flash.sh` |

---

## OEM Sync Workflow (Fast)

### Step 1: Build & Stage
```bash
cd /home/pascale/projects/63411/luxfox/media/ulama
./build.sh
```

Output:
```
✓ Files Staged for OEM Sync
  OEM:     /path/to/output/out/oem
  - usr/bin/ulamad
  - etc/ulama.conf
  - etc/init.d/S99ulama
  - usr/bin/scripts/unow-{mon,down}.sh
```

### Step 2: Sync to Device
```bash
cd /home/pascale/projects/63411/luxfox
./build.sh sync --host 192.168.100.1
```

How it works:
- `buildspot-sync-ssh.sh` mirrors `output/out/oem/` → device's `/oem` via rsync
- **Only changed files** are transferred (delta sync)
- Takes **0.5-2 seconds** depending on changes

### Step 3: Test Immediately
```bash
ssh root@192.168.100.1 "/oem/usr/bin/ulamad --help"
ssh root@192.168.100.1 "/etc/init.d/S99ulama status"
```

### Step 4: Iterate
Edit code → `./build.sh` → `./build.sh sync` → test → repeat

---

## Full Reflash Workflow (Production)

### When to Use
- First-time device setup
- Kernel/rootfs changes
- OEM partition corruption
- Production deployment

### Steps
```bash
cd /home/pascale/projects/63411/luxfox

# Build everything (includes OEM)
make media
make
make firmware

# Flash to device
./flash.sh
```

---

## File Mapping

### Host (after `./build.sh`)
```
media/ulama/
├── out/
│   ├── bin/ulamad           ← Built binary
│   ├── etc/ulama.conf       ← Default config
│   └── etc/init.d/S99ulama  ← Init script
└── build.sh                 ← Stages to both paths
```

### Device (after OEM sync or flash)
```
/oem/                                    [OEM partition]
├── usr/bin/ulamad                       ← Deployed binary
├── etc/ulama.conf                       ← Config (editable)
├── etc/init.d/S99ulama                  ← Auto-start service
└── usr/bin/scripts/
    ├── unow-mon.sh                      ← Monitor setup
    └── unow-down.sh                     ← Cleanup script
```

**Note:** Files in `/oem/etc/` can be edited on device; they persist across reboots.

---

## Typical Development Session

```bash
# 1. Make code changes
vi media/ulama/src/core/crsf.c
vi media/ulama/tools/ulamad.c

# 2. Build & stage (output/out/oem/)
cd media/ulama && ./build.sh

# 3. Fast sync to device
cd .. && ./build.sh sync

# 4. Test
ssh root@192.168.100.1 "/oem/usr/bin/ulamad --config /oem/etc/ulama.conf"

# 5. Check logs
ssh root@192.168.100.1 "tail -f /oem/data/logs/ulama.log"

# 6. Iterate → goto step 1
```

**Total time per iteration: ~5-10 seconds** (vs 20 minutes for full flash)

---

## Customizing on Device

### Edit Configuration
```bash
ssh root@192.168.100.1
vi /oem/etc/ulama.conf
/etc/init.d/S99ulama restart
```

### Check Service Status
```bash
ssh root@192.168.100.1 "/etc/init.d/S99ulama status"
```

### View Logs
```bash
ssh root@192.168.100.1 "cat /oem/data/logs/ulama.log"
```

### Restart Service
```bash
ssh root@192.168.100.1 "/etc/init.d/S99ulama restart"
```

---

## Troubleshooting

| Problem | Cause | Solution |
|---------|-------|----------|
| `./build.sh sync` times out | SSH connection issue | Verify device IP: `ping 192.168.100.1` |
| ulamad not found after sync | File didn't transfer | Re-run `./build.sh` then `./build.sh sync` |
| Service doesn't auto-start | Init script not executable | Check: `ssh root@... ls -la /etc/init.d/S99ulama` |
| Old binary still running | Didn't restart service | `ssh root@... /etc/init.d/S99ulama restart` |

---

## See Also
- [OEM Development Guide](../docs/OEM_DEVELOPMENT.md)
- [ULAMA Architecture](./README.md)
- [Host + LuckFox CRSF Smoke Test](./HOST_LUCKFOX_CRSF_SMOKE.md)

---

**Speed principle:** OEM Sync makes iteration **5-10x faster** than full reflash.
Use it for active development. Reserve full flash for production.
