#!/usr/bin/env bash
# run_scheduler_experiment.sh — Scheduler Experiment Harness
#
# Runs controlled experiments to compare Linux CFS scheduling behaviour
# across different workload types and nice-value configurations.
#
# Experiments:
#   1. Two CPU-bound containers at different nice values (fairness / priority)
#   2. A CPU-bound and an I/O-bound container running concurrently (CFS interaction)
#
# Prerequisites:
#   - supervisor is already running: sudo ./engine supervisor ./rootfs-base
#   - workload binaries have been copied into rootfs:
#       cp cpu_test    ./rootfs-alpha/
#       cp cpu_test    ./rootfs-beta/
#       cp workload_io ./rootfs-gamma/
#   - cp -a ./rootfs-base ./rootfs-alpha
#   - cp -a ./rootfs-base ./rootfs-beta
#   - cp -a ./rootfs-base ./rootfs-gamma
#
# Output:
#   Results are printed to stdout and also appended to scheduler_results.txt

set -euo pipefail

ENGINE="sudo ./engine"
DURATION=30   # workload duration in seconds for each experiment
RESULTS="scheduler_results.txt"

echo "======================================================" | tee -a "$RESULTS"
echo "Scheduler Experiment — $(date)"                         | tee -a "$RESULTS"
echo "Kernel: $(uname -r)"                                    | tee -a "$RESULTS"
echo "CPUs: $(nproc)"                                         | tee -a "$RESULTS"
echo "======================================================" | tee -a "$RESULTS"

# ─── Helper: wait for a container to reach 'stopped' state ────────────────
wait_stopped() {
    local id="$1"
    local timeout=120
    local elapsed=0
    while [ $elapsed -lt $timeout ]; do
        state=$($ENGINE ps 2>/dev/null | awk -v id="$id" '$1 == id {print $3}')
        [ "$state" = "stopped" ] && return 0
        sleep 1
        elapsed=$((elapsed + 1))
    done
    echo "WARNING: container '$id' did not stop within ${timeout}s" >&2
    return 1
}

# ─── Experiment 1: Two CPU-bound containers, different nice values ─────────
echo ""                                                         | tee -a "$RESULTS"
echo "--- Experiment 1: CPU-bound (nice=0) vs CPU-bound (nice=19) ---" | tee -a "$RESULTS"
echo "    Both run /cpu_test $DURATION seconds simultaneously"  | tee -a "$RESULTS"
echo ""                                                         | tee -a "$RESULTS"

# Start both at (nearly) the same time
T0=$(date +%s%3N)
$ENGINE start hi_cpu ./rootfs-alpha /cpu_test $DURATION
$ENGINE start lo_cpu ./rootfs-beta  /cpu_test $DURATION --nice 19
T_LAUNCHED=$(date +%s%3N)

echo "Both launched at T+$((T_LAUNCHED - T0)) ms" | tee -a "$RESULTS"

# Poll CPU usage every 2 seconds while they run
echo ""
echo "time_s  hi_cpu_%  lo_cpu_%  note" | tee -a "$RESULTS"
HI_PID=$($ENGINE ps 2>/dev/null | awk '$1=="hi_cpu"{print $2}')
LO_PID=$($ENGINE ps 2>/dev/null | awk '$1=="lo_cpu"{print $2}')

for tick in $(seq 2 2 $((DURATION - 2))); do
    sleep 2
    HI_PCT=$(ps -p "$HI_PID" -o %cpu --no-headers 2>/dev/null | tr -d ' ' || echo "0")
    LO_PCT=$(ps -p "$LO_PID" -o %cpu --no-headers 2>/dev/null | tr -d ' ' || echo "0")
    printf "%-7s %-9s %-9s\n" "$tick" "${HI_PCT}" "${LO_PCT}" | tee -a "$RESULTS"
done

# Record end times
T_HI_DONE=$(wait_stopped hi_cpu && date +%s%3N)
T_LO_DONE=$(wait_stopped lo_cpu && date +%s%3N)

echo "" | tee -a "$RESULTS"
echo "hi_cpu (nice=0)  finished in $((T_HI_DONE - T0)) ms" | tee -a "$RESULTS"
echo "lo_cpu (nice=19) finished in $((T_LO_DONE - T0)) ms" | tee -a "$RESULTS"

echo ""                                                         | tee -a "$RESULTS"
echo "Expected: hi_cpu should consume significantly more CPU share than lo_cpu" | tee -a "$RESULTS"
echo "          due to its lower nice value giving it higher CFS weight."       | tee -a "$RESULTS"

# ─── Experiment 2: CPU-bound vs I/O-bound at same nice value ───────────────
echo ""                                                         | tee -a "$RESULTS"
echo "--- Experiment 2: CPU-bound (nice=0) vs I/O-bound (nice=0) ---" | tee -a "$RESULTS"
echo "    Both run for $DURATION seconds"                       | tee -a "$RESULTS"
echo ""                                                         | tee -a "$RESULTS"

T0=$(date +%s%3N)
$ENGINE start cpu_exp ./rootfs-alpha /cpu_test $DURATION
$ENGINE start io_exp  ./rootfs-gamma /workload_io $DURATION
T_LAUNCHED=$(date +%s%3N)

CPU_EXP_PID=$($ENGINE ps 2>/dev/null | awk '$1=="cpu_exp"{print $2}')
IO_EXP_PID=$($ENGINE ps  2>/dev/null | awk '$1=="io_exp"{print $2}')

echo "time_s  cpu_exp_%  io_exp_%  note" | tee -a "$RESULTS"
for tick in $(seq 2 2 $((DURATION - 2))); do
    sleep 2
    CPU_PCT=$(ps -p "$CPU_EXP_PID" -o %cpu --no-headers 2>/dev/null | tr -d ' ' || echo "0")
    IO_PCT=$(ps  -p "$IO_EXP_PID"  -o %cpu --no-headers 2>/dev/null | tr -d ' ' || echo "0")
    printf "%-7s %-10s %-9s\n" "$tick" "${CPU_PCT}" "${IO_PCT}" | tee -a "$RESULTS"
done

wait_stopped cpu_exp || true
wait_stopped io_exp  || true

echo ""                                                         | tee -a "$RESULTS"
echo "Expected: cpu_exp uses near 100% CPU; io_exp uses very little CPU"  | tee -a "$RESULTS"
echo "          (mostly in D/S state, blocked on fsync).  CFS correctly"  | tee -a "$RESULTS"
echo "          gives spare cycles to cpu_exp while io_exp sleeps."        | tee -a "$RESULTS"

# ─── Print final ps snapshot ───────────────────────────────────────────────
echo ""                                                         | tee -a "$RESULTS"
echo "--- Final container table ---"                            | tee -a "$RESULTS"
$ENGINE ps 2>/dev/null | tee -a "$RESULTS"

echo ""                                                         | tee -a "$RESULTS"
echo "Results saved to $RESULTS"
