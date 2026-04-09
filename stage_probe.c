#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "i2c_mem_rw.h"

#define I2C_ADDR 0x12
#define BOOT_STATE_ADDR 0x01
#define I2C_HW_DIAG_ADDR 0xde
#define I2C_HW_DIAG_LEN 10
#define FWU_RUNTIME_ADDR 0xe8
#define FWU_RUNTIME_LEN 14
#define FWU_STATUS_ADDR 0xfb
#define FWU_FIFO_LENGTH_ADDR 0xfc
#define I2C_DIAG_ADDR 0xf2
#define I2C_DIAG_LEN 9
#define FWU_FIFO_ADDR 0xfe
#define FWU_CONTROL_ADDR 0xff

#define FWU_CMD_REQUEST 0x01
#define FWU_CMD_RESEND 0x02
#define FWU_CMD_FILE_SEND_DONE 0x03

#define BOOT_STATE_BOOTLOADER 0x00
#define BOOT_STATE_FWU_SRAM 0x02

static int get_env_int(const char *name, int default_value, int min_value, int max_value)
{
    const char *env = getenv(name);
    char *end = NULL;
    long value;

    if (env == NULL || *env == '\0') {
        return default_value;
    }

    value = strtol(env, &end, 0);
    if (end == env || *end != '\0' || value < min_value || value > max_value) {
        return default_value;
    }

    return (int)value;
}

static int read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_mem_read(I2C_ADDR, reg, buf, len);
}

static int write_reg(uint8_t reg, const uint8_t *buf, size_t len)
{
    return i2c_mem_write(I2C_ADDR, reg, (unsigned char *)buf, len);
}

static bool read_boot_state(uint8_t *state)
{
    return read_reg(BOOT_STATE_ADDR, state, 1) == 0;
}

static bool wait_boot_state(uint8_t expected, int retries, useconds_t delay_us)
{
    int i;

    for (i = 0; i < retries; ++i) {
        uint8_t state;

        if (read_boot_state(&state) && state == expected) {
            return true;
        }
        usleep(delay_us);
    }

    return false;
}

static bool wait_ready(int retries, useconds_t delay_us)
{
    int i;

    for (i = 0; i < retries; ++i) {
        uint8_t status;

        if (read_reg(FWU_STATUS_ADDR, &status, 1) == 0 && status == 0U) {
            return true;
        }
        usleep(delay_us);
    }

    return false;
}

static size_t read_fifo_length(void)
{
    uint8_t raw[2];

    if (read_reg(FWU_FIFO_LENGTH_ADDR, raw, sizeof(raw)) != 0) {
        return 0;
    }

    return (size_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
}

static void dump_runtime(void)
{
    uint8_t boot_state = 0xff;
    uint8_t hw_diag[I2C_HW_DIAG_LEN];
    uint8_t runtime[FWU_RUNTIME_LEN];
    uint8_t diag[I2C_DIAG_LEN];
    uint8_t status = 0xff;

    memset(hw_diag, 0, sizeof(hw_diag));
    memset(runtime, 0, sizeof(runtime));
    memset(diag, 0, sizeof(diag));

    (void)read_boot_state(&boot_state);
    (void)read_reg(I2C_HW_DIAG_ADDR, hw_diag, sizeof(hw_diag));
    (void)read_reg(FWU_RUNTIME_ADDR, runtime, sizeof(runtime));
    (void)read_reg(I2C_DIAG_ADDR, diag, sizeof(diag));
    (void)read_reg(FWU_STATUS_ADDR, &status, 1);

    printf("boot_state=0x%02x\n", boot_state);
    printf("hw_diag:");
    for (size_t i = 0; i < sizeof(hw_diag); ++i) {
        printf(" %02x", hw_diag[i]);
    }
    printf("\n");
    printf("runtime:");
    for (size_t i = 0; i < sizeof(runtime); ++i) {
        printf(" %02x", runtime[i]);
    }
    printf("\n");
    printf("diag:");
    for (size_t i = 0; i < sizeof(diag); ++i) {
        printf(" %02x", diag[i]);
    }
    printf("\n");
    printf("status=0x%02x\n", status);
}

static int send_cmd(uint8_t cmd, bool allow_disconnect)
{
    if (write_reg(FWU_CONTROL_ADDR, &cmd, 1) == 0) {
        return 0;
    }

    return allow_disconnect ? 0 : -1;
}

static int send_file(const char *path, bool wait_for_final_ready)
{
    FILE *fp = fopen(path, "rb");
    size_t fifo_len = read_fifo_length();
    size_t chunk_limit = i2c_transport_chunk_size();
    useconds_t poll_us = (useconds_t)get_env_int("A510_STAGE_PROBE_POLL_US", 10000, 1, 1000000);
    useconds_t chunk_delay_us = (useconds_t)get_env_int("A510_STAGE_PROBE_CHUNK_DELAY_US", 10000, 0, 1000000);
    uint8_t *buf;
    size_t size;
    size_t offset = 0;

    if (fp == NULL) {
        perror("fopen");
        return -1;
    }

    if (fifo_len == 0) {
        fclose(fp);
        return -1;
    }

    if (fifo_len > chunk_limit) {
        fifo_len = chunk_limit;
    }

    buf = malloc(fifo_len);
    if (buf == NULL) {
        fclose(fp);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    size = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    while (offset < size) {
        size_t len = size - offset;

        if (len > fifo_len) {
            len = fifo_len;
        }

        if (fread(buf, 1, len, fp) != len) {
            free(buf);
            fclose(fp);
            return -1;
        }

        if (!wait_ready(500, poll_us)) {
            fprintf(stderr, "ready timeout offset=%zu\n", offset);
            free(buf);
            fclose(fp);
            return -1;
        }

        if (write_reg(FWU_FIFO_ADDR, buf, len) != 0) {
            fprintf(stderr, "write failure offset=%zu len=%zu\n", offset, len);
            dump_runtime();
            free(buf);
            fclose(fp);
            return -1;
        }

        if (chunk_delay_us > 0) {
            usleep(chunk_delay_us);
        }

        offset += len;
        if ((offset % 1024U) == 0U || offset == size) {
            printf("offset=%zu/%zu\n", offset, size);
            fflush(stdout);
        }
    }

    free(buf);
    fclose(fp);

    if (wait_for_final_ready && !wait_ready(500, poll_us)) {
        fprintf(stderr, "final ready timeout\n");
        dump_runtime();
        return -1;
    }

    return 0;
}

int main(void)
{
    const char *fwu_sram = "/home/sk/Programming/seehi/A510_MCU/a510_mcu/build/c2/fwu_sram/fwu_sram.bin";
    const char *app = "/home/sk/Programming/seehi/A510_MCU/a510_mcu/build/c2/app/app.bin";
    uint8_t state = 0xff;

    if (init_device() != 0) {
        fprintf(stderr, "init_device failed\n");
        return 1;
    }

    if (!read_boot_state(&state)) {
        fprintf(stderr, "read_boot_state failed\n");
        return 1;
    }

    if (state != BOOT_STATE_BOOTLOADER) {
        if (send_cmd(FWU_CMD_REQUEST, true) != 0) {
            fprintf(stderr, "request failed\n");
            return 1;
        }
    }

    if (!wait_boot_state(BOOT_STATE_BOOTLOADER, 500, 10000)) {
        fprintf(stderr, "bootloader not ready\n");
        return 1;
    }

    if (send_file(fwu_sram, false) != 0) {
        fprintf(stderr, "send fwu_sram failed\n");
        return 1;
    }

    if (send_cmd(FWU_CMD_FILE_SEND_DONE, true) != 0) {
        fprintf(stderr, "file_send_done failed\n");
        return 1;
    }

    usleep(100000);
    if (!wait_boot_state(BOOT_STATE_FWU_SRAM, 500, 10000)) {
        fprintf(stderr, "FWU_SRAM not ready\n");
        dump_runtime();
        return 1;
    }

    printf("entered FWU_SRAM fifo_length=%zu chunk_limit=%zu\n",
           read_fifo_length(),
           i2c_transport_chunk_size());

    if (send_cmd(FWU_CMD_RESEND, false) != 0) {
        fprintf(stderr, "resend failed\n");
        return 1;
    }

    usleep(20000);
    if (!wait_boot_state(BOOT_STATE_FWU_SRAM, 500, 10000)) {
        fprintf(stderr, "FWU_SRAM disappeared after resend\n");
        dump_runtime();
        return 1;
    }

    if (send_file(app, true) != 0) {
        fprintf(stderr, "send app failed\n");
        return 1;
    }

    printf("APP sent successfully\n");
    dump_runtime();
    return 0;
}
