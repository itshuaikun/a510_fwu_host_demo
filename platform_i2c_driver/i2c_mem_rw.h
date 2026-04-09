#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int init_device();
int init_device_ex(int dev_index, int clock_speed, int verbose);
int deinit_device();
int i2c_mem_write(unsigned char addr, unsigned char reg, unsigned char* data, size_t len);
int i2c_mem_read(unsigned char addr, unsigned char reg, unsigned char* data, size_t len);
int i2c_mem_write_ex(uint8_t addr, uint16_t reg, size_t reg_width, const uint8_t *data, size_t len);
int i2c_mem_read_ex(uint8_t addr, uint16_t reg, size_t reg_width, uint8_t *data, size_t len);
bool i2c_probe_device(uint8_t addr);
size_t i2c_transport_chunk_size(void);
