#!/usr/bin/env python3
"""Offline regression for the slot A / slot B image pre-check. No MCU needed.

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

    tests/image_precheck.py [--build <a510_mcu/build/<board>>]

Without --build it uses the first ../a510_mcu/build/<board> holding both
app/app.bin (slot A) and app_b/app.bin (slot B).
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
SLOT_A = 0x08010000
SLOT_B = 0x08090000
SLOT_SIZE = 448 * 1024 - 16
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


def slot_a_cases(app):
    """(name, bytes) mutants for the slot A position."""
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
    add("vector unaligned", lambda b: struct.pack_into("<L", b, 12, SLOT_A + VECTOR_OFF + 1), True)
    add("vector in the old app window", lambda b: struct.pack_into("<L", b, 12, 0x08008200), True)
    add("vector in the slot B window", lambda b: struct.pack_into("<L", b, 12, SLOT_B + VECTOR_OFF), True)
    add("vector at the wrong slot offset", lambda b: struct.pack_into("<L", b, 12, SLOT_A + 0x400), True)
    add("stack pointer outside SRAM", lambda b: struct.pack_into("<L", b, VECTOR_OFF, 0x12345678), True)
    add("stack pointer past 192KB SRAM", lambda b: struct.pack_into("<L", b, VECTOR_OFF, 0x20038000), True)
    add("reset vector without Thumb bit",
        lambda b: struct.pack_into("<L", b, VECTOR_OFF + 4,
                                   struct.unpack_from("<L", b, VECTOR_OFF + 4)[0] & ~1), True)
    add("reset vector outside the image",
        lambda b: struct.pack_into("<L", b, VECTOR_OFF + 4, SLOT_A + SLOT_SIZE - 3), True)

    cases.append(("truncated in half", app[: len(app) // 2]))
    cases.append(("trailing bytes", app + b"\x00" * 4096))
    cases.append(("payload shrunk under the minimum", seal(app[: HEADER + 256])))
    cases.append(("larger than a slot", seal(app + b"\xff" * SLOT_SIZE)))
    cases.append(("tiny file", b"\x01\x02\x03"))
    cases.append(("ascii junk", b"not an A510 image, just some text.\n" * 8))
    return cases


def slot_b_cases(img):
    return [
        ("slot B: bad magic", mutated(img, lambda b: b.__setitem__(slice(0, 2), b"\x00\x00"))),
        ("slot B: payload bit flip", mutated(img, lambda b: b.__setitem__(len(b) - 1, b[-1] ^ 0xFF))),
        ("slot B: vector in the slot A window",
         mutated(img, lambda b: struct.pack_into("<L", b, 12, SLOT_A + VECTOR_OFF), True)),
    ]


def other_board(img):
    """The same image re-labelled for another board, header kept consistent."""
    f = fields(img)
    bid = 1 if f["board_id"] != 1 else 2

    def relabel(b):
        struct.pack_into("<L", b, 24, bid)
        b[28:36] = BOARDS[bid].encode().ljust(8, b"\0")
    return mutated(img, relabel, True)


def tmpfile(blob):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(blob)
        return f.name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", help="a510_mcu build/<board> directory with app/ and app_b/")
    args = ap.parse_args()

    if args.build:
        build_dir = args.build
    else:
        found = [d for d in sorted(glob.glob(os.path.join(ROOT, "..", "a510_mcu", "build", "*")))
                 if os.path.exists(os.path.join(d, "app_b", "app.bin"))]
        if not found:
            sys.exit("no a510_mcu build with app/app.bin and app_b/app.bin (pass --build)")
        build_dir = found[0]
    a_path = os.path.join(build_dir, "app", "app.bin")
    b_path = os.path.join(build_dir, "app_b", "app.bin")
    a_img = open(a_path, "rb").read()
    b_img = open(b_path, "rb").read()
    tool = build()

    for label, img in (("slot A", a_img), ("slot B", b_img)):
        f = fields(img)
        print("%s %s: board=%s(%d) image_size=%d vectors=0x%08x git=%s"
              % (label, os.path.relpath(build_dir), f["board_name"], f["board_id"],
                 f["image_size"], f["vector_addr"], f["git_sha"]))

    failures = []
    total = 0

    def expect(name, want_rc, rc, first, must_contain=None):
        nonlocal total
        total += 1
        ok = rc == want_rc and (must_contain is None or must_contain in first)
        print("  %-38s exit=%d %s" % (name, rc, "ok" if ok else "FAIL (want %d)" % want_rc))
        if not ok:
            failures.append("%s: exit %d, want %d (%s)" % (name, rc, want_rc, first))

    print("== rejecting bad slot A images ==")
    for name, blob in slot_a_cases(a_img):
        path = tmpfile(blob)
        try:
            rc, out = run(tool, path, b_path)
            expect(name, 2, rc, first_line(out))
        finally:
            os.unlink(path)
    print("== rejecting bad slot B images ==")
    for name, blob in slot_b_cases(b_img):
        path = tmpfile(blob)
        try:
            rc, out = run(tool, a_path, path)
            expect(name, 2, rc, first_line(out))
        finally:
            os.unlink(path)

    print("== pair consistency (refused offline, before any I2C) ==")
    rc, out = run(tool, b_path, a_path)
    expect("swapped slots", 2, rc, first_line(out), "this is a slot B image")
    path = tmpfile(other_board(b_img))
    try:
        rc, out = run(tool, a_path, path)
        expect("pair from two different boards", 2, rc, first_line(out), "image pair mismatch")
    finally:
        os.unlink(path)

    print("== usage ==")
    rc, out = run(tool, "-h")
    expect("--help", 0, rc, first_line(out))
    rc, out = run(tool)
    expect("no arguments", 1, rc, first_line(out))
    rc, out = run(tool, a_path)
    expect("one argument", 1, rc, first_line(out))

    print("== accepting a real pair (stops at the I2C layer) ==")
    rc, out = run(tool, a_path, b_path)
    expect("slot A + slot B", 1, rc, out, "Failed to initialize I2C driver")

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
