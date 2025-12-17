#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>

#define RETUEN0_OR_DIE(expr, fmt, ...) do { \
    if ((expr) != 0) { \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
        exit(EXIT_FAILURE); \
    } \
} while(0)

#define I2C_ADDR 0x12
#define DEVICE_ID_ADDR 0
#define BOOT_STATE_ADDR 1
#define MEM_DESC_VERSION_ADDR 2
#define MEM_DESC_VERSION_VALUE 0x0003
#define FWU_STATUS_ADDR 0xfb
#define FWU_FIFO_LENGTH_ADDR 0xfc
#define FWU_FIFO_ADDR 0xfe
#define FWU_CONTROL_ADDR 0xff

#define FWU_CMD_REBOOT 0x00
#define FWU_CMD_REQUEST 0x01
#define FWU_CMD_RESEND 0x02
#define FWU_CMD_FILE_SEND_DONE 0x03

// 7bit device address, 8bit register address, 8bit data unit
// return 0: success, other: error
typedef struct {
    int (*init)();
    int (*deinit)();
    int (*write)(unsigned char dev_addr, unsigned char reg_addr, unsigned char* data, size_t len);
    int (*read)(unsigned char dev_addr, unsigned char reg_addr, unsigned char* data, size_t len);
} platform_i2c_driver_t;

// platform_i2c_driver
#include "i2c_mem_rw.h"
platform_i2c_driver_t i2c_driver = {
    .init = init_device,
    .deinit = deinit_device,
    .write = i2c_mem_write,
    .read = i2c_mem_read,
};

static inline uint8_t get_device_id(void) {
    uint8_t device_id;
    i2c_driver.read(I2C_ADDR, DEVICE_ID_ADDR, &device_id, 1);
    return device_id;
}

static inline uint8_t get_boot_state(void) {
    uint8_t boot_state;
    i2c_driver.read(I2C_ADDR, BOOT_STATE_ADDR, &boot_state, 1);
    return boot_state;
}

static inline bool is_fwu_ready(void) {
    uint8_t status;
    i2c_driver.read(I2C_ADDR, FWU_STATUS_ADDR, &status, 1);
    return status == 0x00;
}

static int send_file(const char *filename, bool show_progress) {
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) {
        fprintf(stderr, "Failed to open file: %s\n", filename);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    unsigned char data[128];
    size_t offset = 0;
    int last_percent = -1;
    while (offset < file_size) {
        size_t to_read = (file_size - offset > 128) ? 128 : file_size - offset;
        fread(data, 1, to_read, fp);
        while (!is_fwu_ready());
        i2c_driver.write(I2C_ADDR, FWU_FIFO_ADDR, data, to_read);
        offset += to_read;
        if (show_progress) {
            int percent = (int)(offset * 100 / file_size);
            if (percent != last_percent) {
                printf("\r%3d%%", percent);
                fflush(stdout);
                last_percent = percent;
            }
        }
    }
    if (show_progress) printf("\n");
    fclose(fp);
    while (!is_fwu_ready()); // make sure sent data is processed
    return 0;
}

static inline void send_fwu_cmd(uint8_t cmd) {
    RETUEN0_OR_DIE(i2c_driver.write(I2C_ADDR, FWU_CONTROL_ADDR, (uint8_t[]){cmd}, 1), "Failed to send command");
}

int main(int argc, char **argv)
{
    assert(&i2c_driver != NULL);
    assert(i2c_driver.init != NULL);
    assert(i2c_driver.deinit != NULL);
    assert(i2c_driver.write != NULL);
    assert(i2c_driver.read != NULL);
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <app.bin> [fwu_sram.bin]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    RETUEN0_OR_DIE(i2c_driver.init(), "Failed to initialize I2C driver");

    send_fwu_cmd(FWU_CMD_REQUEST);
    usleep(20000);

    if (argc == 3) {
        printf("     BL => FWU_SRAM... ");
        RETUEN0_OR_DIE(send_file(argv[2], true), "Failed to send file: %s\n", argv[2]);
        send_fwu_cmd(FWU_CMD_FILE_SEND_DONE);
        usleep(10000);
    }

    printf("     programming... ");
    RETUEN0_OR_DIE(send_file(argv[1], true), "Failed to send file: %s\n", argv[1]);
    
    send_fwu_cmd(FWU_CMD_REBOOT);

    RETUEN0_OR_DIE(i2c_driver.deinit(), "Failed to deinitialize I2C driver");
    return 0;
}