#!/usr/bin/env python3
"""Offline regression for the app.bin / fwu_sram.bin pre-check. No MCU needed.

The tool is always run with A510_I2C_BUS pointing at a bus that cannot exist, so
any run that reaches the driver fails with exit 1. That makes the exit codes
meaningful on their own:

    exit 2  the image was rejected offline, the MCU was never touched
    exit 1  the images were accepted and the run only failed at the I2C layer

Mutants come in two families. Integrity mutants are raw corruptions (bad magic,
bad version, bad size, flipped payload byte, truncation, trailing bytes) and must
be caught by the header/CRC checks. Semantic mutants (bad board id, misplaced
vector table, bad stack pointer or reset vector) keep the header self-consistent
by recomputing image_size and CRC32, so they exercise the checks that sit behind
the CRC instead of stopping at it.

    tests/image_precheck.py [--app <app.bin>] [--sram <fwu_sram.bin>]

Without --app it looks for ../a510_mcu/build/*/app/app.bin and skips the cases
that need a real app image when none is found.
Env: BACKEND=smbus|ch347 (default smbus).
Exit: 0 every case behaved as expected, 1 otherwise.
"""
import argparse
import glob
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BACKEND = os.environ.get("BACKEND", "smbus")

HEADER = 36
VECTOR_OFF = 0x200
APP_BASE = 0x08008000
APP_SIZE = 992 * 1024
SRAM_BASE = 0x20001800
SRAM_SIZE = 250 * 1024
BOARDS = {1: "D1_EVB", 2: "D1_C2", 3: "D2_EVB", 4: "D2_OAM"}

# A bus number that cannot exist, so a run that gets past the pre-check fails in
# the driver instead of programming a real board.
NO_SUCH_BUS = "999"


def build():
    src = os.path.join(ROOT, "platform_i2c_driver", BACKEND, "platform_i2c_driver.c")
    if not os.path.exists(src):
        sys.exit("no such backend: %s" % src)
    exe = os.path.join(ROOT, "fwu_host_demo")
    r = subprocess.run(
        ["gcc", "-O2", "-o", exe, "main.c", "image.c", src, "-Iplatform_i2c_driver"],
        cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("build failed:\n%s" % r.stderr)
    return exe


def run(tool, *args):
    env = dict(os.environ, A510_I2C_BUS=NO_SUCH_BUS)
    r = subprocess.run([tool] + list(args), cwd=ROOT, capture_output=True, text=True, env=env)
    return r.returncode, r.stderr + r.stdout


def first_line(text):
    for line in text.splitlines():
        if line.strip():
            return line.strip()
    return ""


def fields(buf):
    magic, version, crc, size, vaddr = struct.unpack("<HHLLI", buf[0:16])
    return {
        "magic": magic, "version": version, "crc32": crc, "image_size": size,
        "vector_addr": vaddr, "git_sha": buf[16:24].split(b"\0")[0].decode(errors="replace"),
        "board_id": struct.unpack("<I", buf[24:28])[0],
        "board_name": buf[28:36].split(b"\0")[0].decode(errors="replace"),
    }


def seal(buf):
    """Recompute image_size and CRC32 so only the intended field is wrong."""
    b = bytearray(buf)
    size = len(b) - HEADER
    struct.pack_into("<LL", b, 4, zlib.crc32(bytes(b[HEADER:])) & 0xffffffff, size)
    return bytes(b)


def mutated(base, fn, seal_it=False):
    b = bytearray(base)
    fn(b)
    return seal(bytes(b)) if seal_it else bytes(b)


def app_cases(app):
    """(name, bytes) mutants for the app.bin position."""
    cases = []

    def add(name, fn, seal_it=False):
        cases.append((name, mutated(app, fn, seal_it)))

    add("bad magic", lambda b: b.__setitem__(slice(0, 2), b"\x34\x12"))
    add("bad version 1", lambda b: b.__setitem__(2, 1))
    add("bad version 9", lambda b: b.__setitem__(2, 9))
    add("size field mismatch", lambda b: struct.pack_into("<L", b, 8, 12345))
    add("payload bit flip", lambda b: b.__setitem__(len(b) - 1, b[-1] ^ 0xFF))
    add("unknown board id", lambda b: struct.pack_into("<L", b, 24, 99), True)
    add("board id 0", lambda b: struct.pack_into("<L", b, 24, 0), True)
    add("board name mismatch", lambda b: b.__setitem__(slice(28, 36), b"ZZZZZZZ\x00"), True)
    add("vector unaligned", lambda b: struct.pack_into("<L", b, 12, APP_BASE + VECTOR_OFF + 1), True)
    add("vector out of any window", lambda b: struct.pack_into("<L", b, 12, 0x08000200), True)
    add("vector in the SRAM window", lambda b: struct.pack_into("<L", b, 12, SRAM_BASE + VECTOR_OFF), True)
    add("vector at the wrong app offset", lambda b: struct.pack_into("<L", b, 12, APP_BASE + 0x400), True)
    add("stack pointer outside SRAM", lambda b: struct.pack_into("<L", b, VECTOR_OFF, 0x12345678), True)
    add("reset vector without Thumb bit",
        lambda b: struct.pack_into("<L", b, VECTOR_OFF + 4,
                                   struct.unpack_from("<L", b, VECTOR_OFF + 4)[0] & ~1), True)
    add("reset vector outside the image",
        lambda b: struct.pack_into("<L", b, VECTOR_OFF + 4, APP_BASE + APP_SIZE - 3), True)

    cases.append(("truncated in half", app[: len(app) // 2]))
    cases.append(("trailing bytes", app + b"\x00" * 4096))
    cases.append(("payload shrunk under the minimum", seal(app[: HEADER + 256])))
    cases.append(("tiny file", b"\x01\x02\x03"))
    cases.append(("ascii junk", b"not an A510 image, just some text.\n" * 8))
    return cases


def sram_cases(sram):
    return [
        ("sram: bad magic", mutated(sram, lambda b: b.__setitem__(slice(0, 2), b"\x00\x00"))),
        ("sram: payload bit flip", mutated(sram, lambda b: b.__setitem__(len(b) - 1, b[-1] ^ 0xFF))),
        ("sram: vector in the app window",
         mutated(sram, lambda b: struct.pack_into("<L", b, 12, APP_BASE + VECTOR_OFF), True)),
    ]


def shipped_sram(board_id=None):
    """Shipped fwu_sram_bin/ images, optionally only those of one board."""
    out = []
    for path in sorted(glob.glob(os.path.join(ROOT, "fwu_sram_bin", "*_fwu_sram.bin"))):
        with open(path, "rb") as f:
            if board_id is None or fields(f.read())["board_id"] == board_id:
                out.append(path)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", help="a real app.bin from the firmware build (version 2)")
    ap.add_argument("--sram", help="a real fwu_sram.bin (default: the shipped one for the app's board)")
    args = ap.parse_args()

    app_path = args.app
    if app_path is None:
        found = sorted(glob.glob(os.path.join(ROOT, "..", "a510_mcu", "build", "*", "app", "app.bin")))
        app_path = found[0] if found else None
    app = open(app_path, "rb").read() if app_path else None

    if args.sram:
        sram_path = args.sram
    else:
        # Two images naming different boards are refused as a pair, so default to
        # the shipped fwu_sram of the same board as the app image.
        same_board = shipped_sram(fields(app)["board_id"] if app else None)
        sram_path = same_board[0] if same_board else (shipped_sram() or [None])[0]
        if sram_path is None:
            sys.exit("no fwu_sram image under fwu_sram_bin/ (pass --sram)")
    if not os.path.exists(sram_path):
        sys.exit("no fwu_sram image: %s (pass --sram)" % sram_path)

    tool = build()
    sram = open(sram_path, "rb").read()

    if app:
        f = fields(app)
        print("app  %s: board=%s(%d) image_size=%d vectors=0x%08x git=%s"
              % (os.path.basename(app_path), f["board_name"], f["board_id"],
                 f["image_size"], f["vector_addr"], f["git_sha"]))
    f = fields(sram)
    print("sram %s: board=%s(%d) image_size=%d vectors=0x%08x git=%s"
          % (os.path.basename(sram_path), f["board_name"], f["board_id"],
             f["image_size"], f["vector_addr"], f["git_sha"]))

    failures = []
    total = 0

    def expect(name, want_rc, rc, first, why=""):
        nonlocal total
        total += 1
        ok = rc == want_rc
        print("  %-38s exit=%d %s%s" % (name, rc, "ok" if ok else "FAIL (want %d)" % want_rc, why))
        if not ok:
            failures.append("%s: exit %d, want %d (%s)" % (name, rc, want_rc, first))
        return ok

    print("== rejecting bad images (%s) ==" % os.path.basename(app_path or "-"))
    if app:
        for name, blob in app_cases(app):
            with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
                f.write(blob)
                path = f.name
            try:
                rc, out = run(tool, path, sram_path)
                expect(name, 2, rc, first_line(out))
            finally:
                os.unlink(path)
    else:
        print("  (no app.bin found; pass --app or build the firmware to cover these)")
    for name, blob in sram_cases(sram):
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(blob)
            path = f.name
        try:
            rc, out = run(tool, app_path or sram_path, path)
            if app_path is None:
                print("  %-38s skipped (needs --app)" % name)
            else:
                expect(name, 2, rc, first_line(out))
        finally:
            os.unlink(path)

    print("== image pair consistency (refused offline, before any I2C) ==")
    if app:
        app_board = fields(app)["board_id"]
        other = [p for p in shipped_sram() if fields(open(p, "rb").read())["board_id"] != app_board]
        if not other:
            print("  %-38s skipped (no shipped fwu_sram for another board)"
                  % "pair from two different boards")
        else:
            rc, out = run(tool, app_path, other[0])
            ok = rc == 2 and "image pair mismatch" in out
            total += 1
            print("  %-38s exit=%d %s" % ("pair from two different boards", rc,
                                          "ok" if ok else "FAIL (want exit 2 + 'image pair mismatch')"))
            if not ok:
                failures.append("pair from two different boards: exit %d, want 2 with 'image "
                                "pair mismatch' (%s)" % (rc, first_line(out)))
    else:
        print("  skipped (needs --app)")

    print("== usage ==")
    rc, out = run(tool, "-h")
    expect("--help", 0, rc, first_line(out))
    rc, out = run(tool)
    expect("no arguments", 1, rc, first_line(out))

    print("== accepting a real pair (stops at the I2C layer) ==")
    if app:
        rc, out = run(tool, app_path, sram_path)
        expect("app + fwu_sram", 1, rc, first_line(out))
        total += 1
        if "Failed to initialize I2C driver" in out:
            print("  %-38s ok (%s)" % ("stopped in the I2C layer", first_line(out)))
        else:
            print("  %-38s FAIL (%s)" % ("stopped in the I2C layer", first_line(out)))
            failures.append("accepted pair did not stop in the I2C layer: %s" % first_line(out))
    else:
        print("  skipped (needs --app)")

    print()
    if failures:
        print("%d/%d checks failed:" % (len(failures), total))
        for f in failures:
            print("  - %s" % f)
        return 1
    print("all %d checks passed" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
