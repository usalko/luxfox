#!/bin/bash

##############################################################################
# build.sh — Build ULAMA and stage files for flashing to LuckFox
#
# Builds ULAMA and stages files into the same directories that `./flash.sh`
# packages into rootfs.img, and also prepares files for OEM sync.
#
# Usage:
#   ./build.sh [COMMAND] [OPTIONS]
#
# Commands:
#   build                Build and stage files (default)
#   test                 Interactive hardware smoke test
#
# Options:
#   -h, --help           Show this help
#   -c, --clean          Clean build artifacts
#   -v, --verbose        Verbose output
#
# Examples:
#   ./build.sh                      # Build all targets and stage files
#   ./build.sh -c                   # Clean and rebuild
#   ./build.sh test                 # Run hardware smoke test (interactive)
#
##############################################################################

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ULAMA_ROOT="$SCRIPT_DIR"
UNOW_ROOT="$SCRIPT_DIR/../unow"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../" && pwd)"
OUTPUT_ROOT="$PROJECT_ROOT/output/out"

# Firmware staging paths (same as used by main SDK build)
# - output/out/media_out/root -> copied into rootfs during build_firmware
# - output/out/oem -> synced to /oem on device via ./build.sh sync
MEDIA_ROOT_STAGING="$OUTPUT_ROOT/media_out/root"
OEM_STAGING="$OUTPUT_ROOT/oem"

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Options
CLEAN=0
VERBOSE=0

# Logging functions
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_debug() {
    if [ $VERBOSE -eq 1 ]; then
        echo -e "${BLUE}[DEBUG]${NC} $1"
    fi
}

print_usage() {
    grep "^#" "$0" | grep -E "^# " | sed 's/^# //' | head -30
}

##############################################################################
# CLEAN
##############################################################################
do_clean() {
    log_info "Cleaning build artifacts..."
    
    log_debug "Removing build/ and out/"
    rm -rf "$ULAMA_ROOT/build" "$ULAMA_ROOT/out"
    
    log_info "Clean complete"
}

##############################################################################
# VERSION GENERATION
##############################################################################
generate_version() {
    local version_file="$ULAMA_ROOT/include/ulama/ulama_version.h"
    local build_number_file="$ULAMA_ROOT/.build_number"
    local build_num=0
    
    # Increment build number
    if [ -f "$build_number_file" ]; then
        build_num=$(($(cat "$build_number_file") + 1))
    fi
    echo "$build_num" > "$build_number_file"
    
    # Get git info
    local git_hash=""
    local git_branch=""
    if git rev-parse --git-dir > /dev/null 2>&1; then
        git_hash=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
        git_branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")
    fi
    
    # Generate version header
    cat > "$version_file" << EOF
#ifndef ULAMA_VERSION_H
#define ULAMA_VERSION_H

#define ULAMA_BUILD_NUMBER    $build_num
#define ULAMA_GIT_HASH        "$git_hash"
#define ULAMA_GIT_BRANCH      "$git_branch"
#define ULAMA_BUILD_DATE      "$(date '+%Y-%m-%d %H:%M:%S')"

#endif /* ULAMA_VERSION_H */
EOF
    
    log_debug "Generated version: build #$build_num ($git_branch@$git_hash)"
}

##############################################################################
# BUILD
##############################################################################
do_build() {
    log_info "=========================================="
    log_info "Building ULAMA suite"
    log_info "=========================================="

    cd "$ULAMA_ROOT"

    # Generate version before building — shared by all components
    generate_version

    local VCPD_ROOT="$ULAMA_ROOT/../vcpd"
    local GW_ROOT="$ULAMA_ROOT/../ulama-gw"
    local RADIOD_ROOT="$ULAMA_ROOT/../radiod"

    log_info "Step 1/8: ulama host tests"
    make host || {
        log_error "ulama host failed"
        return 1
    }

    log_info "Step 2/8: ulama host-unow"
    make host-unow || {
        log_error "ulama host-unow failed"
        return 1
    }

    log_info "Step 3/8: ulama ARM build"
    make || {
        log_error "ulama ARM build failed"
        return 1
    }

    log_info "Step 4/8: vcpd"
    if [ -f "$VCPD_ROOT/Makefile" ]; then
        make -C "$VCPD_ROOT" host || log_warn "vcpd host build failed (non-fatal)"
        make -C "$VCPD_ROOT" || log_warn "vcpd ARM build failed (non-fatal)"
    else
        log_warn "vcpd not found at $VCPD_ROOT, skipping"
    fi

    log_info "Step 5/8: radiod host-unow"
    if [ -f "$RADIOD_ROOT/Makefile" ]; then
        make -C "$RADIOD_ROOT" host-unow || log_warn "radiod host-unow build failed (non-fatal)"
    else
        log_warn "radiod not found at $RADIOD_ROOT, skipping"
    fi

    log_info "Step 6/8: radiod ARM build"
    if [ -f "$RADIOD_ROOT/Makefile" ]; then
        make -C "$RADIOD_ROOT" || log_warn "radiod ARM build failed (non-fatal)"
    else
        log_warn "radiod not found at $RADIOD_ROOT, skipping"
    fi

    log_info "Step 7/8: ulama-gw host-unow"
    if [ -f "$GW_ROOT/Makefile" ]; then
        make -C "$GW_ROOT" host-unow || {
            log_error "ulama-gw host-unow build failed"
            return 1
        }
    else
        log_warn "ulama-gw not found at $GW_ROOT, skipping"
    fi

    log_info "Step 8/8: ulama-gw ARM build"
    if [ -f "$GW_ROOT/Makefile" ]; then
        make -C "$GW_ROOT" || log_warn "ulama-gw ARM build failed (non-fatal)"
    fi

    return 0
}

##############################################################################
# STAGE FILES FOR FIRMWARE PACKAGING
##############################################################################
stage_files() {
    log_info "=========================================="
    log_info "Staging files for firmware packaging & OEM sync"
    log_info "=========================================="

    log_info "Rootfs staging: $MEDIA_ROOT_STAGING"
    log_info "OEM staging:    $OEM_STAGING"

    local VCPD_ROOT="$ULAMA_ROOT/../vcpd"
    local RADIOD_ROOT="$ULAMA_ROOT/../radiod"

    # Create staging directories
    mkdir -p "$MEDIA_ROOT_STAGING/usr/bin/scripts"
    mkdir -p "$MEDIA_ROOT_STAGING/etc/init.d"
    mkdir -p "$OEM_STAGING/usr/bin/scripts"
    mkdir -p "$OEM_STAGING/etc/init.d"

    # ---- Binaries → OEM ----

    local bins_staged=""

    for pair in \
        "$ULAMA_ROOT/out/bin/ulamad:ulamad" \
        "$VCPD_ROOT/out/bin/vcpd:vcpd" \
        "$RADIOD_ROOT/out/bin/radiod:radiod"; do
        local src="${pair%%:*}"
        local name="${pair##*:}"
        if [ -f "$src" ]; then
            cp -f "$src" "$OEM_STAGING/usr/bin/$name"
            chmod +x "$OEM_STAGING/usr/bin/$name"
            bins_staged="$bins_staged $name"
            log_debug "Staged $name"
        else
            log_warn "Binary not found: $src (skipping)"
        fi
    done

    # ---- Configs → rootfs ----

    for pair in \
        "$ULAMA_ROOT/defaults/ulama.conf:ulama.conf" \
        "$VCPD_ROOT/defaults/vcpd.conf:vcpd.conf" \
        "$RADIOD_ROOT/defaults/radiod.conf:radiod.conf"; do
        local src="${pair%%:*}"
        local name="${pair##*:}"
        if [ -f "$src" ]; then
            cp -f "$src" "$MEDIA_ROOT_STAGING/etc/$name"
            log_debug "Staged /etc/$name"
        else
            log_warn "Config not found: $src (skipping)"
        fi
    done

    # ---- Init script → rootfs + OEM ----

    if [ -f "$RADIOD_ROOT/scripts/S96radiod" ]; then
        cp -f "$RADIOD_ROOT/scripts/S96radiod" "$MEDIA_ROOT_STAGING/etc/init.d/S96radiod"
        chmod +x "$MEDIA_ROOT_STAGING/etc/init.d/S96radiod"
        cp -f "$RADIOD_ROOT/scripts/S96radiod" "$OEM_STAGING/etc/init.d/S96radiod"
        chmod +x "$OEM_STAGING/etc/init.d/S96radiod"
        log_debug "Staged /etc/init.d/S96radiod"
    fi

    # ---- Helper scripts → both ----

    for script in unow-mon.sh unow-down.sh; do
        if [ -f "$UNOW_ROOT/scripts/$script" ]; then
            cp -f "$UNOW_ROOT/scripts/$script" "$OEM_STAGING/usr/bin/scripts/$script"
            chmod +x "$OEM_STAGING/usr/bin/scripts/$script"
            cp -f "$UNOW_ROOT/scripts/$script" "$MEDIA_ROOT_STAGING/usr/bin/scripts/$script"
            chmod +x "$MEDIA_ROOT_STAGING/usr/bin/scripts/$script"
            log_debug "Staged $script"
        else
            log_warn "Script not found: $UNOW_ROOT/scripts/$script (skipping)"
        fi
    done

    log_info "Staged binaries:$bins_staged"

    return 0
}

##############################################################################
# POST-BUILD SUMMARY
##############################################################################
print_summary() {
    log_info "=========================================="
    log_info "Build Summary"
    log_info "=========================================="

    local VCPD_ROOT="$ULAMA_ROOT/../vcpd"
    local RADIOD_ROOT="$ULAMA_ROOT/../radiod"
    local GW_ROOT="$ULAMA_ROOT/../ulama-gw"

    echo ""
    for pair in \
        "ulamad:$ULAMA_ROOT/out/bin/ulamad" \
        "vcpd:$VCPD_ROOT/out/bin/vcpd" \
        "radiod:$RADIOD_ROOT/out/bin/radiod" \
        "ulama-gw:$GW_ROOT/out/bin/ulama-gw"; do
        local name="${pair%%:*}"
        local path="${pair##*:}"
        if [ -f "$path" ]; then
            echo -e "  ${GREEN}✓${NC} $name  ($path)"
        else
            echo -e "  ${RED}✗${NC} $name  (not found)"
        fi
    done

    if [ -d "$MEDIA_ROOT_STAGING" ]; then
        echo ""
        echo -e "${GREEN}✓ Files Staged for Flashing${NC}"
        echo "  Rootfs:  $MEDIA_ROOT_STAGING"
    fi

    if [ -d "$OEM_STAGING" ]; then
        echo ""
        echo -e "${GREEN}✓ Files Staged for OEM Sync${NC}"
        echo "  OEM:     $OEM_STAGING"
    fi
    
    echo ""
}

print_next_steps() {
    echo -e "${BLUE}═══════════════════════════════════════════════════${NC}"
    echo -e "${BLUE}Next Steps:${NC}"
    echo -e "${BLUE}═══════════════════════════════════════════════════${NC}"
    echo ""
    echo "• Rootfs staging: $MEDIA_ROOT_STAGING"
    echo "• OEM staging:    $OEM_STAGING"
    echo ""
    echo "To update on LuckFox Pico Ultra:"
    echo "  1. Full reflash (rootfs + OEM):"
    echo "     cd $PROJECT_ROOT && ./flash.sh"
    echo ""
    echo "  2. Fast OEM sync (seconds, not minutes):"
    echo "     cd $PROJECT_ROOT && ./build.sh sync"
    echo ""
}

##############################################################################
# TESTING STAGES — Interactive Hardware Validation
##############################################################################

print_test_intro() {
    echo ""
    echo -e "${BLUE}═══════════════════════════════════════════════════${NC}"
    echo -e "${BLUE}ULAMA Hardware Smoke Test — Step-by-Step${NC}"
    echo -e "${BLUE}═══════════════════════════════════════════════════${NC}"
    echo ""
    echo "Hardware:"
    echo "  • Host:    WiFi adapter wlx088af1287d57 → mon-host (monitor mode), joystick /dev/input/js0"
    echo "  • LuckFox: WiFi adapter wlan0, UART3 /dev/ttyS3 @ 420000 baud"
    echo ""
    echo "Time: ~30 min for full progression (can skip stages if issues detected)"
    echo ""
}

stage_a_radio_only() {
    echo -e "${YELLOW}┌─────────────────────────────────────────┐${NC}"
    echo -e "${YELLOW}│ STAGE A: Radio-Only Reception           │${NC}"
    echo -e "${YELLOW}└─────────────────────────────────────────┘${NC}"
    echo ""
    echo "Goal: Receive CRSF frames via UNOW without UART3 output"
    echo ""
    echo "PREREQS:"
    echo "  1. LuckFox connected to host network (SSH 192.168.100.1)"
    echo "  2. Both on same WiFi channel (use: iw dev mon0 set channel 6)"
    echo ""
    echo "STEPS:"
    echo ""
    echo "Step 1: On LuckFox, SSH in"
    echo "  ssh root@192.168.100.1"
    echo ""
    echo "Step 2: Check available interfaces"
    echo "  iw dev"
    echo "  Expected: 'Interface wlan0' (or similar)"
    echo ""
    echo "Step 3: Create monitor interface from wlan0"
    echo "  /oem/usr/bin/scripts/unow-mon.sh wlan0 mon0 6"
    echo "  Expected: '✅ Success! Monitor mode is active on mon0'"
    echo "  NOTE: Script takes 3 params: <base-iface> <mon-iface> <channel>"
    echo ""
    echo "Step 4: Start ulamad in debug mode"
    echo "  UNOW_LOG_LEVEL=debug /oem/usr/bin/ulamad \\"
    echo "    --transport unow \\"
    echo "    --iface mon0 \\"
    echo "    --node 1 \\"
    echo "    --output /dev/null \\"
    echo "    --verbose 2>&1 | tee /tmp/ulama_stage_a.log"
    echo ""
    echo "Step 5: On host, put WiFi adapter into monitor mode FIRST"
    echo "  IMPORTANT: Your host adapter is wlx088af1287d57 (NOT wlan0 on host)"
    echo "  Create a virtual monitor interface:"
    echo "    sudo /home/pascale/projects/63411/luxfox/media/unow/scripts/unow-mon.sh wlx088af1287d57 mon-host 6"
    echo "  (or if script not available: sudo iw wlx088af1287d57 interface add mon-host type monitor)"
    echo ""
    echo "Step 5b: Send fixed CRSF pattern from host"
    echo "  cd $ULAMA_ROOT"
    echo "  export LD_LIBRARY_PATH=$UNOW_ROOT/lib:\$LD_LIBRARY_PATH"
    echo "  sudo ./build/host-unow-tools/ulama_js_tx \\"
    echo "    --transport unow \\"
    echo "    --iface mon-host \\"
    echo "    --channel 6 \\"
    echo "    --count 10 \\"
    echo "    --verbose"
    echo ""
    echo "Step 6: Observe LuckFox output"
    echo "  Expected in ulamad log:"
    echo "    'rx_frames: 10, valid: 10, dropped: 0' (or similar success)"
    echo "  If PASS → proceed to Stage B"
    echo "  If FAIL → debug checklist below"
    echo ""
    echo "FAIL DIAGNOSTICS:"
    echo ""
    echo "If no frames received (rx_frames: 0):"
    echo "  1. Check LuckFox monitor interface:"
    echo "     iw dev mon0 link"
    echo "     iw mon0 info"
    echo "     (should show 'type monitor')"
    echo ""
    echo "  2. Check if packets arrive at LuckFox interface (in new terminal):"
    echo "     tcpdump -i mon0 -e 'type mgt subtype action' 2>&1 | head -20"
    echo "     (look for action frames with OUI 00:05:87 = UNOW)"
    echo ""
    echo "  3. Verify host is sending (on host, new terminal):"
    echo "     tcpdump -i mon-host -e 2>&1 | head -20"
    echo "     (should see outgoing packets)"
    echo ""
    echo "  4. Check both on same channel:"
    echo "     LuckFox: iw dev mon0 link | grep 'freq'"
    echo "     Host:    iw dev mon-host link | grep 'freq'"
    echo ""
    echo "  5. Check MAC addresses are valid:"
    echo "     LuckFox: ifconfig mon0 | grep HWaddr"
    echo "     Host:    ifconfig mon-host | grep HWaddr"
    echo "     (should NOT be 00:00:00:00:00:00)"
    echo ""
    echo "If datalink error or no monitor mode:"
    echo "  → Recreate monitor: /oem/usr/bin/scripts/unow-mon.sh wlan0 mon0 6"
    echo "  → Or manually: iw wlan0 interface add mon0 type monitor && iw dev mon0 set channel 6"
    echo ""
}

stage_b_uart_output() {
    echo ""
    echo -e "${YELLOW}┌─────────────────────────────────────────┐${NC}"
    echo -e "${YELLOW}│ STAGE B: UART3 Output (Betaflight)      │${NC}"
    echo -e "${YELLOW}└─────────────────────────────────────────┘${NC}"
    echo ""
    echo "Goal: Write CRSF frames to /dev/ttyS3 @ 420000 baud"
    echo ""
    echo "PREREQS:"
    echo "  ✓ Stage A PASS (radio reception working)"
    echo "  • Betaflight FC connected to LuckFox UART3"
    echo "  • FC powered but NOT armed (safety!)"
    echo ""
    echo "STEPS:"
    echo ""
    echo "Step 1: On LuckFox, verify monitor mode is still up"
    echo "  iw dev mon0 info"
    echo "  Expected: 'type monitor' in output"
    echo ""
    echo "Step 2: On LuckFox, verify UART3"
    echo "  stty -F /dev/ttyS3 -a"
    echo "  Expected: speed 420000 baud (may show 115200, that's OK, ulamad sets it)"
    echo ""
    echo "Step 3: On LuckFox, start ulamad with UART3 output"
    echo "  UNOW_LOG_LEVEL=debug /oem/usr/bin/ulamad \\"
    echo "    --transport unow \\"
    echo "    --iface mon0 \\"
    echo "    --node 1 \\"
    echo "    --uart /dev/ttyS3 \\"
    echo "    --baud 420000 \\"
    echo "    --verbose 2>&1 | tee /tmp/ulama_stage_b.log"
    echo ""
    echo "Step 4: On host, ensure mon-host is in monitor mode, then send 5 frames"
    echo "  (If not already in monitor mode, run in another terminal:)"
    echo "  sudo /home/pascale/projects/63411/luxfox/media/unow/scripts/unow-mon.sh wlx088af1287d57 mon-host 6"
    echo ""
    echo "  Then send frames:"
    echo "  export LD_LIBRARY_PATH=/home/pascale/projects/63411/luxfox/media/unow/lib:\$LD_LIBRARY_PATH"
    echo "  sudo ./build/host-unow-tools/ulama_js_tx \\"
    echo "    --transport unow \\"
    echo "    --iface mon-host \\"
    echo "    --channel 6 \\"
    echo "    --count 5"
    echo ""
    echo "Step 5: Check Betaflight"
    echo "  • Betaflight Configurator → Receiver tab"
    echo "  • Channels should show 1500 (center) or ±values"
    echo "  • If bars not moving: RC link broken"
    echo ""
    echo "Step 6: Verify UART write"
    echo "  On LuckFox, check /tmp/ulama_stage_b.log:"
    echo "    'uart_write: 26 bytes to /dev/ttyS3'"
    echo "  If PASS → proceed to Stage C"
    echo ""
    echo "FAIL DIAGNOSTICS:"
    echo "  ✗ Monitor mode down:"
    echo "    → Recreate: /oem/usr/bin/scripts/unow-mon.sh wlan0 mon0 6"
    echo "  ✗ Betaflight channels not moving:"
    echo "    → Check UART connection: multimeter /dev/ttyS3 pins"
    echo "    → Verify baud rate: 420000"
    echo "    → Check FC RX input: Betaflight CLI 'serial' command"
    echo "  ✗ uart_write errors:"
    echo "    → Device not open? Check permissions"
    echo "    → Baud mismatch? FC expects 420000 CRSF only"
    echo ""
}

stage_c_joystick_live() {
    echo ""
    echo -e "${YELLOW}┌─────────────────────────────────────────┐${NC}"
    echo -e "${YELLOW}│ STAGE C: Live Joystick Input            │${NC}"
    echo -e "${YELLOW}└─────────────────────────────────────────┘${NC}"
    echo ""
    echo "Goal: Read joystick → CRSF → UART3 (full loop)"
    echo ""
    echo "PREREQS:"
    echo "  ✓ Stage B PASS (UART3 output working)"
    echo "  • Joystick connected to host (/dev/input/js0)"
    echo "  • Betaflight FC connected and responsive"
    echo ""
    echo "STEPS:"
    echo ""
    echo "Step 1: On host, verify joystick"
    echo "  ls -la /dev/input/js*"
    echo "  jstest /dev/input/js0"
    echo "  Expected: Axes 0-3, buttons 0+ respond to input"
    echo ""
    echo "Step 2: On LuckFox, verify monitor mode is still up"
    echo "  iw dev mon0 info"
    echo "  Expected: 'type monitor' in output"
    echo "  If down, recreate: /oem/usr/bin/scripts/unow-mon.sh wlan0 mon0 6"
    echo ""
    echo "Step 3: On LuckFox, start ulamad (same as Stage B)"
    echo "  UNOW_LOG_LEVEL=debug /oem/usr/bin/ulamad \\"
    echo "    --transport unow --iface mon0 --node 1 \\"
    echo "    --uart /dev/ttyS3 --baud 420000 --verbose 2>&1"
    echo ""
    echo "Step 4: On host, ensure mon-host is in monitor mode, then start joystick sender"
    echo "  (If not already in monitor mode, run in another terminal:)"
    echo "  sudo /home/pascale/projects/63411/luxfox/media/unow/scripts/unow-mon.sh wlx088af1287d57 mon-host 6"
    echo ""
    echo "  Then start joystick sender:"
    echo "  export LD_LIBRARY_PATH=/home/pascale/projects/63411/luxfox/media/unow/lib:\$LD_LIBRARY_PATH"
    echo "  sudo ./build/host-unow-tools/ulama_js_tx \\"
    echo "    --transport unow \\"
    echo "    --iface mon-host \\"
    echo "    --channel 6 \\"
    echo "    --joystick /dev/input/js0"
    echo ""
    echo "Step 5: Move joystick & observe Betaflight"
    echo "  • Channels in Betaflight Configurator should follow joystick"
    echo "  • Axis 0 (X) → Channel 1 (Roll)"
    echo "  • Axis 1 (Y) → Channel 2 (Pitch)"
    echo "  • Axis 2 (Throttle) → Channel 3"
    echo "  • Axis 3 (Yaw) → Channel 4"
    echo "  • Button 0 (A) → Channel 5 (Arm toggle)"
    echo "  • Button 1+ (B,X,Y) → Channels 6+ (Aux)"
    echo ""
    echo "Step 6: Test arm/disarm"
    echo "  • Press Button 0 to toggle ARM on Channel 5 (900↔2100)"
    echo "  • Check FC shows armed state"
    echo "  • If armed: SLOWLY move throttle stick to verify control"
    echo ""
    echo "✓ STAGE C PASS = ULAMA SMOKE TEST COMPLETE"
    echo ""
    echo "FAIL DIAGNOSTICS:"
    echo "  ✗ Joystick not found:"
    echo "    → Connect USB adapter, check dmesg"
    echo "    → Try different port: /dev/input/js1, js2"
    echo "  ✗ Monitor mode down:"
    echo "    → Recreate: /oem/usr/bin/scripts/unow-mon.sh wlan0 mon0 6"
    echo "  ✗ Channels not responding to joystick:"
    echo "    → Check mapping: values should move 1000-2000 range"
    echo "    → Verify ulama_js_tx is actually sending"
    echo "    → Check logs for errors"
    echo "  ✗ Arm button doesn't toggle:"
    echo "    → Check button mapping in ulama_js_tx code"
    echo "    → Verify button press detected: 'jstest' output"
    echo ""
}

print_test_summary() {
    echo ""
    echo -e "${GREEN}═══════════════════════════════════════════════════${NC}"
    echo -e "${GREEN}Testing Roadmap:${NC}"
    echo -e "${GREEN}═══════════════════════════════════════════════════${NC}"
    echo ""
    echo "Stage A (Radio):     Check UNOW reception on /oem side"
    echo "Stage B (UART):      Verify Betaflight receives RC frames"
    echo "Stage C (Joystick):  Full closed-loop with live input"
    echo ""
    echo "Expected timing:"
    echo "  Stage A: 5-10 min"
    echo "  Stage B: 5-10 min (if Stage A pass)"
    echo "  Stage C: 5-10 min (if Stage B pass)"
    echo ""
    echo "Logs saved to /tmp/ulama_stage_*.log on device"
    echo ""
}

do_test() {
    print_test_intro
    
    echo "Which stage to run?"
    echo "  a) Stage A - Radio only"
    echo "  b) Stage B - UART output"
    echo "  c) Stage C - Joystick live"
    echo "  all) All stages (full progression)"
    echo "  summary) Show roadmap only"
    echo ""
    read -p "Choice [a/b/c/all/summary]: " choice
    
    case "$choice" in
        a|A) stage_a_radio_only ;;
        b|B) stage_b_uart_output ;;
        c|C) stage_c_joystick_live ;;
        all|ALL)
            stage_a_radio_only
            echo ""
            read -p "Stage A done. Press Enter for Stage B..."
            stage_b_uart_output
            echo ""
            read -p "Stage B done. Press Enter for Stage C..."
            stage_c_joystick_live
            ;;
        summary|SUMMARY)
            print_test_summary
            ;;
        *)
            log_error "Unknown choice: $choice"
            return 1
            ;;
    esac
    
    print_test_summary
}

##############################################################################
# MAIN
##############################################################################
main() {
    local COMMAND="${1:-build}"
    
    # Handle help first
    if [[ "$COMMAND" == "-h" || "$COMMAND" == "--help" ]]; then
        print_usage
        exit 0
    fi
    
    shift 2>/dev/null || true
    
    # Parse arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                print_usage
                exit 0
                ;;
            -c|--clean)
                CLEAN=1
                shift
                ;;
            -v|--verbose)
                VERBOSE=1
                shift
                ;;
            *)
                log_error "Unknown option: $1"
                print_usage
                exit 1
                ;;
        esac
    done
    
    log_debug "COMMAND=$COMMAND"
    log_debug "CLEAN=$CLEAN"
    log_debug "VERBOSE=$VERBOSE"
    
    case "$COMMAND" in
        build)
            # Clean if requested
            if [ $CLEAN -eq 1 ]; then
                do_clean
            fi

            if ! do_build; then
                log_error "Build failed!"
                return 1
            fi

            if ! stage_files; then
                log_error "Failed to stage files for flashing!"
                return 1
            fi

            print_summary
            print_next_steps
            return 0
            ;;
        test)
            do_test
            return $?
            ;;
        *)
            log_error "Unknown command: $COMMAND"
            echo ""
            echo "Usage: $0 [COMMAND] [OPTIONS]"
            echo ""
            echo "Commands:"
            echo "  build                Build and stage files (default)"
            echo "  test                 Interactive hardware smoke test"
            echo ""
            echo "Options:"
            echo "  -h, --help           Show help"
            echo "  -c, --clean          Clean before build"
            echo "  -v, --verbose        Verbose output"
            return 1
            ;;
    esac
}

# Run main
main "$@"
