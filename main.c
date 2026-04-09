#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
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
#define FWU_PACKET_DATA_ADDR 0x40
#define FWU_PACKET_DATA_LEN 128
#define FWU_PACKET_OFFSET_ADDR 0xc0
#define FWU_PACKET_OFFSET_LEN 4
#define FWU_PACKET_LEN_ADDR 0xc4
#define FWU_PACKET_LEN_LEN 2
#define FWU_PACKET_CRC_ADDR 0xc6
#define FWU_PACKET_CRC_LEN 4
#define FWU_PACKET_ACK_ADDR 0xca
#define FWU_PACKET_ACK_LEN 4
#define FWU_PACKET_STATUS_ADDR 0xce
#define FWU_PACKET_STATUS_LEN 1
#define FWU_PACKET_COMMIT_ADDR 0xcf
#define FWU_RUNTIME_ADDR 0xe8
#define FWU_RUNTIME_LEN 14
#define FWU_STATUS_ADDR 0xfb
#define FWU_FIFO_LENGTH_ADDR 0xfc
#define FWU_FIFO_ADDR 0xfe
#define FWU_CONTROL_ADDR 0xff

#define FWU_CMD_REBOOT 0x00
#define FWU_CMD_REQUEST 0x01
#define FWU_CMD_RESEND 0x02
#define FWU_CMD_FILE_SEND_DONE 0x03
#define FWU_PACKET_STATUS_IDLE 0x00
#define FWU_PACKET_STATUS_OK 0x01
#define FWU_PACKET_STATUS_OFFSET_MISMATCH 0x02
#define FWU_PACKET_STATUS_LEN_INVALID 0x03
#define FWU_PACKET_STATUS_CRC_MISMATCH 0x04
#define FWU_PACKET_STATUS_BUSY 0x05

#define BOOT_STATE_BOOTLOADER 0x00
#define BOOT_STATE_APPLICATION 0x01
#define BOOT_STATE_FWU_SRAM 0x02

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

static inline bool is_fwu_ready(void);

static uint32_t crc32_for_byte(uint32_t r)
{
    int j;

    for (j = 0; j < 8; ++j) {
        r = (r & 1U ? 0U : (uint32_t)0xEDB88320U) ^ r >> 1;
    }

    return r ^ (uint32_t)0xFF000000U;
}

static uint32_t crc32_calc(const void *data, uint32_t n_bytes)
{
    static uint32_t table[0x100];
    static bool have_table = false;
    uint32_t crc = 0;
    const unsigned char *p = (const unsigned char *)data;
    uint32_t i;

    if (!have_table) {
        for (i = 0; i < 0x100; ++i) {
            table[i] = crc32_for_byte(i);
        }
        have_table = true;
    }

    for (i = 0; i < n_bytes; ++i) {
        crc = table[(uint8_t)crc ^ p[i]] ^ crc >> 8;
    }

    return crc;
}

static int i2c_read_retry(unsigned char dev_addr,
                          unsigned char reg_addr,
                          uint8_t *data,
                          size_t len,
                          int retries,
                          useconds_t delay_us)
{
    int attempt;

    for (attempt = 0; attempt < retries; ++attempt) {
        if (i2c_driver.read(dev_addr, reg_addr, data, len) == 0) {
            return 0;
        }
        usleep(delay_us);
    }

    return -1;
}

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

static useconds_t get_ready_poll_delay_us(void)
{
    return (useconds_t)get_env_int("A510_I2C_READY_POLL_US", 10000, 1, 1000000);
}

static useconds_t get_inter_chunk_delay_us(void)
{
    return (useconds_t)get_env_int("A510_I2C_INTER_CHUNK_US", 0, 0, 1000000);
}

static useconds_t get_ready_poll_delay_us_for_state(uint8_t expected_state)
{
    if (expected_state == BOOT_STATE_FWU_SRAM) {
        return (useconds_t)get_env_int("A510_I2C_FWU_READY_POLL_US", 1000, 1, 1000000);
    }

    return get_ready_poll_delay_us();
}

static useconds_t get_inter_chunk_delay_us_for_state(uint8_t expected_state)
{
    if (expected_state == BOOT_STATE_FWU_SRAM) {
        return (useconds_t)get_env_int("A510_I2C_FWU_INTER_CHUNK_US", 0, 0, 1000000);
    }

    return get_inter_chunk_delay_us();
}

static size_t get_chunk_limit_for_state(size_t default_chunk_limit, uint8_t expected_state)
{
    if (expected_state == BOOT_STATE_FWU_SRAM) {
        size_t requested = (size_t)get_env_int("A510_I2C_FWU_CHUNK_SIZE", 32, 1, (int)default_chunk_limit);
        return (requested < default_chunk_limit) ? requested : default_chunk_limit;
    }

    return default_chunk_limit;
}

static useconds_t get_cmd_retry_delay_us(void)
{
    return (useconds_t)get_env_int("A510_I2C_CMD_RETRY_DELAY_US", 20000, 1, 1000000);
}

static useconds_t get_stage_transition_delay_us(void)
{
    return (useconds_t)get_env_int("A510_I2C_STAGE_TRANSITION_US", 100000, 1, 2000000);
}

static int get_packet_retry_count(void)
{
    return get_env_int("A510_I2C_PACKET_RETRY_COUNT", 20, 1, 100);
}

static int get_packet_ack_poll_count(void)
{
    return get_env_int("A510_I2C_PACKET_ACK_POLL_COUNT", 10, 1, 100);
}

static useconds_t get_packet_ack_poll_delay_us(void)
{
    return (useconds_t)get_env_int("A510_I2C_PACKET_ACK_POLL_US", 200, 0, 1000000);
}

static int get_ready_stable_reads(void)
{
    return get_env_int("A510_I2C_READY_STABLE_READS", 1, 1, 16);
}

static int get_cmd_retry_count(void)
{
    return get_env_int("A510_I2C_CMD_RETRY_COUNT", 3, 1, 16);
}

static bool wait_for_fwu_ready_stable(int retries, useconds_t delay_us)
{
    int attempt;
    int stable_reads = 0;
    int stable_target = get_ready_stable_reads();

    for (attempt = 0; attempt < retries; ++attempt) {
        if (is_fwu_ready()) {
            stable_reads++;
            if (stable_reads >= stable_target) {
                return true;
            }
        } else {
            stable_reads = 0;
        }
        usleep(delay_us);
    }

    return false;
}

static bool read_boot_state(uint8_t *state)
{
    uint8_t raw_state;

    if (state == NULL) {
        return false;
    }

    if (i2c_read_retry(I2C_ADDR, BOOT_STATE_ADDR, &raw_state, 1, 3, 5000) != 0) {
        return false;
    }

    *state = raw_state;
    return true;
}

static bool wait_for_boot_state(uint8_t expected_state, int retries, useconds_t delay_us)
{
    int attempt;

    for (attempt = 0; attempt < retries; ++attempt) {
        uint8_t state;

        if (read_boot_state(&state) && state == expected_state) {
            return true;
        }
        usleep(delay_us);
    }

    return false;
}

static bool wait_for_any_boot_state(uint8_t *state, int retries, useconds_t delay_us)
{
    int attempt;

    if (state == NULL) {
        return false;
    }

    for (attempt = 0; attempt < retries; ++attempt) {
        if (read_boot_state(state)) {
            return true;
        }
        usleep(delay_us);
    }

    return false;
}

static bool read_u32_reg(uint8_t reg, uint32_t *value)
{
    uint8_t raw[4];

    if (value == NULL) {
        return false;
    }

    if (i2c_read_retry(I2C_ADDR, reg, raw, sizeof(raw), 3, 5000) != 0) {
        return false;
    }

    *value =
        ((uint32_t)raw[0] << 0U) |
        ((uint32_t)raw[1] << 8U) |
        ((uint32_t)raw[2] << 16U) |
        ((uint32_t)raw[3] << 24U);
    return true;
}

static bool read_u8_reg(uint8_t reg, uint8_t *value)
{
    return i2c_read_retry(I2C_ADDR, reg, value, 1, 3, 5000) == 0;
}

static bool read_runtime_reg(uint8_t *value, size_t len)
{
    if (value == NULL || len == 0) {
        return false;
    }

    return i2c_read_retry(I2C_ADDR, FWU_RUNTIME_ADDR, value, len, 3, 5000) == 0;
}

static bool read_runtime_state(uint32_t *bytes_written, uint32_t *staged_len, uint8_t *file_done)
{
    uint8_t raw[FWU_RUNTIME_LEN];

    if (!read_runtime_reg(raw, sizeof(raw))) {
        return false;
    }

    if (bytes_written != NULL) {
        *bytes_written =
            ((uint32_t)raw[0] << 0U) |
            ((uint32_t)raw[1] << 8U) |
            ((uint32_t)raw[2] << 16U) |
            ((uint32_t)raw[3] << 24U);
    }

    if (staged_len != NULL) {
        *staged_len =
            ((uint32_t)raw[4] << 0U) |
            ((uint32_t)raw[5] << 8U) |
            ((uint32_t)raw[6] << 16U) |
            ((uint32_t)raw[7] << 24U);
    }

    if (file_done != NULL) {
        *file_done = raw[13];
    }

    return true;
}

static int write_u32_reg(uint8_t reg, uint32_t value)
{
    uint8_t raw[4] = {
        (uint8_t)(value >> 0U),
        (uint8_t)(value >> 8U),
        (uint8_t)(value >> 16U),
        (uint8_t)(value >> 24U),
    };

    return i2c_driver.write(I2C_ADDR, reg, raw, sizeof(raw));
}

static int write_reg_block(uint8_t reg, const uint8_t *data, size_t len)
{
    return i2c_driver.write(I2C_ADDR, reg, (unsigned char *)data, len);
}

static int write_u16_reg(uint8_t reg, uint16_t value)
{
    uint8_t raw[2] = {
        (uint8_t)(value >> 0U),
        (uint8_t)(value >> 8U),
    };

    return i2c_driver.write(I2C_ADDR, reg, raw, sizeof(raw));
}

static bool read_ack_and_status(uint32_t *ack_offset, uint8_t *status)
{
    uint8_t raw[FWU_PACKET_ACK_LEN + FWU_PACKET_STATUS_LEN];

    if (ack_offset == NULL || status == NULL) {
        return false;
    }

    if (i2c_read_retry(I2C_ADDR,
                       FWU_PACKET_ACK_ADDR,
                       raw,
                       sizeof(raw),
                       3,
                       5000) != 0) {
        return false;
    }

    *ack_offset =
        ((uint32_t)raw[0] << 0U) |
        ((uint32_t)raw[1] << 8U) |
        ((uint32_t)raw[2] << 16U) |
        ((uint32_t)raw[3] << 24U);
    *status = raw[4];
    return true;
}

static inline bool is_fwu_ready(void) {
    uint8_t status;
    if (i2c_read_retry(I2C_ADDR, FWU_STATUS_ADDR, &status, 1, 3, 5000) != 0) {
        return false;
    }
    return status == 0x00;
}

static uint16_t fwu_fifo_length = 0;
static uint8_t* fwu_fifo_buf = NULL;
static inline void update_fwu_fifo_length(void) {
    union {
        uint8_t length8[2];
        uint16_t length16;
    } length;
    ASSERT_FATAL(i2c_read_retry(I2C_ADDR, FWU_FIFO_LENGTH_ADDR, length.length8, 2, 50, 10000) == 0,
                 "Failed to read FWU_FIFO_LENGTH");
    ASSERT_FATAL(length.length16 > 0, "FWU_FIFO_LENGTH is 0");
    fwu_fifo_length = LE16_TO_HOST(length.length16);
    free(fwu_fifo_buf);
    fwu_fifo_buf = (uint8_t*)malloc(fwu_fifo_length);
    ASSERT_FATAL(fwu_fifo_buf != NULL, "Buy more RAM!");
}

static int send_fwu_cmd_retry(uint8_t cmd, bool allow_disconnect)
{
    int attempt;
    int retry_count = get_cmd_retry_count();
    useconds_t retry_delay_us = get_cmd_retry_delay_us();
    int ret = -1;

    for (attempt = 1; attempt <= retry_count; ++attempt) {
        ret = i2c_driver.write(I2C_ADDR, FWU_CONTROL_ADDR, (uint8_t[]){cmd}, 1);
        if (ret == 0) {
            usleep(retry_delay_us);
            return 0;
        }
        usleep(retry_delay_us);
    }

    if (ret != 0 && !allow_disconnect) {
        return -1;
    }

    return ret;
}

static size_t get_packet_size_for_state(uint8_t expected_state)
{
    int default_size = (expected_state == BOOT_STATE_FWU_SRAM) ? FWU_PACKET_DATA_LEN : FWU_PACKET_DATA_LEN;
    int packet_size = get_env_int("A510_I2C_PACKET_SIZE", default_size, 1, FWU_PACKET_DATA_LEN);
    return (size_t)packet_size;
}

static int send_packet(uint32_t offset,
                       const uint8_t *data,
                       size_t len,
                       uint8_t expected_state,
                       useconds_t ready_poll_delay_us,
                       useconds_t inter_chunk_delay_us)
{
    int attempt;
    int retry_count = get_packet_retry_count();
    int ack_poll_count = get_packet_ack_poll_count();
    useconds_t ack_poll_delay_us = get_packet_ack_poll_delay_us();
    uint32_t last_ack_offset = 0;
    uint8_t last_packet_status = FWU_PACKET_STATUS_IDLE;

    for (attempt = 1; attempt <= retry_count; ++attempt) {
        uint32_t ack_offset = 0;
        uint8_t packet_status = FWU_PACKET_STATUS_IDLE;
        uint32_t expected_ack = offset + (uint32_t)len;
        uint8_t meta[FWU_PACKET_OFFSET_LEN + FWU_PACKET_LEN_LEN + FWU_PACKET_CRC_LEN];

        if (!wait_for_fwu_ready_stable(500, ready_poll_delay_us)) {
            continue;
        }

        meta[0] = (uint8_t)(offset >> 0U);
        meta[1] = (uint8_t)(offset >> 8U);
        meta[2] = (uint8_t)(offset >> 16U);
        meta[3] = (uint8_t)(offset >> 24U);
        meta[4] = (uint8_t)(len >> 0U);
        meta[5] = (uint8_t)(len >> 8U);
        {
            uint32_t crc = crc32_calc(data, (uint32_t)len);
            meta[6] = (uint8_t)(crc >> 0U);
            meta[7] = (uint8_t)(crc >> 8U);
            meta[8] = (uint8_t)(crc >> 16U);
            meta[9] = (uint8_t)(crc >> 24U);
        }

        if (write_reg_block(FWU_PACKET_OFFSET_ADDR, meta, sizeof(meta)) != 0 ||
            i2c_driver.write(I2C_ADDR, FWU_PACKET_DATA_ADDR, (unsigned char *)data, len) != 0) {
            usleep(ready_poll_delay_us);
        } else if (i2c_driver.write(I2C_ADDR,
                                    FWU_PACKET_COMMIT_ADDR,
                                    (uint8_t[]){0xA5},
                                    1) != 0) {
            usleep(ready_poll_delay_us);
        }

        for (int poll = 0; poll < ack_poll_count; ++poll) {
            bool ack_ok = read_ack_and_status(&ack_offset, &packet_status);
            bool status_ok = ack_ok;

            if (ack_ok) {
                last_ack_offset = ack_offset;
            }
            if (status_ok) {
                last_packet_status = packet_status;
            }

            if (ack_ok && ack_offset == expected_ack) {
                if (inter_chunk_delay_us > 0) {
                    usleep(inter_chunk_delay_us);
                }
                return 0;
            }

            if (ack_ok && ack_offset != offset) {
                fprintf(stderr,
                        "\npacket ack mismatch: offset=%u ack=%u len=%zu status=0x%02x state=0x%02x attempt=%d/%d\n",
                        offset,
                        ack_offset,
                        len,
                        packet_status,
                        expected_state,
                        attempt,
                        retry_count);
                return -1;
            }

            if (status_ok) {
                if (packet_status == FWU_PACKET_STATUS_LEN_INVALID ||
                    packet_status == FWU_PACKET_STATUS_CRC_MISMATCH ||
                    packet_status == FWU_PACKET_STATUS_OFFSET_MISMATCH) {
                    break;
                }
            }

            if (poll + 1 < ack_poll_count && ack_poll_delay_us > 0) {
                usleep(ack_poll_delay_us);
            }
        }

        usleep(ready_poll_delay_us);
    }

    fprintf(stderr,
            "\npacket retry exhausted: offset=%u ack=%u len=%zu status=0x%02x state=0x%02x\n",
            offset,
            last_ack_offset,
            len,
            last_packet_status,
            expected_state);
    return -1;
}

static int send_file(const char *filename,
                     bool show_progress,
                     uint8_t expected_state,
                     bool wait_for_final_ready) {
    FILE *fp = fopen(filename, "rb");
    size_t chunk_limit = get_packet_size_for_state(expected_state);
    useconds_t ready_poll_delay_us = get_ready_poll_delay_us_for_state(expected_state);
    useconds_t inter_chunk_delay_us = get_inter_chunk_delay_us_for_state(expected_state);
    uint8_t packet_buf[FWU_PACKET_DATA_LEN];
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
        size_t to_read = file_size - offset;
        if (to_read > chunk_limit) {
            to_read = chunk_limit;
        }
        if (fread(packet_buf, 1, to_read, fp) != to_read) {
            fclose(fp);
            return -1;
        }
        if (send_packet((uint32_t)offset,
                        packet_buf,
                        to_read,
                        expected_state,
                        ready_poll_delay_us,
                        inter_chunk_delay_us) != 0) {
            uint8_t state = 0xff;
            bool state_ok = read_boot_state(&state);
            fprintf(stderr,
                    "\npacket failure: file=%s offset=%zu len=%zu boot_state=%s0x%02x\n",
                    filename,
                    offset,
                    to_read,
                    state_ok ? "" : "unreadable:",
                    state);
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
    if (wait_for_final_ready && !wait_for_fwu_ready_stable(500, ready_poll_delay_us)) {
        return -1;
    }
    return 0;
}

static inline void send_fwu_cmd(uint8_t cmd, bool allow_disconnect) {
    if (send_fwu_cmd_retry(cmd, allow_disconnect) != 0 && !allow_disconnect) {
        ASSERT_FATAL(false, "Failed to send command");
    }
}

static int resend_current_stage(uint8_t expected_state)
{
    useconds_t ready_poll_delay_us = get_ready_poll_delay_us();

    if (!wait_for_boot_state(expected_state, 200, 10000)) {
        return -1;
    }

    send_fwu_cmd(FWU_CMD_RESEND, false);
    usleep(20000);

    if (!wait_for_boot_state(expected_state, 200, 10000)) {
        return -1;
    }

    while (!is_fwu_ready()) {
        usleep(ready_poll_delay_us);
    }

    return 0;
}

static bool wait_for_runtime_flush(uint32_t expected_bytes, int retries, useconds_t delay_us)
{
    int attempt;

    for (attempt = 0; attempt < retries; ++attempt) {
        uint32_t bytes_written = 0;
        uint32_t staged_len = 0;
        uint8_t file_done = 0;

        if (read_runtime_state(&bytes_written, &staged_len, &file_done) &&
            bytes_written == expected_bytes &&
            staged_len == 0U &&
            file_done != 0U) {
            return true;
        }

        usleep(delay_us);
    }

    return false;
}

static int finalize_current_stage(uint8_t expected_state, uint32_t expected_bytes)
{
    useconds_t ready_poll_delay_us = get_ready_poll_delay_us_for_state(expected_state);

    if (!wait_for_boot_state(expected_state, 200, 10000)) {
        return -1;
    }

    send_fwu_cmd(FWU_CMD_FILE_SEND_DONE, false);
    usleep(20000);

    if (!wait_for_boot_state(expected_state, 200, 10000)) {
        return -1;
    }

    if (!wait_for_runtime_flush(expected_bytes, 500, ready_poll_delay_us)) {
        return -1;
    }

    if (!wait_for_fwu_ready_stable(500, ready_poll_delay_us)) {
        return -1;
    }

    return 0;
}

static int send_file_with_resend(const char *filename,
                                 bool show_progress,
                                 uint8_t expected_state,
                                 bool wait_for_final_ready,
                                 bool send_file_done,
                                 const char *stage_name)
{
    const int max_attempts = 5;
    int attempt;
    uint32_t expected_bytes;

    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) {
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    expected_bytes = (uint32_t)ftell(fp);
    fclose(fp);

    for (attempt = 1; attempt <= max_attempts; ++attempt) {
        if (send_file(filename,
                      show_progress,
                      expected_state,
                      send_file_done ? false : wait_for_final_ready) == 0) {
            if (!send_file_done) {
                return 0;
            }

            if (finalize_current_stage(expected_state, expected_bytes) == 0) {
                return 0;
            }
        }

        if (attempt == max_attempts) {
            break;
        }

        fprintf(stderr,
                "%s transfer failed on attempt %d/%d, restarting stage\n",
                stage_name,
                attempt,
                max_attempts);

        if (resend_current_stage(expected_state) != 0) {
            break;
        }
    }

    return -1;
}

static int send_fwu_sram_stage(const char *filename)
{
    const int max_attempts = 5;
    int attempt;

    for (attempt = 1; attempt <= max_attempts; ++attempt) {
        if (send_file(filename, true, BOOT_STATE_BOOTLOADER, false) == 0) {
            send_fwu_cmd(FWU_CMD_FILE_SEND_DONE, true);
            usleep(get_stage_transition_delay_us());
            if (wait_for_boot_state(BOOT_STATE_FWU_SRAM, 200, 10000)) {
                return 0;
            }
        }

        if (attempt == max_attempts) {
            break;
        }

        fprintf(stderr,
                "FWU_SRAM stage did not transition on attempt %d/%d, restarting stage\n",
                attempt,
                max_attempts);

        if (resend_current_stage(BOOT_STATE_BOOTLOADER) != 0) {
            break;
        }
    }

    return -1;
}

int main(int argc, char **argv)
{
    uint8_t boot_state;

    assert(&i2c_driver != NULL);
    assert(i2c_driver.init != NULL);
    assert(i2c_driver.deinit != NULL);
    assert(i2c_driver.write != NULL);
    assert(i2c_driver.read != NULL);
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <app.bin> [fwu_sram.bin]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    ASSERT_FATAL(i2c_driver.init() == 0, "Failed to initialize I2C driver");

    ASSERT_FATAL(wait_for_any_boot_state(&boot_state, 100, 10000), "Target did not respond on I2C");

    if (boot_state != BOOT_STATE_BOOTLOADER) {
        send_fwu_cmd(FWU_CMD_REQUEST, true);
    }
    ASSERT_FATAL(wait_for_boot_state(BOOT_STATE_BOOTLOADER, 100, 10000), "Bootloader did not come up");
    update_fwu_fifo_length();

    printf("     BL => FWU_SRAM... ");
    ASSERT_FATAL(send_fwu_sram_stage((argc == 3) ? argv[2] : ".default_fwu_sram") == 0,
                 "Failed to send fwu_sram");
    update_fwu_fifo_length();

    printf("     programming... ");
    ASSERT_FATAL(send_file_with_resend(argv[1],
                                       true,
                                       BOOT_STATE_FWU_SRAM,
                                       true,
                                       true,
                                       "APP") == 0,
                 "Failed to send file: %s", argv[1]);
    
    send_fwu_cmd(FWU_CMD_REBOOT, true);

    ASSERT_FATAL(i2c_driver.deinit() == 0, "Failed to deinitialize I2C driver");
    free(fwu_fifo_buf);
    return 0;
}
