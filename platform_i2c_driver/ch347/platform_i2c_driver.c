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
 * CH347 backend.
 *
 * The WCH CH347 I2C master is exported by the ch34x_mphsi_master kernel driver
 * as an i2c-dev adapter with raw I2C support, so a transfer is a plain
 * I2C_RDWR message: register byte, then data bytes, and for a read the
 * register write is followed by a repeated START.
 *
 * Driver bug workaround: ch34x_mphsi_master reports -EPROTO for write
 * transactions larger than one 512-byte USB packet even when the bus waveform
 * shows every byte is ACKed, so a single write stays inside that packet.
 */

#define MCU_ADDR 0x12
#define MCU_DEVICE_ID_REG 0x00
#define MCU_DEVICE_ID 0xA5
#define ADAPTER_NAME_PREFIX "ch34x-mphsi-i2c"
#define BUS_SCAN_MAX 64
#define BUS_NAME_LEN 64
#define CHUNK_MAX 498

static int i2c_fd = -1;
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

/* One raw I2C message: reg, then len data bytes. */
static int i2c_write_msg(int fd, uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buffer[CHUNK_MAX + 1];
    struct i2c_msg msg = {
        .addr = MCU_ADDR,
        .flags = 0,
        .len = (uint16_t)(len + 1),
        .buf = buffer,
    };
    struct i2c_rdwr_ioctl_data transfer = {
        .msgs = &msg,
        .nmsgs = 1,
    };

    buffer[0] = reg;
    memcpy(&buffer[1], data, len);
    return ioctl(fd, I2C_RDWR, &transfer) < 0 ? -1 : 0;
}

/* One raw I2C message pair: reg write, repeated START, len data bytes. */
static int i2c_read_msg(int fd, uint8_t reg, uint8_t *data, size_t len)
{
    struct i2c_msg msgs[2] = {
        { .addr = MCU_ADDR, .flags = 0, .len = 1, .buf = &reg },
        { .addr = MCU_ADDR, .flags = I2C_M_RD, .len = (uint16_t)len, .buf = data },
    };
    struct i2c_rdwr_ioctl_data transfer = {
        .msgs = msgs,
        .nmsgs = 2,
    };

    return ioctl(fd, I2C_RDWR, &transfer) < 0 ? -1 : 0;
}

static int probe_bus(int fd, const char *name)
{
    uint8_t id = 0;

    if (ioctl(fd, I2C_SLAVE, MCU_ADDR) < 0)
        return -1;
    if (i2c_read_msg(fd, MCU_DEVICE_ID_REG, &id, 1) != 0 || id != MCU_DEVICE_ID)
        return -1;

    printf("CH347 I2C bus %s: I2C_RDWR\n", name);
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
        if (strncmp(name, ADAPTER_NAME_PREFIX, strlen(ADAPTER_NAME_PREFIX)) != 0)
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
                "No %s adapter answered with A510 DEVICE_ID=0x%02X at 0x%02X.\n"
                "Load ch34x_mphsi_master and/or set A510_I2C_BUS=<n>.\n",
                ADAPTER_NAME_PREFIX, MCU_DEVICE_ID, MCU_ADDR);
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

        if (chunk_len > CHUNK_MAX)
            chunk_len = CHUNK_MAX;

        if (i2c_write_msg(i2c_fd, reg, data + offset, chunk_len) != 0) {
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

        if (chunk_len > CHUNK_MAX)
            chunk_len = CHUNK_MAX;

        /*
         * The slave auto-increments its address pointer, but every transaction
         * restarts at the register we send, so advance it ourselves.
         */
        if (i2c_read_msg(i2c_fd, (uint8_t)(reg + offset), data + offset, chunk_len) != 0) {
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
