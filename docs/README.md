# LuckFox Pico Ultra SDK — Documentation Index

Complete documentation for developers, maintainers, and CI/CD engineers.

---

## 🚀 Quick Start Guides

### [BUILD_SH_SYNC.md](./BUILD_SH_SYNC.md) — Fast File Sync via SSH + rsync
**For:** Developers syncing code to device
- How to use `./build.sh sync`
- Architecture and components
- Troubleshooting
- Performance benchmarks
- CI/CD integration examples

**Start here if:** You want to understand SSH sync, how it works, or troubleshoot sync issues.

### [OEM_DEVELOPMENT.md](./OEM_DEVELOPMENT.md) — Custom Software Development
**For:** Application developers building custom software for /oem
- /oem partition structure and organization
- Development workflow and iteration patterns
- Common patterns (services, configs, data, binaries, Python)
- Fast development cycle (seconds, not minutes)
- Deployment best practices
- Production release and OTA updates

**Start here if:** You're building custom applications for LuckFox (primary use case).

---

## 📡 Network & SSH Configuration

### [../SSH-DNS-SETUP.md](../SSH-DNS-SETUP.md) — Network Setup Guide
**For:** Setting up Ethernet connection and DNS
- Connecting device to host via RJ45
- Configuring static IP (192.168.100.0/24)
- DHCP+DNS server setup (dnsmasq)
- Host DNS configuration (systemd-resolved)

**Start here if:** Device won't connect to your network or you need DNS/DHCP help.

### [../SSH-AUTH-TROUBLESHOOTING.md](../SSH-AUTH-TROUBLESHOOTING.md) — SSH Key Authentication
**For:** Fixing SSH "Permission denied" errors
- Root causes: PubkeyAuthentication disabled, wrong permissions
- Automatic firmware fixes
- Manual workarounds
- SSH config setup

**Start here if:** SSH key auth is failing or you get "Too many authentication failures".

---

## 🔧 Build System

### [../media/buildspot/README.md](../media/buildspot/README.md) — Buildspot Service Overview
**For:** Understanding the buildspot SSH service architecture
- UART vs SSH sync comparison
- Buildspot components
- rsync configuration
- Known issues and solutions

**Start here if:** You want to understand how the overall SSH infrastructure works.

### [BUILD_SH_SYNC.md](./BUILD_SH_SYNC.md) — build.sh sync Details
See **Quick Start Guides** section above.

---

## 📚 Related Source Code

| File | Purpose |
|------|---------|
| `build.sh` (line 785-801) | Main build script `build_sync()` function |
| `media/buildspot/build.sh` | Stages buildspot services into firmware |
| `media/buildspot/buildspot-sync-ssh.sh` | SSH+rsync sync engine |
| `media/buildspot/buildspot-ssh-daemon.sh` | Device-side SSH daemon service |
| `media/buildspot/dnsmasq.conf` | DHCP+DNS server configuration |
| `media/buildspot/fix-ssh-auth.sh` | SSH permission fixes (runs on device) |

---

## 🎯 Common Tasks

### Task: First-time device connection
1. Read: [SSH-DNS-SETUP.md](../SSH-DNS-SETUP.md)
2. Run: `/dns-setup-ubuntu.sh` on host
3. Connect: `ssh root@192.168.100.1` (password: luckfox)

### Task: Setup SSH key authentication
1. Generate key: `ssh-keygen -t rsa -b 2048 -f ~/.ssh/id_rsa -N ""`
2. Copy to device: `ssh-copy-id -i ~/.ssh/id_rsa.pub root@192.168.100.1`
3. Test: `ssh -i ~/.ssh/id_rsa root@192.168.100.1`

### Task: Develop custom application
1. Read: [OEM_DEVELOPMENT.md](./OEM_DEVELOPMENT.md)
2. Create: `mkdir -p output/out/oem/{app,data,etc,usr/bin}`
3. Add code: `cat > output/out/oem/app/myapp.sh << 'EOF'...`
4. Deploy: `./build.sh sync`
5. Test: `ssh root@192.168.100.1 "/oem/app/myapp.sh"`
6. Iterate: Edit → Sync → Test (cycle time: ~2-4 seconds!)

### Task: Deploy custom service
1. Read: [OEM_DEVELOPMENT.md - Common Development Patterns](./OEM_DEVELOPMENT.md#common-development-patterns)
2. Create service script in `output/out/oem/app/`
3. Deploy: `./build.sh sync`
4. Enable on device: `ssh root@192.168.100.1 "/oem/app/service.sh start"`

### Task: Debug SSH connection issues
1. Check connectivity: `ping 192.168.100.1`
2. Test SSH: `ssh -vvv root@192.168.100.1`
3. Check device logs: `ssh root@192.168.100.1 "tail -50 /var/log/auth.log"`
4. Read: [SSH-AUTH-TROUBLESHOOTING.md](../SSH-AUTH-TROUBLESHOOTING.md)

### Task: Modify sync parameters
1. Edit: `build.sh` function `build_sync()` (line 785-801)
2. Or override: `./build.sh sync --host 192.168.50.10 --user myuser`
3. Reference: [BUILD_SH_SYNC.md](./BUILD_SH_SYNC.md#configuration--customization)

---

## 📖 Documentation Map

```
docs/
├── README.md                       — Documentation index (this file)
├── OEM_DEVELOPMENT.md              — Custom software development (START HERE for app dev)
└── BUILD_SH_SYNC.md                — SSH sync technical details

Root level:
├── SSH-DNS-SETUP.md                — Network configuration
├── SSH-AUTH-TROUBLESHOOTING.md     — SSH key issues
└── media/buildspot/README.md       — Buildspot service overview

Source code:
├── build.sh                        — build_sync() function
└── media/buildspot/
    ├── build.sh                    — Staging script
    ├── buildspot-sync-ssh.sh       — Sync engine
    ├── buildspot-ssh-daemon.sh     — Device service
    ├── fix-ssh-auth.sh             — Permission fixes
    └── dnsmasq.conf                — Network config
```

---

## 🔍 Search Tips

**By feature:**
- SSH auth issues → [SSH-AUTH-TROUBLESHOOTING.md](../SSH-AUTH-TROUBLESHOOTING.md)
- Network setup → [SSH-DNS-SETUP.md](../SSH-DNS-SETUP.md)
- File sync → [BUILD_SH_SYNC.md](./BUILD_SH_SYNC.md)
- OEM development → [OEM_DEVELOPMENT.md](./OEM_DEVELOPMENT.md)
- Build system → [BUILD_SH_SYNC.md](./BUILD_SH_SYNC.md#architecture--components)

**By role:**
- **Developer** → START: [OEM_DEVELOPMENT.md](./OEM_DEVELOPMENT.md) for custom software
- **Developer (sync focused)** → START: [BUILD_SH_SYNC.md](./BUILD_SH_SYNC.md#quick-start-for-developers)
- **DevOps/CI** → READ: [BUILD_SH_SYNC.md](./BUILD_SH_SYNC.md#integration-with-cicd)
- **Maintainer** → READ: [BUILD_SH_SYNC.md](./BUILD_SH_SYNC.md#maintenance--future-development)
- **Network admin** → START: [SSH-DNS-SETUP.md](../SSH-DNS-SETUP.md)

**By problem:**
- Can't connect device → [SSH-DNS-SETUP.md](../SSH-DNS-SETUP.md#troubleshooting)
- SSH key rejected → [SSH-AUTH-TROUBLESHOOTING.md](../SSH-AUTH-TROUBLESHOOTING.md#root-cause-analysis)
- Sync too slow → [BUILD_SH_SYNC.md](./BUILD_SH_SYNC.md#issue-5-slow-synchronization)
- Build fails → Check: `./build.sh sync --help`
- Writing custom app → [OEM_DEVELOPMENT.md](./OEM_DEVELOPMENT.md#common-development-patterns)

---

## 📋 Quick Reference

## 📋 Quick Reference

| Command/Resource | Purpose | Docs |
|---------|---------|------|
| `./build.sh sync` | Sync custom software to device (/oem) | [OEM_DEVELOPMENT.md](./OEM_DEVELOPMENT.md) |
| `./build.sh firmware` | Build complete firmware (no /oem sync) | build.sh |
| `/dns-setup-ubuntu.sh` | Setup host DNS for device resolution | [SSH-DNS-SETUP.md](../SSH-DNS-SETUP.md) |
| `ssh root@192.168.100.1` | SSH to device | [SSH-DNS-SETUP.md](../SSH-DNS-SETUP.md) |
| `/oem/usr/bin/fix-ssh-auth.sh` | Fix SSH auth on device (run on device) | [SSH-AUTH-TROUBLESHOOTING.md](../SSH-AUTH-TROUBLESHOOTING.md) |
| `/oem/usr/bin/diagnose-ssh-auth.sh` | Diagnose SSH issues (run on device) | [SSH-AUTH-TROUBLESHOOTING.md](../SSH-AUTH-TROUBLESHOOTING.md) |
| `/oem/usr/bin/diagnose-ssh-dhcp.sh` | Diagnose DHCP issues (run on device) | [SSH-DNS-SETUP.md](../SSH-DNS-SETUP.md) |
| `output/out/oem/` | Local /oem directory (synced to device) | [OEM_DEVELOPMENT.md](./OEM_DEVELOPMENT.md#oem-partition-structure) |

---

## 🤝 Contributing

When modifying SSH sync, buildspot, or network components:

1. Update relevant documentation files
2. Follow patterns in existing docs (code examples, troubleshooting)
3. Add new sections to [BUILD_SH_SYNC.md](./BUILD_SH_SYNC.md) if adding features
4. Test your changes: `./build.sh sync`
5. Document any new parameters or options

---

## 📞 Support Resources

**Internal Docs:**
- [OEM_DEVELOPMENT.md](./OEM_DEVELOPMENT.md) — Custom software development (primary)
- [BUILD_SH_SYNC.md](./BUILD_SH_SYNC.md) — Comprehensive technical guide
- [SSH-DNS-SETUP.md](../SSH-DNS-SETUP.md) — Network setup
- [SSH-AUTH-TROUBLESHOOTING.md](../SSH-AUTH-TROUBLESHOOTING.md) — Auth issues

**Build Scripts:**
- `build.sh` — Root build script with `build_sync()` function
- `media/buildspot/buildspot-sync-ssh.sh` — Actual sync implementation

**Device Diagnostics:**
- `/oem/usr/bin/diagnose-ssh-auth.sh` — SSH diagnostics (on device)
- `/oem/usr/bin/diagnose-ssh-dhcp.sh` — DHCP diagnostics (on device)
- `/oem/usr/bin/fix-ssh-auth.sh` — Auto-fix SSH issues (on device)

---

**Last Updated:** June 15, 2026

This documentation is maintained for developers and CI/CD systems. If you find issues or gaps, please update the relevant docs file.
