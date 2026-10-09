#!/bin/bash
#
# A510 MCU FWU stress harness.
#
#   tests/fwu_stress.sh <iterations> <app.bin> <fwu_sram.bin> [results_dir] [resume_from]
#
# Each iteration runs one full FWU (REQUEST -> BL -> FWU_SRAM -> app -> REBOOT)
# and records the outcome plus host health around it, so a failure can be tied
# to a specific iteration instead of "it hung at some point".
#
# Env:
#   BACKEND=smbus|ch347   platform_i2c_driver backend to build/use (default smbus)
#   A510_BDF=05:00.0      PCIe BDF of the card; when set, the link speed and the
#                         PCIe error counter are recorded before/after each run
#   A510_MAX_SECONDS=120  kill a single iteration that takes longer than this
#   A510_EXPECT_HASH=...  expected GIT_HASH after the update (default: current)
#
# Results land in <results_dir>/ (default ./fwu_stress_results):
#   results.csv      one row per iteration, header first
#   iter_<n>.log     full tool output of that iteration
#   state            iteration counter, so a run can resume after a reboot
#
# Exit: 0 all iterations PASS, 1 at least one FAIL, 2 harness error.
set -u

ITERATIONS=${1:?usage: fwu_stress.sh <iterations> <app.bin> <fwu_sram.bin> [results_dir] [resume_from]}
APP_BIN=${2:?usage: fwu_stress.sh <iterations> <app.bin> <fwu_sram.bin> [results_dir] [resume_from]}
FWU_SRAM=${3:?usage: fwu_stress.sh <iterations> <app.bin> <fwu_sram.bin> [results_dir] [resume_from]}
RESULTS=${4:-fwu_stress_results}
RESUME=${5:-0}

BACKEND=${BACKEND:-smbus}
BDF=${A510_BDF:-}
MAX_SECONDS=${A510_MAX_SECONDS:-120}
EXPECT_HASH=${A510_EXPECT_HASH:-}

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 2

SRC="platform_i2c_driver/$BACKEND/platform_i2c_driver.c"
[ -f "$SRC" ] || { echo "no such backend: $SRC" >&2; exit 2; }

echo "== building ($BACKEND backend) =="
gcc -O2 -o fwu_host_demo main.c image.c "$SRC" -Iplatform_i2c_driver || exit 2
gcc -O2 -o mcu_status tests/mcu_status.c "$SRC" -Iplatform_i2c_driver || exit 2

mkdir -p "$RESULTS"
CSV="$RESULTS/results.csv"
[ -f "$CSV" ] || echo "iter,start,duration_s,exit,device_id,boot_state,app,git_hash,alert,total_power,junction,aer_before,aer_after,link_before,link_after,uptime_before,uptime_after,result,notes" > "$CSV"

# ---- host health probes (optional) ------------------------------------------
pcie_errors() {
    [ -n "$BDF" ] || { echo "n/a"; return; }
    journalctl -b -k --no-pager 2>/dev/null | grep -c "PCIe Bus Error" || echo 0
}

link_speed() {
    [ -n "$BDF" ] || { echo "n/a"; return; }
    lspci -vv -s "$BDF" 2>/dev/null | sed -n 's/.*LnkSta:[^S]*Speed \([0-9.]*GT\/s\).*/\1/p' | head -1
}

uptime_s() { cut -d. -f1 /proc/uptime; }

status_field() { grep -m1 "^$2=" "$1" | cut -d= -f2; }

# ---- main loop ---------------------------------------------------------------
fail=0
start_iter=$((RESUME + 1))
end_iter=$((RESUME + ITERATIONS))

for i in $(seq "$start_iter" "$end_iter"); do
    log="$RESULTS/iter_${i}.log"
    pre_state="$RESULTS/.pre_${i}"
    start_iso=$(date '+%F %T')
    aer_before=$(pcie_errors); link_before=$(link_speed); up_before=$(uptime_s)

    ./mcu_status > "$pre_state" 2>&1
    pre_ok=$?

    t0=$(date +%s.%N)
    timeout "$MAX_SECONDS" ./fwu_host_demo "$APP_BIN" "$FWU_SRAM" > "$log" 2>&1
    rc=$?
    t1=$(date +%s.%N)
    duration=$(awk "BEGIN{printf \"%.1f\", $t1-$t0}")

    sleep 1
    post_state="$RESULTS/.post_${i}"
    ./mcu_status > "$post_state" 2>&1
    post_rc=$?

    aer_after=$(pcie_errors); link_after=$(link_speed); up_after=$(uptime_s)

    device_id=$(status_field "$post_state" device_id)
    boot_state=$(status_field "$post_state" boot_state)
    app=$(status_field "$post_state" app)
    git_hash=$(status_field "$post_state" git_hash)
    alert=$(status_field "$post_state" alert_status)
    power=$(status_field "$post_state" total_power)
    junction=$(status_field "$post_state" junction_temp)

    result=PASS
    notes=""
    [ "$rc" -eq 0 ] || { result=FAIL; notes="tool exit=$rc"; }
    [ "$post_rc" -eq 0 ] || { result=FAIL; notes="${notes:+$notes; }mcu not APP (rc=$post_rc)"; }
    [ "$app" = "1" ] || { result=FAIL; notes="${notes:+$notes; }boot_state=$boot_state"; }
    [ -z "$EXPECT_HASH" ] || [ "$git_hash" = "$EXPECT_HASH" ] || {
        result=FAIL; notes="${notes:+$notes; }git_hash=$git_hash != $EXPECT_HASH"; }
    [ "$up_after" -ge "$up_before" ] 2>/dev/null || {
        result=FAIL; notes="${notes:+$notes; }host rebooted during iteration"; }
    if [ "$rc" -eq 124 ]; then
        result=FAIL; notes="${notes:+$notes; }timeout after ${MAX_SECONDS}s"
    fi
    if [ -n "$BDF" ] && [ "$aer_before" != "n/a" ] && [ "$aer_after" -gt "$aer_before" ]; then
        notes="${notes:+$notes; }PCIe errors +$((aer_after - aer_before))"
        [ "$result" = PASS ] && result=WARN
    fi
    if [ -n "$BDF" ] && [ "$link_after" != "$link_before" ]; then
        notes="${notes:+$notes; }link $link_before -> $link_after"
        [ "$result" = PASS ] && result=WARN
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"%s"\n' \
        "$i" "$start_iso" "$duration" "$rc" "$device_id" "$boot_state" "$app" \
        "$git_hash" "$alert" "$power" "$junction" "$aer_before" "$aer_after" \
        "$link_before" "$link_after" "$up_before" "$up_after" "$result" "$notes" >> "$CSV"

    echo "$i" > "$RESULTS/state"
    echo "iter $i: $result  ${duration}s  rc=$rc  app=$app  hash=$git_hash  ${notes}"
    rm -f "$pre_state" "$post_state"

    [ "$result" = FAIL ] && fail=1
done

echo
echo "== summary ($RESULTS/results.csv) =="
echo "iterations: $start_iter..$end_iter"
awk -F, 'NR>1 {c[$18]++} END {for (k in c) printf "  %-6s %d\n", k, c[k]}' "$CSV"
exit $fail
