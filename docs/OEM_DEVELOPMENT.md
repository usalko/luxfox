# OEM Development Guide — Custom Software Deployment

**Fast development and deployment of custom software to LuckFox Pico Ultra**

---

## Overview

The **`/oem`** partition is where custom software, applications, and configurations live on the LuckFox device. It's persistent across reboots and independent of the main rootfs.

| Aspect | /oem | rootfs |
|--------|------|--------|
| **Purpose** | Custom software, apps, configs | System core, drivers, kernel |
| **Update Method** | `./build.sh sync` (fast) | Full reflash (slow) |
| **Development** | Iterative, fast cycle | Rare, careful changes |
| **Size** | ~512MB (on eMMC) | ~256MB (compressed) |
| **Persistence** | Survives rootfs updates | Recoverable from image |
| **Best For** | Application development | Firmware/driver modifications |

---

## When to Use /oem vs Full Firmware Flash

### Use `/oem` (./build.sh sync) for:
✅ Custom applications (C, Python, shell scripts)
✅ Service daemons
✅ Configuration files
✅ User data
✅ Development & testing
✅ **Fast iteration** (seconds, not minutes)
✅ Avoiding full reflash cycles

### Use Full Firmware Flash for:
❌ Kernel modifications
❌ Buildroot package changes
❌ Device tree changes
❌ Boot loader updates
❌ Production deployment
❌ First-time device setup

---

## /oem Partition Structure

Standard layout for LuckFox `/oem`:

```
/oem/
├── .ssh/                          # SSH infrastructure
│   ├── id_rsa                     # SSH host private key
│   ├── id_rsa.pub                 # SSH host public key
│   └── authorized_keys            # Allowed SSH public keys
│
├── app/                           # Your applications
│   ├── myapp                      # Binary or script
│   ├── myservice.sh               # Service script
│   └── config.conf                # App configuration
│
├── data/                          # Application data
│   ├── cache/                     # Temporary data
│   ├── logs/                      # Log files
│   └── user/                      # User files
│
├── lib/                           # Shared libraries (optional)
│   └── libmylib.so                # Custom library
│
├── etc/                           # Configuration files
│   ├── myapp.conf                 # App config
│   └── init.d/                    # Init scripts (if needed)
│
├── usr/bin/                       # Executable scripts & utilities
│   ├── buildspot-ssh-daemon.sh    # SSH daemon (auto-installed)
│   ├── fix-ssh-auth.sh            # SSH auth fixes (auto-installed)
│   └── myapp.sh                   # Your utility script
│
└── www/                           # Web interface (optional)
    ├── index.html                 # HTML files
    └── api/                       # REST API endpoints
```

---

## Development Workflow

### Phase 1: Local Development

```bash
# Create local /oem directory structure
mkdir -p output/out/oem/{app,data,etc,usr/bin}

# Create your application
cat > output/out/oem/app/myapp << 'EOF'
#!/bin/sh
echo "Hello from LuckFox!"
EOF
chmod +x output/out/oem/app/myapp

# Add configuration
cat > output/out/oem/etc/myapp.conf << 'EOF'
# MyApp Configuration
DEBUG=1
LOG_LEVEL=INFO
EOF
```

### Phase 2: Deploy to Device

```bash
# Sync everything to device in one command
./build.sh sync

# Result: Files appear in /oem/ on device
```

### Phase 3: Test on Device

```bash
# SSH to device and test
ssh root@192.168.100.1

# On device:
root@luckfox:# /oem/app/myapp
Hello from LuckFox!
```

### Phase 4: Iterate

Edit local files → `./build.sh sync` → Test → Repeat

**Time per iteration: 0.5-2 seconds** (only changed files sync)

---

## Build System Integration

### build.sh Targets

```bash
# Build firmware only (no /oem sync)
./build.sh firmware

# Build firmware and sync /oem (fast development)
./build.sh sync

# Rebuild everything from scratch
./build.sh clean
./build.sh firmware
```

### Local /oem Directory

All files in `output/out/oem/` are synced to device's `/oem/`

```bash
# File mapping:
output/out/oem/app/myapp        → /oem/app/myapp (on device)
output/out/oem/usr/bin/myscript → /oem/usr/bin/myscript (on device)
output/out/oem/etc/config.conf  → /oem/etc/config.conf (on device)
```

### Buildroot Integration

The main `./build.sh firmware` command:
1. Builds kernel, rootfs, and other components
2. Copies `output/out/oem/*` into oem partition
3. Packages into `oem.img`
4. Ready for flashing or sync

**Note:** Use `media/buildspot/build.sh` to populate initial buildspot services.

---

## Common Development Patterns

### Pattern 1: Custom Application Service

**Step 1: Create startup script**
```bash
cat > output/out/oem/app/myservice.sh << 'EOF'
#!/bin/sh
# My custom service

NAME="myservice"
PATH="/oem/app:$PATH"

start() {
    echo "Starting $NAME..."
    /oem/app/myapp &
    echo $! > /var/run/$NAME.pid
}

stop() {
    echo "Stopping $NAME..."
    kill $(cat /var/run/$NAME.pid)
    rm /var/run/$NAME.pid
}

case "$1" in
    start) start ;;
    stop)  stop ;;
    *)     echo "Usage: $0 {start|stop}" ;;
esac
EOF
chmod +x output/out/oem/app/myservice.sh
```

**Step 2: Deploy**
```bash
./build.sh sync
```

**Step 3: Start service on device**
```bash
ssh root@192.168.100.1 "/oem/app/myservice.sh start"
```

---

### Pattern 2: Configuration Management

**Step 1: Organize configs**
```bash
mkdir -p output/out/oem/etc

cat > output/out/oem/etc/myapp.conf << 'EOF'
# MyApp Configuration
APP_NAME="MyLuckFoxApp"
DEBUG=1
LISTEN_PORT=8080
DATA_DIR="/oem/data"
LOG_FILE="/oem/data/logs/app.log"
EOF
```

**Step 2: Create init script that reads config**
```bash
cat > output/out/oem/usr/bin/myapp-start << 'EOF'
#!/bin/sh
. /oem/etc/myapp.conf
echo "Starting $APP_NAME on port $LISTEN_PORT"
/oem/app/myapp --config /oem/etc/myapp.conf
EOF
chmod +x output/out/oem/usr/bin/myapp-start
```

**Step 3: Deploy and test**
```bash
./build.sh sync
ssh root@192.168.100.1 "/oem/usr/bin/myapp-start"
```

---

### Pattern 3: Data Persistence

**Step 1: Create data directories**
```bash
mkdir -p output/out/oem/data/{cache,logs,user}
```

**Step 2: Ensure permissions**
```bash
# On device, data is owned by app user
ssh root@192.168.100.1 "chown -R appuser:appuser /oem/data"
ssh root@192.168.100.1 "chmod 755 /oem/data/*"
```

**Step 3: App writes to /oem/data**
```bash
cat > output/out/oem/app/myapp << 'EOF'
#!/bin/sh
LOG_FILE="/oem/data/logs/app.log"
echo "$(date): App started" >> "$LOG_FILE"
EOF
```

---

### Pattern 4: Binary Applications (C/Go)

**Step 1: Cross-compile for ARM (RV1106)**
```bash
# Use LuckFox toolchain
source tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/env_install_toolchain.sh

# Compile
arm-rockchip830-linux-uclibcgnueabihf-gcc -o output/out/oem/app/myapp myapp.c
chmod +x output/out/oem/app/myapp
```

**Step 2: Sync and run**
```bash
./build.sh sync
ssh root@192.168.100.1 "/oem/app/myapp arg1 arg2"
```

---

### Pattern 5: Python Applications

**Step 1: Add Python to /oem**
```bash
# Create venv or add Python scripts
mkdir -p output/out/oem/app/pyenv
cat > output/out/oem/app/myapp.py << 'EOF'
#!/usr/bin/env python3
import json
import time

print("Hello from Python on LuckFox!")
data = {"time": time.time(), "uptime": "running"}
print(json.dumps(data))
EOF
chmod +x output/out/oem/app/myapp.py
```

**Step 2: Create wrapper script**
```bash
cat > output/out/oem/usr/bin/pyapp << 'EOF'
#!/bin/sh
export PYTHONPATH=/oem/app/pyenv:$PYTHONPATH
/usr/bin/python3 /oem/app/myapp.py "$@"
EOF
chmod +x output/out/oem/usr/bin/pyapp
```

**Step 3: Deploy**
```bash
./build.sh sync
ssh root@192.168.100.1 "/oem/usr/bin/pyapp"
```

---

## Fast Development Cycle

### Iteration Template

```bash
# 1. Edit your code
vi output/out/oem/app/myapp.sh

# 2. Deploy (only changed files synced)
./build.sh sync

# 3. Test immediately
ssh root@192.168.100.1 "/oem/app/myapp.sh test"

# 4. Check logs if needed
ssh root@192.168.100.1 "tail -f /oem/data/logs/app.log"

# 5. Iterate → goto step 1
```

**Typical timing:**
- `./build.sh sync` → 0.5-2 seconds (rsync delta)
- SSH test → 1-2 seconds
- **Total per iteration: ~2-4 seconds**

Compare to full flash:
- Build → 10-15 minutes
- Flash → 2-5 minutes
- Test → 1-2 minutes
- **Total per iteration: ~15-20 minutes**

**Speed improvement: 3-5x faster!** 🚀

---

## Managing /oem During Development

### Before First Sync

Initialize `/oem` directory structure locally:

```bash
# Create directory skeleton
mkdir -p output/out/oem/{app,data/{cache,logs,user},etc,usr/bin}

# Verify structure
tree output/out/oem
# output/out/oem/
# ├── app/
# ├── data/
# │   ├── cache/
# │   ├── logs/
# │   └── user/
# ├── etc/
# └── usr/bin/
```

### Syncing Clean

First sync with all new files:

```bash
./build.sh sync
# [buildspot-sync] Source: output/out/oem (186 files)
# [buildspot-sync] Syncing to root@192.168.100.1:/oem
```

### Syncing Incremental

Subsequent syncs only transfer changed files:

```bash
# Edit one file
echo "new version" > output/out/oem/app/version.txt

# Sync (only 1 file transferred)
./build.sh sync
# [buildspot-sync] Source: output/out/oem (186 files, 1 modified)
# [buildspot-sync] Transfer: 1 file, 0.2 MB
```

### Force Full Resync

If you need everything refreshed:

```bash
# Delete remote files first
ssh root@192.168.100.1 "rm -rf /oem/*"

# Full sync
./build.sh sync
```

### Selective Sync

Sync only specific directories:

```bash
# Sync just app directory
rsync -azv --delete output/out/oem/app/ root@192.168.100.1:/oem/app/

# Sync just configs
rsync -azv output/out/oem/etc/ root@192.168.100.1:/oem/etc/
```

---

## Permissions & Ownership

### Default Permissions on Device

When `/oem` is synced:

| Path | Owner | Perms | Purpose |
|------|-------|-------|---------|
| `/oem/` | root:root | 755 | OEM partition |
| `/oem/app/` | root:root | 755 | Executables |
| `/oem/app/myapp` | root:root | 755 | Executable script |
| `/oem/data/` | root:root | 755 | Data directory |
| `/oem/data/logs/` | root:root | 755 | Log directory |
| `/oem/etc/` | root:root | 755 | Config directory |
| `/oem/etc/config.conf` | root:root | 644 | Config file |

### Custom Ownership

If your app needs specific user:

```bash
# On device
ssh root@192.168.100.1 << 'EOF'
# Create app user
adduser -s /bin/false -h /oem -D appuser

# Set ownership
chown -R appuser:appuser /oem/data
chown appuser:appuser /oem/etc/myapp.conf

# Set secure permissions
chmod 700 /oem/data
chmod 600 /oem/etc/myapp.conf
EOF
```

---

## Security Considerations

### SSH Keys in /oem

If you distribute SSH keys:

```bash
# Store with secure permissions (600)
mkdir -p output/out/oem/.ssh
chmod 700 output/out/oem/.ssh

# Private keys
echo "-----BEGIN RSA PRIVATE KEY-----" > output/out/oem/.ssh/id_rsa
# ... key content ...
chmod 600 output/out/oem/.ssh/id_rsa

# Public keys
echo "ssh-rsa AAAA..." > output/out/oem/.ssh/authorized_keys
chmod 600 output/out/oem/.ssh/authorized_keys
```

### Configuration with Secrets

Don't commit secrets to version control:

```bash
# Good: Template with placeholders
cp config.conf.template output/out/oem/etc/config.conf
# Edit config.conf manually with real values before sync

# Bad: Commit actual passwords
# git add output/out/oem/etc/config.conf  # ❌ Don't do this!
```

### Sanitizing /oem Before Release

```bash
# Remove development artifacts before production build
ssh root@192.168.100.1 << 'EOF'
rm -rf /oem/data/logs/*
rm -rf /oem/data/cache/*
rm -f /oem/.debug
rm -f /oem/etc/*.bak
EOF
```

---

## Troubleshooting OEM Development

### Issue 1: "File not found" after sync

**Cause:** File wasn't synced or wrong path

**Solution:**
```bash
# Check local file exists
ls -la output/out/oem/app/myapp

# Check device file exists
ssh root@192.168.100.1 "ls -la /oem/app/myapp"

# Verify sync completed
ssh root@192.168.100.1 "find /oem -name 'myapp' -type f"
```

### Issue 2: "Permission denied" when running app

**Cause:** File not executable or wrong user

**Solution:**
```bash
# Make executable locally
chmod +x output/out/oem/app/myapp

# Verify on device
ssh root@192.168.100.1 "ls -l /oem/app/myapp"
# Should show: -rwxr-xr-x (755)

# Re-sync to apply permissions
./build.sh sync
```

### Issue 3: App crashes with "library not found"

**Cause:** Missing shared libraries

**Solution:**
```bash
# Check what libraries app needs
arm-rockchip830-linux-uclibcgnueabihf-ldd /oem/app/myapp

# Copy needed libraries to /oem/lib
mkdir -p output/out/oem/lib
cp /path/to/libc.so.6 output/out/oem/lib/

# Add library path when running
export LD_LIBRARY_PATH=/oem/lib:$LD_LIBRARY_PATH
/oem/app/myapp

# Or set in init script
./build.sh sync
```

### Issue 4: Sync is slow

**Cause:** Too many files or large files

**Solution:**
```bash
# Check file count
find output/out/oem -type f | wc -l

# Compress large files
gzip large_file > output/out/oem/data/large_file.gz

# Use --exclude in sync script
# Edit media/buildspot/buildspot-sync-ssh.sh
# Add: --exclude='*.tmp' --exclude='*.log'

# Or manually sync only needed files
rsync -azv output/out/oem/app/ root@192.168.100.1:/oem/app/
```

### Issue 5: Data lost after reboot

**Cause:** Not using persistent /oem storage

**Solution:**
```bash
# Write to /oem/data instead of /tmp
cat > output/out/oem/app/myapp << 'EOF'
#!/bin/sh
# Good: persistent storage
DATA_FILE="/oem/data/mydata.txt"
echo "persistent" > $DATA_FILE

# Bad: temporary storage
# echo "lost after reboot" > /tmp/mydata.txt
EOF
```

---

## Production Deployment

### Release Build

```bash
# 1. Clean local /oem
rm -rf output/out/oem/*

# 2. Add production binaries/configs
mkdir -p output/out/oem/{app,etc}
cp -r /path/to/release/binaries output/out/oem/app/
cp -r /path/to/release/configs output/out/oem/etc/

# 3. Verify structure
tree output/out/oem

# 4. Build firmware
./build.sh firmware

# 5. Image ready for production
ls -lh output/image/oem.img
```

### OTA Update via /oem

For over-the-air updates:

```bash
# 1. Create update package
tar czf myapp-v1.0.tar.gz -C output/out/oem app/

# 2. Transfer to device
scp myapp-v1.0.tar.gz root@192.168.100.1:/tmp/

# 3. Update on device
ssh root@192.168.100.1 << 'EOF'
cd /oem
tar xzf /tmp/myapp-v1.0.tar.gz
sync
EOF

# 4. Verify and restart
ssh root@192.168.100.1 "pkill -f myapp; /oem/app/myapp"
```

---

## Best Practices

### DO ✅

- **Organize files** in clear directory structure
- **Make scripts executable** locally (`chmod +x`)
- **Test immediately** after sync
- **Keep configs** separate from binaries
- **Use /oem/data** for persistent data
- **Version control** your /oem structure (use .gitignore for data/)
- **Document** what each app does in /oem/README

### DON'T ❌

- **Commit binary artifacts** to git (unless intentional)
- **Commit secrets/passwords** in config files
- **Write to /tmp** for persistent data (lost after reboot)
- **Use /oem** for kernel/driver development (use full flash)
- **Forget to make scripts executable** before sync
- **Modify device /oem** manually (changes lost on next sync)
- **Leave debug files** in production builds

---

## Related Documentation

- [BUILD_SH_SYNC.md](./BUILD_SH_SYNC.md) — SSH sync technical details
- [SSH-DNS-SETUP.md](../SSH-DNS-SETUP.md) — Network connectivity
- [SSH-AUTH-TROUBLESHOOTING.md](../SSH-AUTH-TROUBLESHOOTING.md) — SSH authentication
- [media/buildspot/README.md](../media/buildspot/README.md) — Buildspot overview

---

## Quick Reference

| Task | Command |
|------|---------|
| Create /oem structure | `mkdir -p output/out/oem/{app,data,etc,usr/bin}` |
| Sync to device | `./build.sh sync` |
| Run app on device | `ssh root@192.168.100.1 "/oem/app/myapp"` |
| View device logs | `ssh root@192.168.100.1 "tail -f /oem/data/logs/app.log"` |
| SSH to device | `ssh root@192.168.100.1` |
| Full rebuild | `./build.sh clean && ./build.sh firmware` |
| Make script executable | `chmod +x output/out/oem/app/myapp.sh` |
| Check sync progress | Real-time via rsync output in build.sh sync |

---

## Summary

**`./build.sh sync` is the primary development tool for LuckFox:**

- ⚡ **Fast** — Only changed files synced (0.5-2 seconds)
- 🔄 **Iterative** — Edit → sync → test → repeat
- 📦 **Clean** — Separate from rootfs, persistent across updates
- 🚀 **Production-ready** — Use `/oem` for all custom software
- 🔐 **Secure** — SSH encrypted, checksummed transfers

**Development workflow:** `Edit code` → `./build.sh sync` → `Test` → **Repeat**

**For kernel/firmware changes:** Use full `./build.sh firmware` + flash

**For application/service development:** Use `./build.sh sync` + /oem
