#pragma once

#include <stddef.h>

/*
 * Platform I2C driver for the A510 MCU memory interface: one 7-bit slave
 * address, an 8-bit register address, then data bytes, and for a read the
 * register address is followed by a repeated START.
 *
 * A backend implements this interface and nothing else: it performs transfers
 * and returns 0 on success or -1 with errno preserved. Deciding what to log is
 * the caller's job. Which backend is built is the meson option `i2c_backend`.
 */

typedef struct {
    int (*init)(void);
    int (*deinit)(void);
    // 7-bit device address, 8-bit register address, 8-bit data unit
    int (*write)(unsigned char dev_addr, unsigned char reg_addr, unsigned char* data, size_t len);
    int (*read)(unsigned char dev_addr, unsigned char reg_addr, unsigned char* data, size_t len);
} platform_i2c_driver_t;

/* The backend selected at build time. */
extern const platform_i2c_driver_t platform_i2c_driver;
