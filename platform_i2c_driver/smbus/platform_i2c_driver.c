#include "platform_i2c_driver.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * SMBus backend.
 *
 * A PCH SMBus controller (Intel I801) exposes no raw I2C, only SMBus, so the
 * transfer has to be an I2C_SMBUS ioctl. It must be an *I2C-block* transfer:
 * that puts register byte + data bytes on the wire, exactly like the MCU
 * protocol wants. An SMBus *block* transfer would insert a length byte
 * between them and corrupt the stream.
 *
 * I2C-block carries at most I2C_SMBUS_BLOCK_MAX (32) data bytes, so long
 * writes are split into several transactions; adapters without I2C-block fall
 * back to byte-data, one data byte per transfer.
 */

#define MCU_ADDR 0x12
#define MCU_DEVICE_ID_REG 0x00
#define MCU_DEVICE_ID 0xA5
#define BUS_SCAN_MAX 64
#define BUS_NAME_LEN 64
#define BLOCK_CHUNK_MAX 32

static int i2c_fd = -1;
static size_t chunk_max = 0;

static int adapter_name(int bus, char *out, size_t len)
{
    char path[64];
    FILE *fp;

    snprintf(path, sizeof(path), "/sys/class/i2c-dev/i2c-%d/name", bus);
    fp = fopen(path, "r");
    if (fp == NULL)
        return -1;
    if (fgets(out, len, fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    out[strcspn(out, "\r\n")] = '\0';
    return 0;
}

static int smbus_xfer(int fd, int read_write, uint8_t command, uint32_t size,
                      union i2c_smbus_data *data)
{
    struct i2c_smbus_ioctl_data args = {
        .read_write = read_write,
        .command = command,
        .size = size,
        .data = data,
    };

    return ioctl(fd, I2C_SMBUS, &args);
}

/* One SMBus transaction: reg, then len (<= chunk_max) data bytes. */
static int smbus_write_msg(int fd, uint8_t reg, const uint8_t *data, size_t len)
{
    union i2c_smbus_data d;

    if (chunk_max == BLOCK_CHUNK_MAX) {
        memset(&d, 0, sizeof(d));
        d.block[0] = (uint8_t)len;
        memcpy(&d.block[1], data, len);
        return smbus_xfer(fd, I2C_SMBUS_WRITE, reg, I2C_SMBUS_I2C_BLOCK_DATA, &d);
    }

    d.byte = *data;
    return smbus_xfer(fd, I2C_SMBUS_WRITE, reg, I2C_SMBUS_BYTE_DATA, &d);
}

/* One SMBus transaction: reg, repeated START, len (<= chunk_max) data bytes. */
static int smbus_read_msg(int fd, uint8_t reg, uint8_t *data, size_t len)
{
    union i2c_smbus_data d;

    memset(&d, 0, sizeof(d));
    if (chunk_max == BLOCK_CHUNK_MAX) {
        d.block[0] = (uint8_t)len;
        if (smbus_xfer(fd, I2C_SMBUS_READ, reg, I2C_SMBUS_I2C_BLOCK_DATA, &d) < 0)
            return -1;
        memcpy(data, &d.block[1], len);
        return 0;
    }

    if (smbus_xfer(fd, I2C_SMBUS_READ, reg, I2C_SMBUS_BYTE_DATA, &d) < 0)
        return -1;
    *data = d.byte;
    return 0;
}

static int probe_bus(int fd, const char *name)
{
    unsigned long funcs = 0;
    uint8_t id = 0;

    if (ioctl(fd, I2C_SLAVE, MCU_ADDR) < 0)
        return -1;
    if (ioctl(fd, I2C_FUNCS, &funcs) < 0)
        return -1;

    if (funcs & I2C_FUNC_SMBUS_I2C_BLOCK) {
        chunk_max = BLOCK_CHUNK_MAX;
    } else if (funcs & I2C_FUNC_SMBUS_BYTE_DATA) {
        chunk_max = 1;
    } else {
        return -1;
    }

    if (smbus_read_msg(fd, MCU_DEVICE_ID_REG, &id, 1) != 0 || id != MCU_DEVICE_ID) {
        chunk_max = 0;
        return -1;
    }

    printf("SMBus I2C bus %s: %s\n", name,
           chunk_max == BLOCK_CHUNK_MAX ? "SMBus I2C-block, 32 bytes per transfer"
                                        : "SMBus byte-data, 1 byte per transfer");
    return 0;
}

static int find_bus(void)
{
    const char *forced = getenv("A510_I2C_BUS");
    int first = 0, last = BUS_SCAN_MAX - 1;

    if (forced != NULL && *forced != '\0') {
        first = last = atoi(forced);
        if (first < 0) {
            fprintf(stderr, "A510_I2C_BUS=%s is not a bus number\n", forced);
            return -1;
        }
    }

    for (int bus = first; bus <= last; bus++) {
        char path[64], name[BUS_NAME_LEN];
        int fd;

        if (adapter_name(bus, name, sizeof(name)) != 0)
            continue;
        snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (probe_bus(fd, name) == 0)
            return fd;
        close(fd);
    }

    return -1;
}

static int init_device(void)
{
    if (i2c_fd >= 0)
        return 0;

    i2c_fd = find_bus();
    if (i2c_fd < 0) {
        fprintf(stderr,
                "No SMBus adapter answered with A510 DEVICE_ID=0x%02X at 0x%02X.\n"
                "Set A510_I2C_BUS=<n> to force a bus.\n",
                MCU_DEVICE_ID, MCU_ADDR);
        return -1;
    }

    return 0;
}

static int deinit_device(void)
{
    if (i2c_fd < 0)
        return 0;

    if (close(i2c_fd) < 0) {
        fprintf(stderr, "Failed to close I2C device: %s\n", strerror(errno));
        return -1;
    }

    i2c_fd = -1;
    chunk_max = 0;
    return 0;
}

static int i2c_mem_write(unsigned char addr, unsigned char reg, unsigned char *data, size_t len)
{
    size_t offset = 0;

    if (i2c_fd < 0 || addr != MCU_ADDR) {
        fprintf(stderr, "I2C device is not initialized for slave 0x%02X\n", addr);
        return -1;
    }

    /*
     * The register byte is resent for every chunk: the only long write is the
     * FWU FIFO, whose address must not auto-increment.
     */
    while (offset < len) {
        size_t chunk_len = len - offset;

        if (chunk_len > chunk_max)
            chunk_len = chunk_max;

        if (smbus_write_msg(i2c_fd, reg, data + offset, chunk_len) != 0) {
            return -1;
        }

        offset += chunk_len;
    }

    return 0;
}

static int i2c_mem_read(unsigned char addr, unsigned char reg, unsigned char *data, size_t len)
{
    size_t offset = 0;

    if (i2c_fd < 0 || addr != MCU_ADDR) {
        fprintf(stderr, "I2C device is not initialized for slave 0x%02X\n", addr);
        return -1;
    }

    while (offset < len) {
        size_t chunk_len = len - offset;

        if (chunk_len > chunk_max)
            chunk_len = chunk_max;

        /*
         * The slave auto-increments its address pointer, but every transaction
         * restarts at the register we send, so advance it ourselves.
         */
        if (smbus_read_msg(i2c_fd, (uint8_t)(reg + offset), data + offset, chunk_len) != 0) {
            return -1;
        }

        offset += chunk_len;
    }

    return 0;
}

const platform_i2c_driver_t platform_i2c_driver = {
    .init = init_device,
    .deinit = deinit_device,
    .write = i2c_mem_write,
    .read = i2c_mem_read,
};
