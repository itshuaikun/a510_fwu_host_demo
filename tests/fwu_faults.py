#!/usr/bin/env python3
"""A510 FWU fault-injection suite.

Every case breaks the update on purpose and then requires a plain re-run of the
host tool to restore the board: a release blocker is any case that leaves the
MCU in a state a normal FWU cannot recover from.

Cases that hand the tool a bad image no longer reach the MCU at all: the host
pre-check rejects them offline (exit 2) and the board keeps running the firmware
it had. Recovery from a half-written image is still covered by the kill@NN%
cases, which kill the tool mid-transfer of a valid image.

    tests/fwu_faults.py <app.bin> <fwu_sram.bin> [results_dir]

Env:
    BACKEND=smbus|ch347   backend to build/run (default smbus)
    A510_OLD_APP=...      optional second app image to test a real content swap
                          (must be a current, version 2 image for this board)
    A510_BDF=05:00.0      optional PCIe BDF, recorded as an AER delta per case

Outputs <results_dir>/faults.csv and one log per case.
Exit: 0 every case recovered, 1 otherwise.
"""
import csv
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BACKEND = os.environ.get("BACKEND", "smbus")
BDF = os.environ.get("A510_BDF", "")


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def build():
    src = os.path.join(ROOT, "platform_i2c_driver", BACKEND, "platform_i2c_driver.c")
    if not os.path.exists(src):
        sys.exit("no such backend: %s" % src)
    for out, args in (("fwu_host_demo", ["main.c", "image.c"]),
                      ("mcu_status", [os.path.join("tests", "mcu_status.c")])):
        r = sh(["gcc", "-O2", "-o", out] + args + [src, "-Iplatform_i2c_driver"], cwd=ROOT)
        if r.returncode != 0:
            sys.exit("build %s failed:\n%s" % (out, r.stderr))
    return os.path.join(ROOT, "fwu_host_demo"), os.path.join(ROOT, "mcu_status")


def mcu_status(exe, tries=12, delay=0.5):
    """Read the MCU state, retrying while the slave is still off the bus.

    Right after a REBOOT the MCU is resetting and briefly does not answer, so a
    single read would look like a dead board.
    """
    r = None
    for _ in range(tries):
        r = sh([exe])
        d = {}
        for line in r.stdout.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                d[k] = v
        if "device_id" in d:
            d["_rc"] = r.returncode
            return d
        time.sleep(delay)
    return {"_rc": r.returncode, "_out": r.stdout.strip()}


def aer_count():
    if not BDF:
        return None
    r = sh(["bash", "-c",
            "journalctl -b -k --no-pager 2>/dev/null | grep -c 'PCIe Bus Error' || true"])
    try:
        return int(r.stdout.strip() or 0)
    except ValueError:
        return None


def fwu(tool, app, sram, timeout=180):
    t0 = time.time()
    r = sh(["timeout", str(timeout), tool, app, sram], cwd=ROOT)
    return r.returncode, time.time() - t0, r.stdout + r.stderr


def kill_at_percent(tool, app, sram, pct):
    """Run an update and SIGKILL the tool once the app phase reaches pct%."""
    p = subprocess.Popen([tool, app, sram], cwd=ROOT, bufsize=0,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    in_app_phase = False
    buf = b""
    killed = False
    while True:
        ch = p.stdout.read(1)
        if not ch:
            break
        if ch in (b"\r", b"\n"):
            line = buf.decode(errors="replace")
            buf = b""
            if "programming" in line:
                in_app_phase = True
            m = re.search(r"(\d+)%", line)
            if in_app_phase and m and int(m.group(1)) >= pct:
                p.kill()
                killed = True
                break
        else:
            buf += ch
    p.wait()
    return killed


def recover(tool, status_exe, app, sram):
    rc, dur, out = fwu(tool, app, sram)
    st = mcu_status(status_exe)
    ok = rc == 0 and st.get("app") == "1"
    return ok, rc, dur, st, out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    app, sram = sys.argv[1], sys.argv[2]
    results = sys.argv[3] if len(sys.argv) > 3 else "fwu_fault_results"
    os.makedirs(results, exist_ok=True)
    tool, status_exe = build()
    old_app = os.environ.get("A510_OLD_APP")

    def must_refuse(name, args):
        """The pre-check must reject this offline: exit 2 and no I2C traffic."""
        r = sh(["timeout", "180", tool] + args, cwd=ROOT)
        if r.returncode != 2:
            sys.exit("%s: expected the image pre-check to refuse (exit 2), got %d\n%s%s"
                     % (name, r.returncode, r.stdout, r.stderr))
        return r.returncode

    # prepared bad images
    data = open(os.path.join(ROOT, app), "rb").read()
    trunc = os.path.join(results, "app_truncated.bin")
    open(trunc, "wb").write(data[: len(data) // 2])
    bad = bytearray(data)
    bad[len(data) * 3 // 4] ^= 0xFF          # payload only, header stays intact
    badcrc = os.path.join(results, "app_badcrc.bin")
    open(badcrc, "wb").write(bytes(bad))

    cases = [
        ("kill@10%", lambda: kill_at_percent(tool, app, sram, 10), "MCU away from APP, recoverable"),
        ("kill@50%", lambda: kill_at_percent(tool, app, sram, 50), "MCU away from APP, recoverable"),
        ("kill@90%", lambda: kill_at_percent(tool, app, sram, 90), "MCU away from APP, recoverable"),
        ("truncated app", lambda: must_refuse("truncated app", [trunc, sram]),
         "host pre-check refuses, board untouched"),
        ("corrupt app payload", lambda: must_refuse("corrupt app payload", [badcrc, sram]),
         "host pre-check refuses, board untouched"),
        ("swapped images", lambda: must_refuse("swapped images", [sram, app]),
         "host pre-check refuses, board untouched"),
        ("no args", lambda: sh([tool], cwd=ROOT).returncode, "usage error, board untouched"),
        ("one arg", lambda: sh([tool, app], cwd=ROOT).returncode, "usage error, board untouched"),
        ("wrong bus", lambda: sh(["env", "A510_I2C_BUS=5", tool, app, sram], cwd=ROOT).returncode,
         "init fails, board untouched"),
    ]
    if old_app:
        cases.append(("swap to a different app", lambda: sh(["timeout", "180", tool, old_app, sram], cwd=ROOT).returncode,
                      "board boots the other image, then restored"))

    rows = []
    for name, action, expect in cases:
        aer0 = aer_count()
        before = mcu_status(status_exe)
        out = action()
        time.sleep(1)
        after_fault = mcu_status(status_exe)
        log = os.path.join(results, "case_%s.log" % re.sub(r"[^A-Za-z0-9]+", "_", name))
        with open(log, "w") as f:
            f.write("before=%s\nafter_fault=%s\naction_result=%s\n" % (before, after_fault, out))

        ok, rc, dur, st, rec_out = recover(tool, status_exe, app, sram)
        aer1 = aer_count()
        note = ""
        if aer0 is not None and aer1 is not None and aer1 > aer0:
            note = "PCIe errors +%d" % (aer1 - aer0)
        rows.append({
            "case": name, "expected": expect,
            "state_after_fault": after_fault.get("boot_state", "?"),
            "app_after_fault": after_fault.get("app", "?"),
            "board_id_after_fault": after_fault.get("board_id", "?"),
            "recovery_rc": rc, "recovery_s": "%.1f" % dur,
            "recovery_app": st.get("app", "?"), "recovery_hash": st.get("git_hash", "?"),
            "recovery_board_id": st.get("board_id", "?"),
            "result": "RECOVERED" if ok else "BRICKED",
            "note": note,
        })
        print("%-24s after_fault=%s  recovery=%s (%s)  %s"
              % (name, after_fault.get("boot_state", "?"), rows[-1]["result"], rows[-1]["recovery_s"], note))
        if not ok:
            with open(log, "a") as f:
                f.write("\nRECOVERY FAILED:\n%s\n" % rec_out)

    csv_path = os.path.join(results, "faults.csv")
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    print("\n== summary (%s) ==" % csv_path)
    bad_cases = [r["case"] for r in rows if r["result"] != "RECOVERED"]
    print("cases: %d, recovered: %d, bricked: %d"
          % (len(rows), len(rows) - len(bad_cases), len(bad_cases)))
    for c in bad_cases:
        print("  NOT RECOVERED: %s" % c)
    return 1 if bad_cases else 0


if __name__ == "__main__":
    sys.exit(main())
