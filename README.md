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

## Usage

```bash
# Update app only
./build/fwu_host_demo <app.bin>

# Update with custom FWU_SRAM
./build/fwu_host_demo <app.bin> <fwu_sram.bin>
```

