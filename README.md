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
`fwu_sram` target of the A510 MCU firmware build.

