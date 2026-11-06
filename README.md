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

Requires MCU firmware with memmap `VERSION >= 0x0300` (A/B slot FWU, see
`a510_mcu/doc/interface/host_fwu_guide.md`). The firmware is linked once per
flash slot, so pass both builds of the same commit; the tool sends the one the
MCU asks for (`FWU_SLOT`).

```bash
./build/fwu_host_demo <slot_a.bin> <slot_b.bin>

# e.g. a D2 OAM board, from the a510_mcu build tree
./build/fwu_host_demo ../a510_mcu/build/d2_oam/app/app.bin ../a510_mcu/build/d2_oam/app_b/app.bin
```

What happens:

1. `REQUEST`: the running APP erases its **inactive** slot in the background. It
   first waits until the running slot has confirmed itself (30 s after the MCU
   started), so this step can take up to ~40 s.
2. The image is streamed through `FWU_FIFO` and written to the inactive slot
   while the APP keeps running (power, protection, PERST follow).
3. `FILE_SEND_DONE`: the MCU checks header, CRC, board and link address; `READY`.
4. `ACTIVATE`: warm switch to the new slot. The MCU is not reset, so the A510
   stays powered and its PCIe link stays up. The tool waits until the APP
   reports the new slot.

The new slot is on trial for 30 s: a crash or reset before that rolls back to
the previous slot. Firmware older than `0x0300` uses a different flash layout:
flash `a510_mcu build/<board>/flash.bin` over SWD once.

If no slot is valid the MCU waits in the bootloader (`BOOT_STATE=0`,
`FWU_SLOT=0xF0`); the same command restores slot A (that path resets the MCU).

## Pre-check

Both files are checked **before the first I2C access**, and the board is asked
which board it is before anything is written. A rejected file or a wrong board
therefore leaves the MCU running what it already has.

Exit codes:

| Code | Meaning |
| ---- | ------- |
| 0 | Update finished and the new slot is running (`-h`/`--help` also exits 0) |
| 1 | Usage, I2C/device failure, MCU firmware too old, or the MCU refused the image |
| 2 | **Image rejected** (including an unreadable file); the MCU was never touched |

Per file: readable regular file; magic `0x9CA3`; header version 2;
`image_size == file size - 36`; CRC32 of everything after the header; the image
fits a slot (448KB - 16B); `vector_addr` is exactly the slot base + `0x200`
(slot A `0x08010000`, slot B `0x08090000`), so swapped files are refused; the
initial stack pointer is in SRAM and the reset vector has its Thumb bit set and
points into the image. Both files must name the same board, and that board must
match the MCU's `BOARD_ID` (`0x52`).

Offline regression (no MCU needed):

```bash
python3 tests/image_precheck.py [--build ../a510_mcu/build/<board>]
```

## Side effects on the host

None by design: the update never resets the MCU, so the A510 and its PCIe link
stay up. `tests/fwu_stress.sh` with `A510_BDF=<bdf>` treats any PCIe error,
link-speed change or a slot that did not switch as a failure; consecutive
iterations alternate slots and each waits for the previous one to confirm.

## Interrupting

Safe at any point: until `ACTIVATE` only the inactive slot is written, and the
running firmware is untouched. Re-run the tool to finish the update
(`tests/fwu_faults.py` kills it mid-write on purpose and checks exactly that).

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
