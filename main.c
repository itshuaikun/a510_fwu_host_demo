#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

// Byte swap macros (MCU data is little-endian)
#define SWAP16(x) ((uint16_t)(((x) >> 8) | ((x) << 8)))
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    #define LE16_TO_HOST(x) (x)
#else
    #define LE16_TO_HOST(x) SWAP16(x)
#endif

#define ASSERT_FATAL(expr, fmt, ...) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
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

#include "platform_i2c_driver.h"

static inline bool is_fwu_ready(void) {
    uint8_t status = 0xFF;
    if (platform_i2c_driver.read(I2C_ADDR, FWU_STATUS_ADDR, &status, 1) != 0) {
        return false;
    }
    return status == 0x00;
}

#define FWU_WAIT_TIMEOUT_US (5 * 1000 * 1000)

static inline void wait_fwu_ready(const char *what) {
    int64_t waited_us = 0;
    while (!is_fwu_ready()) {
        usleep(1000);
        waited_us += 1000;
        ASSERT_FATAL(waited_us < FWU_WAIT_TIMEOUT_US,
                     "Timeout waiting for FWU FIFO to drain (%s)", what);
    }
}

// FWU_CONTROL REQUEST resets the MCU and the bootloader takes a while to
// bring its I2C slave up, so poll until FWU_FIFO_LENGTH reads back a usable
// value (in APP state that register reads 0, in BL/FWU_SRAM it is the FIFO
// capacity). A fixed sleep here is not enough on real hardware.
static uint16_t fwu_fifo_length = 0;
static uint8_t* fwu_fifo_buf = NULL;
static inline void update_fwu_fifo_length(void) {
    union {
        uint8_t length8[2];
        uint16_t length16;
    } length;
    int64_t waited_us = 0;
    // The MCU is off the bus while it resets, so a NACK here is the expected
    // answer: the retry itself is the progress report, no need to log it.
    while (true) {
        if (platform_i2c_driver.read(I2C_ADDR, FWU_FIFO_LENGTH_ADDR, length.length8, 2) == 0 &&
            length.length16 != 0) {
            fwu_fifo_length = LE16_TO_HOST(length.length16);
            break;
        }
        usleep(5000);
        waited_us += 5000;
        ASSERT_FATAL(waited_us < FWU_WAIT_TIMEOUT_US,
                     "Timeout waiting for FWU bootloader FIFO");
    }
    fwu_fifo_buf = (uint8_t*)malloc(fwu_fifo_length);
    ASSERT_FATAL(fwu_fifo_buf != NULL, "Buy more RAM!");
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

    size_t offset = 0;
    int last_percent = -1;
    while (offset < file_size) {
        size_t to_read = (file_size - offset > fwu_fifo_length) ? fwu_fifo_length : file_size - offset;
        if (fread(fwu_fifo_buf, 1, to_read, fp) != to_read) {
            fprintf(stderr, "Failed to read %zu bytes at offset %zu from %s\n",
                    to_read, offset, filename);
            fclose(fp);
            return -1;
        }
        wait_fwu_ready("before FIFO write");
        if (platform_i2c_driver.write(I2C_ADDR, FWU_FIFO_ADDR, fwu_fifo_buf, to_read) != 0) {
            fprintf(stderr, "I2C write failed: reg=0x%02x offset=%zu len=%zu: %s\n",
                    FWU_FIFO_ADDR, offset, to_read, strerror(errno));
            fclose(fp);
            return -1;
        }
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
    wait_fwu_ready("final drain"); // make sure sent data is processed
    return 0;
}

static inline void send_fwu_cmd(uint8_t cmd) {
    ASSERT_FATAL(platform_i2c_driver.write(I2C_ADDR, FWU_CONTROL_ADDR, (uint8_t[]){cmd}, 1) == 0,
                 "Failed to send command 0x%02X: %s", cmd, strerror(errno));
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <app.bin> <fwu_sram.bin>\n", prog);
}

int main(int argc, char **argv)
{
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc != 3) {
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    ASSERT_FATAL(platform_i2c_driver.init() == 0, "Failed to initialize I2C driver");

    send_fwu_cmd(FWU_CMD_REQUEST);
    update_fwu_fifo_length();

    printf("     BL => FWU_SRAM... ");
    ASSERT_FATAL(send_file(argv[2], true) == 0, "Failed to send fwu_sram");
    send_fwu_cmd(FWU_CMD_FILE_SEND_DONE);
    usleep(10000);

    printf("     programming... ");
    ASSERT_FATAL(send_file(argv[1], true) == 0, "Failed to send file: %s", argv[1]);
    
    send_fwu_cmd(FWU_CMD_REBOOT);

    ASSERT_FATAL(platform_i2c_driver.deinit() == 0, "Failed to deinitialize I2C driver");
    free(fwu_fifo_buf);
    return 0;
}
