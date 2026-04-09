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

When using the Linux `i2c-dev` path, the tool auto-detects a `ch34x-mphsi-i2c` bus.
You can also force a specific bus with `A510_I2C_BUS`:

```bash
A510_I2C_BUS=24 ./build/fwu_host_demo <app.bin>
```

## Host I2C CLI

```bash
# Scan the bus
./build/i2c_cli scan

# Read 16 bytes from device 0x12, register 0x00
./build/i2c_cli mem r 0x12 0x00 16

# Write 3 bytes to device 0x12, register 0x10
./build/i2c_cli mem w 0x12 0x10 3 0x11,0x22,0x33

# Use a specific CH34x device and 400kHz bus speed
./build/i2c_cli -i 0 -s 2 scan
```
