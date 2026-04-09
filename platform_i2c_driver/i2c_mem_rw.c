#include "i2c_mem_rw.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CH34X_MPHSI_I2C_BUS_NUM 24
#define CH34X_MPHSI_I2C_DEV_PATH "/dev/i2c-24"
#define CH34X_MPHSI_I2C_NAME_PATH "/sys/class/i2c-dev/i2c-24/name"
#define CH34X_MPHSI_I2C_ADAPTER_NAME "ch34x-mphsi-i2c"
/*
 * WCH CH347 I2C master driver reports -EPROTO for large write transactions
 * even when the bus waveform shows every byte is ACKed. Keeping a single I2C
 * write within one USB packet avoids that driver bug.
 */
#define I2C_TRANSFER_CHUNK_SIZE 498

static int i2c_fd = -1;

static int validate_adapter_name(void)
{
    FILE *fp = fopen(CH34X_MPHSI_I2C_NAME_PATH, "r");
    char name[64];

    if (fp == NULL) {
        fprintf(stderr, "Failed to open %s: %s\n", CH34X_MPHSI_I2C_NAME_PATH, strerror(errno));
        return -1;
    }

    if (fgets(name, sizeof(name), fp) == NULL) {
        fprintf(stderr, "Failed to read adapter name from %s: %s\n", CH34X_MPHSI_I2C_NAME_PATH, strerror(errno));
        fclose(fp);
        return -1;
    }
    fclose(fp);

    name[strcspn(name, "\r\n")] = '\0';
    if (strncmp(name, CH34X_MPHSI_I2C_ADAPTER_NAME, strlen(CH34X_MPHSI_I2C_ADAPTER_NAME)) != 0) {
        fprintf(stderr,
                "Unexpected I2C adapter on bus %d: %s (expected prefix %s)\n",
                CH34X_MPHSI_I2C_BUS_NUM,
                name,
                CH34X_MPHSI_I2C_ADAPTER_NAME);
        return -1;
    }

    return 0;
}

static int transfer_i2c_messages(struct i2c_msg *msgs, uint32_t num_msgs)
{
    struct i2c_rdwr_ioctl_data transfer = {
        .msgs = msgs,
        .nmsgs = num_msgs,
    };

    if (i2c_fd < 0) {
        errno = EBADF;
        return -1;
    }

    if (ioctl(i2c_fd, I2C_RDWR, &transfer) < 0) {
        return -1;
    }

    return 0;
}

int init_device(void)
{
    if (i2c_fd >= 0) {
        return 0;
    }

    if (validate_adapter_name() != 0) {
        return -1;
    }

    i2c_fd = open(CH34X_MPHSI_I2C_DEV_PATH, O_RDWR | O_CLOEXEC);
    if (i2c_fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", CH34X_MPHSI_I2C_DEV_PATH, strerror(errno));
        return -1;
    }

    return 0;
}

int deinit_device(void)
{
    if (i2c_fd < 0) {
        return 0;
    }

    if (close(i2c_fd) < 0) {
        fprintf(stderr, "Failed to close %s: %s\n", CH34X_MPHSI_I2C_DEV_PATH, strerror(errno));
        return -1;
    }

    i2c_fd = -1;
    return 0;
}

int i2c_mem_write(unsigned char addr, unsigned char reg, unsigned char *data, size_t len)
{
    uint8_t buffer[I2C_TRANSFER_CHUNK_SIZE + 1];
    size_t offset = 0;

    if (i2c_fd < 0) {
        fprintf(stderr, "I2C device is not initialized\n");
        return -1;
    }

    while (offset < len) {
        size_t chunk_len = len - offset;
        struct i2c_msg msg = {
            .addr = addr,
            .flags = 0,
            .len = (uint16_t)(chunk_len + 1),
            .buf = buffer,
        };

        if (chunk_len > I2C_TRANSFER_CHUNK_SIZE) {
            chunk_len = I2C_TRANSFER_CHUNK_SIZE;
            msg.len = (uint16_t)(I2C_TRANSFER_CHUNK_SIZE + 1);
        }

        buffer[0] = reg;
        memcpy(&buffer[1], data + offset, chunk_len);

        if (transfer_i2c_messages(&msg, 1) != 0) {
            fprintf(stderr,
                    "I2C write failed: addr=0x%02x reg=0x%02x offset=%zu len=%zu: %s\n",
                    addr,
                    reg,
                    offset,
                    chunk_len,
                    strerror(errno));
            return -1;
        }

        offset += chunk_len;
    }

    return 0;
}

int i2c_mem_read(unsigned char addr, unsigned char reg, unsigned char *data, size_t len)
{
    uint8_t reg_buf = reg;
    struct i2c_msg msgs[2] = {
        {
            .addr = addr,
            .flags = 0,
            .len = 1,
            .buf = &reg_buf,
        },
        {
            .addr = addr,
            .flags = I2C_M_RD,
            .len = (uint16_t)len,
            .buf = data,
        },
    };

    if (i2c_fd < 0) {
        fprintf(stderr, "I2C device is not initialized\n");
        return -1;
    }

    if (transfer_i2c_messages(msgs, 2) != 0) {
        fprintf(stderr,
                "I2C read failed: addr=0x%02x reg=0x%02x len=%zu: %s\n",
                addr,
                reg,
                len,
                strerror(errno));
        return -1;
    }

    return 0;
}
