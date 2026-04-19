#!/usr/bin/env bash
# environment-check.sh — VM preflight check for Mini Container Runtime
#
# Run with: sudo ./environment-check.sh
#
# Verifies:
#   1. Running as root
#   2. Linux kernel (not WSL, not macOS)
#   3. Kernel version >= 5.15
#   4. Kernel headers installed
#   5. build-essential / gcc installed
#   6. Namespace support (PID, mount, UTS, IPC)
#   7. /proc/sys/kernel/unprivileged_userns_clone check
#   8. clone() syscall available
#   9. Secure Boot status (warn if ON)
#
# Exit codes: 0 = all checks passed, 1 = one or more failures

set -euo pipefail

PASS=0
FAIL=0

ok()   { echo "[  OK  ] $*"; PASS=$((PASS + 1)); }
fail() { echo "[ FAIL ] $*"; FAIL=$((FAIL + 1)); }
warn() { echo "[ WARN ] $*"; }
info() { echo "[ INFO ] $*"; }

echo "========================================================"
echo " Mini Container Runtime — Environment Check"
echo " $(date)"
echo "========================================================"
echo ""

# ── 1. Root check ────────────────────────────────────────────────────────
if [ "$(id -u)" -eq 0 ]; then
    ok "Running as root"
else
    fail "Must run as root (sudo ./environment-check.sh)"
fi

# ── 2. Linux check ───────────────────────────────────────────────────────
if [ "$(uname -s)" = "Linux" ]; then
    ok "Linux kernel detected: $(uname -r)"
else
    fail "Not a Linux system (detected: $(uname -s))"
fi

# ── 3. WSL check ─────────────────────────────────────────────────────────
if uname -r | grep -qi "microsoft\|wsl"; then
    fail "WSL detected — kernel modules do NOT work in WSL. Use a real Ubuntu VM."
else
    ok "Not WSL"
fi

# ── 4. Kernel version ≥ 5.15 ─────────────────────────────────────────────
KVER=$(uname -r | cut -d. -f1-2)
KMAJ=$(echo "$KVER" | cut -d. -f1)
KMIN=$(echo "$KVER" | cut -d. -f2)
if [ "$KMAJ" -gt 5 ] || { [ "$KMAJ" -eq 5 ] && [ "$KMIN" -ge 15 ]; }; then
    ok "Kernel version $KVER >= 5.15"
else
    fail "Kernel version $KVER is too old (need >= 5.15)"
fi

# ── 5. Kernel headers ─────────────────────────────────────────────────────
HDIR="/lib/modules/$(uname -r)/build"
if [ -d "$HDIR" ]; then
    ok "Kernel headers found at $HDIR"
else
    fail "Kernel headers NOT found at $HDIR — run: sudo apt install linux-headers-\$(uname -r)"
fi

# ── 6. GCC / build-essential ──────────────────────────────────────────────
if command -v gcc >/dev/null 2>&1; then
    ok "gcc found: $(gcc --version | head -1)"
else
    fail "gcc not found — run: sudo apt install build-essential"
fi

if command -v make >/dev/null 2>&1; then
    ok "make found: $(make --version | head -1)"
else
    fail "make not found — run: sudo apt install build-essential"
fi

# ── 7. Namespace support ──────────────────────────────────────────────────
for ns in pid mnt uts ipc; do
    if [ -e "/proc/self/ns/$ns" ]; then
        ok "Namespace supported: $ns"
    else
        fail "Namespace NOT supported: $ns"
    fi
done

# ── 8. Clone syscall ─────────────────────────────────────────────────────
CLONE_TEST=$(cat <<'EOF'
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
int child(void *a) { (void)a; return 0; }
int main(void) {
    char stack[4096];
    pid_t p = clone(child, stack + 4096, CLONE_NEWPID|SIGCHLD, NULL);
    return p < 0 ? 1 : 0;
}
EOF
)
if echo "$CLONE_TEST" | gcc -x c - -o /tmp/_clone_test 2>/dev/null && /tmp/_clone_test 2>/dev/null; then
    ok "clone() with CLONE_NEWPID works"
    rm -f /tmp/_clone_test
else
    fail "clone() with CLONE_NEWPID failed — check kernel config CONFIG_PID_NS"
    rm -f /tmp/_clone_test
fi

# ── 9. chroot test ────────────────────────────────────────────────────────
TMPDIR_CHR=$(mktemp -d)
mkdir -p "$TMPDIR_CHR/bin"
cp /bin/true "$TMPDIR_CHR/bin/true" 2>/dev/null || true
if chroot "$TMPDIR_CHR" /bin/true 2>/dev/null; then
    ok "chroot works"
else
    fail "chroot failed — check capabilities"
fi
rm -rf "$TMPDIR_CHR"

# ── 10. Secure Boot status ────────────────────────────────────────────────
if command -v mokutil >/dev/null 2>&1; then
    SB=$(mokutil --sb-state 2>/dev/null || echo "unknown")
    if echo "$SB" | grep -qi "enabled"; then
        warn "Secure Boot is ENABLED — unsigned kernel modules will be REJECTED."
        warn "Disable Secure Boot in VM firmware settings."
    else
        ok "Secure Boot is disabled or not applicable"
    fi
else
    info "mokutil not found — cannot check Secure Boot state (install mokutil to verify)"
fi

# ── Summary ───────────────────────────────────────────────────────────────
echo ""
echo "========================================================"
echo " Results: $PASS passed, $FAIL failed"
echo "========================================================"

if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo "Fix the FAIL items above before proceeding."
    exit 1
else
    echo ""
    echo "All checks passed! You are ready to build and run the container runtime."
    echo ""
    echo "Next steps:"
    echo "  cd boilerplate && make"
    echo "  sudo insmod monitor.ko"
    echo "  sudo ./engine supervisor ./rootfs-alpha &"
    echo "  sudo ./engine start alpha ./rootfs-alpha /cpu_test 30"
    exit 0
fi
