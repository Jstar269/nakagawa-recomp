#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
#
# Boot #11 transport qualification.  Launches no PRX, ever.
#
# Modes:
#
#   (default)               host-side only. Reads process, USB, session and staging
#                           state. Sends nothing to the device. Safe to run while
#                           the boot is being held fresh.
#   --round-trip            additionally performs live device round-trip checks:
#                           1) shell channel: "help module" -> requires prompt echo
#                              host0:/>, module help containing ldstart, pspsh
#                              execution, and no ldstart or exit command sent.
#                           2) hostfs channel: "ls host0:/" -> requires prompt echo
#                              and frozen staged FPU filename visible in the listing.
#   --test-shell-input F    evaluates shell-channel qualification against file F.
#   --test-host0-input F    evaluates hostfs-channel qualification against file F.
#
# `pspsh exit` is never issued by this script under any mode.
# No PRX/ELF is launched or rebuilt.

set -u

if ! command -v pspsh >/dev/null 2>&1; then
    if [ -x /home/jstar/pspdev/bin/pspsh ]; then
        PATH="/home/jstar/pspdev/bin:$PATH"
        export PATH
    fi
fi

STAGING="${STAGING:-/tmp/psplink-host0}"
MANIFEST="${MANIFEST:-/mnt/c/nk/reports/psp-hardware-gap-closure-2026-09-03/EVIDENCE_MANIFEST_BOOT10_FINAL.sha256}"
FPU_NAME="probe_fpu_vector.prx"
ROUND_TRIP=0
TEST_SHELL_INPUT=""
TEST_HOST0_INPUT=""

while [ $# -gt 0 ]; do
    case "$1" in
        --round-trip)
            ROUND_TRIP=1
            shift
            ;;
        --test-shell-input)
            TEST_SHELL_INPUT="$2"
            shift 2
            ;;
        --test-host0-input)
            TEST_HOST0_INPUT="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

FAILED=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; FAILED=$((FAILED + 1)); }
info() { echo "INFO  $1"; }

# ---- Shell channel round-trip validator ----------------------------------
# Returns 0 on PASS, 1 on FAIL.
check_shell_round_trip() {
    _raw="$1"
    # 1. PSPLink prompt host0:/> must be present
    if ! printf '%s' "$_raw" | grep -q 'host0:/>'; then
        echo "      missing PSPLink prompt 'host0:/>'" >&2
        return 1
    fi
    # 2. returned module help must contain ldstart
    if ! printf '%s' "$_raw" | grep -qi 'ldstart'; then
        echo "      returned module help does not contain 'ldstart'" >&2
        return 1
    fi
    # 3. command was actually sent through pspsh (must see Category module)
    if ! printf '%s' "$_raw" | grep -qi 'Category module'; then
        echo "      missing 'Category module' banner from pspsh" >&2
        return 1
    fi
    # 4. no ldstart command itself was sent (fail closed if a program was launched)
    if printf '%s' "$_raw" | grep -E -qi '^[[:space:]]*ldstart([[:space:]]+[^-[:space:]]|$)'; then
        echo "      forbidden ldstart execution command found in capture" >&2
        return 1
    fi
    # 5. no exit command was sent
    if printf '%s' "$_raw" | grep -E -qi '^[[:space:]]*(pspsh[[:space:]]+)?exit[[:space:]]*$'; then
        echo "      forbidden exit command found in capture" >&2
        return 1
    fi
    return 0
}

# ---- Hostfs channel round-trip validator ---------------------------------
# Returns 0 on PASS, 1 on FAIL.
check_host0_round_trip() {
    _raw="$1"
    _expected_fpu="${2:-probe_fpu_vector.prx}"
    # 1. PSPLink prompt host0:/> must be present
    if ! printf '%s' "$_raw" | grep -q 'host0:/>'; then
        echo "      missing PSPLink prompt 'host0:/>'" >&2
        return 1
    fi
    # 2. Listing directory confirmation
    if ! printf '%s' "$_raw" | grep -qi 'Listing directory host0:/'; then
        echo "      missing directory listing banner 'Listing directory host0:/'" >&2
        return 1
    fi
    # 3. Frozen staged FPU filename visible through PSP-side host0 filesystem
    if ! printf '%s' "$_raw" | grep -q "$_expected_fpu"; then
        echo "      staged FPU filename '$_expected_fpu' not visible in host0 listing" >&2
        return 1
    fi
    # 4. no exit command sent
    if printf '%s' "$_raw" | grep -E -qi '^[[:space:]]*(pspsh[[:space:]]+)?exit[[:space:]]*$'; then
        echo "      forbidden exit command found in capture" >&2
        return 1
    fi
    return 0
}

# Offline test mode if input files are passed
if [ -n "$TEST_SHELL_INPUT" ] || [ -n "$TEST_HOST0_INPUT" ]; then
    _test_fail=0
    if [ -n "$TEST_SHELL_INPUT" ]; then
        _s_content=$(cat "$TEST_SHELL_INPUT")
        if check_shell_round_trip "$_s_content"; then
            echo "SHELL_ROUND_TRIP = PASS"
        else
            echo "SHELL_ROUND_TRIP = FAIL"
            _test_fail=$((_test_fail + 1))
        fi
    fi
    if [ -n "$TEST_HOST0_INPUT" ]; then
        _h_content=$(cat "$TEST_HOST0_INPUT")
        if check_host0_round_trip "$_h_content" "$FPU_NAME"; then
            echo "HOST0_ROUND_TRIP = PASS"
        else
            echo "HOST0_ROUND_TRIP = FAIL"
            _test_fail=$((_test_fail + 1))
        fi
    fi
    if [ -n "$TEST_SHELL_INPUT" ] && [ -n "$TEST_HOST0_INPUT" ]; then
        if [ "$_test_fail" -eq 0 ]; then
            echo "TRANSPORT_QUALIFIED = YES"
            exit 0
        else
            echo "TRANSPORT_QUALIFIED = NO"
            exit 1
        fi
    fi
    if [ "$_test_fail" -eq 0 ]; then
        exit 0
    else
        exit 1
    fi
fi

echo "== Boot #11 transport qualification (no PRX launch) =="

# ---- 1. exactly one current usbhostfs_pc ---------------------------------
count=$(pgrep -c -x usbhostfs_pc 2>/dev/null || echo 0)
if [ "$count" -eq 1 ]; then
    pass "exactly one usbhostfs_pc running"
    pgrep -a -x usbhostfs_pc | sed 's/^/      /'
elif [ "$count" -eq 0 ]; then
    fail "no usbhostfs_pc running (start exactly one against $STAGING)"
else
    fail "$count usbhostfs_pc processes running (must be exactly one)"
    pgrep -a -x usbhostfs_pc | sed 's/^/      /'
fi

# ---- 2. session freshness -------------------------------------------------
vm_boot=$(uptime -s 2>/dev/null || echo unknown)
info "WSL VM boot time      : $vm_boot"
if [ "$count" -eq 1 ]; then
    pid=$(pgrep -x usbhostfs_pc | head -1)
    hostfs_start=$(ps -o lstart= -p "$pid" 2>/dev/null | sed 's/^ *//')
    info "usbhostfs_pc started  : $hostfs_start (pid $pid)"
fi
if [ -d "$STAGING" ]; then
    info "staging root mtime    : $(stat -c '%y' "$STAGING" 2>/dev/null)"
fi
echo "      A usbhostfs_pc older than the current WSL VM, or staging older than"
echo "      the current VM boot, is NOT current: reattach USB, recreate the"
echo "      host0 root, restage and re-hash all five binaries before launching."

# ---- 3. PSP visible on the bus -------------------------------------------
if lsusb 2>/dev/null | grep -q '054c:01c9'; then
    pass "PSP enumerated as 054c:01c9"
    lsusb | grep '054c:01c9' | sed 's/^/      /'
else
    fail "PSP 054c:01c9 not visible to this WSL VM"
    echo "      On the Windows host: usbipd list  (device must show Attached, not"
    echo "      merely Shared), then: usbipd attach --wsl --busid <busid>"
fi

# ---- 4. keepalive ---------------------------------------------------------
if pgrep -f 'boot11-keepalive' >/dev/null 2>&1; then
    pass "WSL keepalive process present"
else
    fail "no WSL keepalive process (the VM can idle out between launches)"
    echo "      Start one, e.g.:  nohup sh -c 'exec -a boot11-keepalive sleep 36000' >/dev/null 2>&1 &"
fi

# ---- 5. host0 root access and staged digests -----------------------------
if [ -d "$STAGING" ] && [ -r "$STAGING" ] && [ -w "$STAGING" ]; then
    pass "host0 root readable and writable at $STAGING"
else
    fail "host0 root $STAGING missing or not accessible"
fi

if [ -f "$MANIFEST" ]; then
    ok=0
    bad=0
    for prx in "$STAGING"/*.prx; do
        [ -e "$prx" ] || continue
        actual=$(sha256sum "$prx" | cut -d' ' -f1)
        if grep -qi "^$actual" "$MANIFEST"; then
            ok=$((ok + 1))
        else
            bad=$((bad + 1))
            echo "      UNKNOWN DIGEST: $prx $actual"
        fi
    done
    if [ "$ok" -eq 5 ] && [ "$bad" -eq 0 ]; then
        pass "staged binaries 5/5 match the frozen Boot #10 digest manifest"
    else
        fail "staged binaries: $ok matched, $bad unmatched (expected 5 matched, 0 unmatched)"
    fi
else
    fail "frozen digest manifest not found at $MANIFEST"
fi

# ---- 6. stale device log --------------------------------------------------
if [ -e "$STAGING/fpu_vector_log.txt" ]; then
    fail "stale $STAGING/fpu_vector_log.txt present (the probe appends to it)"
    echo "      Run boot11_clear_stale_host0_log.sh before Launch 1."
else
    pass "no stale fpu_vector_log.txt in the staging root"
fi

# ---- 7. device round-trip (opt-in; talks to the device, launches nothing) --
SHELL_ROUND_TRIP="NOT_RUN"
HOST0_ROUND_TRIP="NOT_RUN"

if [ "$ROUND_TRIP" -eq 1 ]; then
    echo "INFO  performing shell round-trip (help module; no ldstart, no exit)"
    shell_out=$({ echo "help module"; sleep 4; } | pspsh 2>&1)
    printf '%s\n' "$shell_out" | sed 's/^/      /'
    if check_shell_round_trip "$shell_out"; then
        pass "shell round-trip: prompt echo AND module command listing (ldstart) returned"
        SHELL_ROUND_TRIP="PASS"
    else
        fail "shell round-trip incomplete or invalid (banner without device response is NOT qualification)"
        SHELL_ROUND_TRIP="FAIL"
    fi

    echo "INFO  performing hostfs round-trip (ls host0:/; looking for $FPU_NAME)"
    host0_out=$({ echo "ls host0:/"; sleep 4; } | pspsh 2>&1)
    printf '%s\n' "$host0_out" | sed 's/^/      /'
    if check_host0_round_trip "$host0_out" "$FPU_NAME"; then
        pass "host0 round-trip: PSP-side host0 listing shows staged $FPU_NAME"
        HOST0_ROUND_TRIP="PASS"
    else
        fail "host0 round-trip incomplete or missing staged FPU binary"
        HOST0_ROUND_TRIP="FAIL"
    fi
else
    info "device round-trip SKIPPED (host-side-only mode); re-run with --round-trip"
    info "immediately before Launch 1"
fi

echo
if [ "$ROUND_TRIP" -eq 1 ]; then
    echo "SHELL_ROUND_TRIP = $SHELL_ROUND_TRIP"
    echo "HOST0_ROUND_TRIP = $HOST0_ROUND_TRIP"
fi

if [ "$FAILED" -eq 0 ]; then
    echo "TRANSPORT_QUALIFIED = YES"
    exit 0
fi
echo "TRANSPORT_QUALIFIED = NO ($FAILED condition(s) failed)"
exit 1
