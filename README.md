# A510 FWU Host Demo

Firmware update host tool for A510 MCU via I2C.

## Requirements

- GCC or Clang
- Meson
- Ninja

## Build

```bash
meson setup build
meson compile -C build
```

### I2C backend

`platform_i2c_driver/<backend>/` implements the MCU memory protocol for one
host transport; the option picks which directory is built. Every backend
provides the same `const platform_i2c_driver_t platform_i2c_driver` declared in
`platform_i2c_driver/platform_i2c_driver.h`, so `main.c` never names a backend
function.

| `-Di2c_backend=` | transport | typical host |
| ---------------- | --------- | ------------ |
| `ch347` (default) | `I2C_RDWR` | CH347 USB bridge (`ch34x-mphsi-i2c`) |
| `smbus` | `I2C_SMBUS` (I2C-block) | PCH SMBus (Intel I801), no raw I2C |

```bash
meson setup build -Di2c_backend=smbus
meson compile -C build
```

The `smbus` backend uses *I2C-block* transfers (register byte + up to 32 data
bytes) and never an SMBus *block* transfer, which would insert a length byte
and corrupt the stream. Either backend auto-detects the bus by reading
`DEVICE_ID` (`0xA5`) at slave `0x12`; set `A510_I2C_BUS=<n>` to force one.

## Usage

Both images are required: the bootloader jumps to whatever `fwu_sram.bin` it
receives without validating it, so it has to match the board being updated.

```bash
./build/fwu_host_demo <app.bin> <fwu_sram.bin>

# e.g. a D1 C2 board
./build/fwu_host_demo app.bin fwu_sram_bin/d1_c2_fwu_sram.bin
```

There is no flag that skips the checks: the tool validates, flashes, and reports
errors, nothing else.

## Pre-check

Both files are checked **before the first I2C access**, and the board is asked
which board it is **before it is reset into the bootloader**. A rejected file or
a wrong board therefore leaves the MCU running what it already has.

Exit codes:

| Code | Meaning |
| ---- | ------- |
| 0 | Update finished (`-h`/`--help` also exits 0) |
| 1 | Usage, I2C/device failure, or MCU firmware too old for this image |
| 2 | **Image rejected** (including an unreadable file); the MCU was never touched |

What is checked, per file: readable regular file; magic `0x9CA3`; header version
2; `image_size == file size - 36`; CRC32 of everything after the header (the same
zlib/IEEE CRC the firmware computes); the image fits its region (992KB flash or
250KB SRAM); `vector_addr` is exactly the 512-byte-aligned vector table at the
region base + `0x200` and lies inside the image; the initial stack pointer is in
SRAM and the reset vector has its Thumb bit set and points into the image. The
two arguments are also role-checked, so `app.bin` and `fwu_sram.bin` swapped by
mistake is refused instead of flashed.

Then, with the MCU on the bus:

* `VERSION` (memmap version, `0x02`) must be `>= 0x0201`. Older firmware computes
  its CRC from offset 24 and would reject every version 2 image, so the tool
  refuses and tells you to flash `build/<board>/flash.bin` over SWD once.
* `BOARD_ID` (`0x52`) must match the `board_id` of **both** images
  (`1=D1_EVB`, `2=D1_C2`, `3=D2_EVB`, `4=D2_OAM`). A version 2 image for another
  board still passes magic/version/CRC, so this is the check that stops it. The
  `fwu_sram` image is compared too, not just `app.bin`: the bootloader jumps into
  it without validating it, and the four `fwu_sram` builds differ only in their
  header, the build timestamp, and the board id they report while running — so
  today any of them would work, and checking both is what keeps that from being
  an assumption the tool silently relies on.

Offline regression (no MCU needed, ~28 cases including CRC-consistent semantic
mutants):

```bash
python3 tests/image_precheck.py [--app <app.bin>] [--sram <fwu_sram.bin>]
```

## Side effects on the host

`FWU_CMD_REQUEST` resets the MCU, and the MCU owns the A510's power and reset
lines, so every update also resets the card. The host sees the device leave the
PCIe link and logs AER entries: a 10-iteration soak produced one
`severity=Uncorrected (Fatal)` entry per iteration, and the link re-trained at
2.5GT/s (its capability is 16GT/s). Update when the device is idle, and pass
`A510_BDF=<bdf>` to `tests/fwu_stress.sh` to record the link speed and the error
counter around every iteration instead of finding out later.

## Interrupting

`Ctrl-C` (SIGINT) and a dropped terminal (SIGHUP) are ignored from the moment the
application image starts programming until the MCU has been asked to reboot. The
MCU is rewriting the only application slot during that window, so stopping there
would leave a half-written image that will not boot (recoverable, but only by
re-running a full update). The first `Ctrl-C` prints a one-line notice explaining
that it was ignored, so nobody reaches for the power switch instead.

Everything else is not held off: SIGTERM and SIGKILL still stop the tool (a
supervisor needs that, and `tests/fwu_faults.py` SIGKILLs the tool on purpose to
test exactly that half-written state), and interrupting the earlier
`BL => FWU_SRAM` phase stays harmless because it only writes SRAM.

## Portability

Runtime dependencies are just libc and the kernel's `i2c-dev` (plus an adapter
that supports I2C-block); no libusb, no i2c-tools, no python. The binary itself
is **not** portable between distributions, for two reasons that are easy to
trip over:

1. **glibc is forward-incompatible.** A binary built against a newer glibc
   refuses to start on an older one
   (`version 'GLIBC_2.38' not found`). Check with `ldd ./fwu_host_demo` and
   `objdump -T ./fwu_host_demo | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1`.
2. **The build host's instruction-set baseline sticks.** A rolling/v4 toolchain
   (e.g. an `x86_64_v4` distro) links a libc that uses AVX-512, and even
   `-march=x86-64 -static` does not help because the offending code comes from
   the prebuilt libc: such a binary dies with `Illegal instruction` on an older
   Xeon.

So build where it will run, or build in a container matching the target:

```bash
# build for Ubuntu 22.04, from anywhere
docker run --rm -v "$PWD:/w" -w /w ubuntu:22.04 bash -c \
  'apt-get -qq update && apt-get -qq install -y gcc meson ninja-build &&
   meson setup build -Di2c_backend=smbus && meson compile -C build'
```

Also required per host: the MCU's SMBus has to be routed to the PCH (the
`smbus` backend only answers on boards whose PCIe slot carries the SMBus
sideband), and `/dev/i2c-*` is root-only by default.

`fwu_sram_bin/` ships the FWU_SRAM image of every board variant
(`d1_evb`, `d1_c2`, `d2_evb`, `d2_oam`); regenerate them with the matching
`fwu_sram` target of the A510 MCU firmware build. Each one carries the board
identity in its header, so they must be rebuilt whenever the firmware tree moves:
an image older than the rest of the tree still passes the format checks but its
`git_sha` will differ from the `app.bin` you flash alongside it (the tool prints a
note, it does not refuse).

