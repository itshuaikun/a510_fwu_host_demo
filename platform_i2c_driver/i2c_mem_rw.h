#pragma once

#include <stddef.h>

int init_device();
int deinit_device();
int i2c_mem_write(unsigned char addr, unsigned char reg, unsigned char* data, size_t len);
int i2c_mem_read(unsigned char addr, unsigned char reg, unsigned char* data, size_t len);
