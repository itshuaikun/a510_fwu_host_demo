#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
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

static inline bool is_fwu_ready(void) {
    uint8_t status;
    i2c_driver.read(I2C_ADDR, FWU_STATUS_ADDR, &status, 1);
    return status == 0x00;
}

static uint16_t fwu_fifo_length = 0;
static uint8_t* fwu_fifo_buf = NULL;
static inline void update_fwu_fifo_length(void) {
    union {
        uint8_t length8[2];
        uint16_t length16;
    } length;
    ASSERT_FATAL(i2c_driver.read(I2C_ADDR, FWU_FIFO_LENGTH_ADDR, length.length8, 2) == 0, "Failed to read FWU_FIFO_LENGTH");
    ASSERT_FATAL(length.length16 > 0, "FWU_FIFO_LENGTH is 0");
    fwu_fifo_length = LE16_TO_HOST(length.length16);
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
        fread(fwu_fifo_buf, 1, to_read, fp);
        while (!is_fwu_ready()) {
        }
        if (i2c_driver.write(I2C_ADDR, FWU_FIFO_ADDR, fwu_fifo_buf, to_read) != 0) {
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
    while (!is_fwu_ready()); // make sure sent data is processed
    return 0;
}

static inline void send_fwu_cmd(uint8_t cmd) {
    ASSERT_FATAL(i2c_driver.write(I2C_ADDR, FWU_CONTROL_ADDR, (uint8_t[]){cmd}, 1) == 0, "Failed to send command");
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <app.bin> [fwu_sram.bin]\n", prog);
}

int main(int argc, char **argv)
{
    assert(&i2c_driver != NULL);
    assert(i2c_driver.init != NULL);
    assert(i2c_driver.deinit != NULL);
    assert(i2c_driver.write != NULL);
    assert(i2c_driver.read != NULL);
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc < 2 || argc > 3) {
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    ASSERT_FATAL(i2c_driver.init() == 0, "Failed to initialize I2C driver");

    send_fwu_cmd(FWU_CMD_REQUEST);
    usleep(20000);
    update_fwu_fifo_length();

    printf("     BL => FWU_SRAM... ");
    ASSERT_FATAL(send_file((argc == 3) ? argv[2] : ".default_fwu_sram", true) == 0, "Failed to send fwu_sram");
    send_fwu_cmd(FWU_CMD_FILE_SEND_DONE);
    usleep(10000);

    printf("     programming... ");
    ASSERT_FATAL(send_file(argv[1], true) == 0, "Failed to send file: %s", argv[1]);
    
    send_fwu_cmd(FWU_CMD_REBOOT);

    ASSERT_FATAL(i2c_driver.deinit() == 0, "Failed to deinitialize I2C driver");
    free(fwu_fifo_buf);
    return 0;
}
