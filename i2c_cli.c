#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "i2c_mem_rw.h"

#define MAX_TRANSFER_SIZE 256

typedef struct {
    int dev_index;
    int clock_speed;
    int verbose;
    size_t addr_width;
    int linux_bus_num;
} global_options_t;

typedef struct {
    bool use_linux_i2c;
    int linux_fd;
} cli_backend_t;

static void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s [global options] scan [start_addr] [end_addr]\n", prog);
    printf("  %s [global options] mem <r|w> <device_addr> <mem_addr> <size> [data0,data1,...]\n", prog);
    printf("\n");
    printf("Global options:\n");
    printf("  -i, --index NUM        CH34x device index, e.g. 0 for /dev/ch34x_pis0\n");
    printf("  -b, --bus BUS          Linux i2c-dev bus number or /dev/i2c-X path\n");
    printf("  -s, --speed SPEED      I2C speed: 0=20k, 1=100k, 2=400k, 3=750k (default: 1)\n");
    printf("  -A, --addr-width WIDTH Register address width: auto, 8, 16 (default: auto)\n");
    printf("  -v, --verbose          Enable verbose output\n");
    printf("  -h, --help             Show this help\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s scan\n", prog);
    printf("  %s -i 0 -s 2 scan 0x08 0x77\n", prog);
    printf("  %s -b 24 scan\n", prog);
    printf("  %s mem r 0x12 0x00 16\n", prog);
    printf("  %s mem w 0x12 0x10 3 0x11,0x22,0x33\n", prog);
    printf("  %s -A 16 mem r 0x50 0x1234 8\n", prog);
}

static bool parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || value == NULL || *text == '\0') {
        return false;
    }

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static bool parse_byte(const char *text, uint8_t *value)
{
    uint32_t parsed;

    if (!parse_u32(text, &parsed) || parsed > 0xFF) {
        return false;
    }

    *value = (uint8_t)parsed;
    return true;
}

static bool parse_addr_width(const char *text, size_t *addr_width)
{
    if (strcmp(text, "auto") == 0) {
        *addr_width = 0;
        return true;
    }

    if (strcmp(text, "8") == 0) {
        *addr_width = 1;
        return true;
    }

    if (strcmp(text, "16") == 0) {
        *addr_width = 2;
        return true;
    }

    return false;
}

static bool parse_bus_arg(const char *text, int *bus_num)
{
    const char *num_text = text;
    uint32_t parsed;

    if (strncmp(text, "/dev/i2c-", 9) == 0) {
        num_text = text + 9;
    } else if (strncmp(text, "i2c-", 4) == 0) {
        num_text = text + 4;
    }

    if (!parse_u32(num_text, &parsed) || parsed > 4096) {
        return false;
    }

    *bus_num = (int)parsed;
    return true;
}

static size_t resolve_addr_width(size_t requested_width, uint32_t mem_addr)
{
    if (requested_width == 1 || requested_width == 2) {
        return requested_width;
    }

    return (mem_addr > 0xFF) ? 2 : 1;
}

static bool parse_data_list(const char *text, uint8_t *buffer, size_t expected_count)
{
    char *copy;
    char *token;
    size_t count = 0;

    if (text == NULL || buffer == NULL) {
        return false;
    }

    copy = strdup(text);
    if (copy == NULL) {
        return false;
    }

    token = strtok(copy, ",");
    while (token != NULL && count < expected_count) {
        uint8_t value;

        if (!parse_byte(token, &value)) {
            free(copy);
            return false;
        }
        buffer[count++] = value;
        token = strtok(NULL, ",");
    }

    free(copy);
    return count == expected_count && token == NULL;
}

static void print_hex_data(const uint8_t *buffer, size_t size)
{
    size_t i;

    for (i = 0; i < size; ++i) {
        printf("0x%02X", buffer[i]);
        if (i + 1 < size) {
            putchar(' ');
        }
        if (((i + 1) % 16) == 0 && (i + 1) < size) {
            putchar('\n');
        }
    }
    putchar('\n');
}

static int linux_i2c_open_bus(int bus_num)
{
    char path[64];

    snprintf(path, sizeof(path), "/dev/i2c-%d", bus_num);
    return open(path, O_RDWR);
}

static int linux_i2c_detect_ch34x_bus(void)
{
    DIR *dir;
    struct dirent *entry;

    dir = opendir("/sys/class/i2c-dev");
    if (dir == NULL) {
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        char name_path[256];
        FILE *fp;
        char name_buf[256];
        int bus_num;

        if (strncmp(entry->d_name, "i2c-", 4) != 0) {
            continue;
        }

        snprintf(name_path, sizeof(name_path), "/sys/class/i2c-dev/%s/name", entry->d_name);
        fp = fopen(name_path, "r");
        if (fp == NULL) {
            continue;
        }

        if (fgets(name_buf, sizeof(name_buf), fp) != NULL &&
            strstr(name_buf, "ch34x-mphsi-i2c") != NULL &&
            parse_bus_arg(entry->d_name, &bus_num)) {
            fclose(fp);
            closedir(dir);
            return bus_num;
        }

        fclose(fp);
    }

    closedir(dir);
    return -1;
}

static bool backend_probe_device(const cli_backend_t *backend, uint8_t addr)
{
    if (backend->use_linux_i2c) {
        struct i2c_msg msg = {
            .addr = addr,
            .flags = 0,
            .len = 0,
            .buf = NULL,
        };
        struct i2c_rdwr_ioctl_data ioctl_data = {
            .msgs = &msg,
            .nmsgs = 1,
        };

        return ioctl(backend->linux_fd, I2C_RDWR, &ioctl_data) >= 0;
    }

    return i2c_probe_device(addr);
}

static int backend_mem_read(const cli_backend_t *backend, uint8_t addr, uint16_t reg, size_t reg_width, uint8_t *data, size_t len)
{
    if (backend->use_linux_i2c) {
        uint8_t reg_buf[2];
        struct i2c_msg msgs[2];
        struct i2c_rdwr_ioctl_data ioctl_data;
        uint8_t reg_len = (uint8_t)reg_width;

        if (reg_width == 2) {
            reg_buf[0] = (uint8_t)((reg >> 8) & 0xFF);
            reg_buf[1] = (uint8_t)(reg & 0xFF);
        } else {
            reg_buf[0] = (uint8_t)(reg & 0xFF);
        }

        msgs[0].addr = addr;
        msgs[0].flags = 0;
        msgs[0].len = reg_len;
        msgs[0].buf = reg_buf;
        msgs[1].addr = addr;
        msgs[1].flags = I2C_M_RD;
        msgs[1].len = len;
        msgs[1].buf = data;

        ioctl_data.msgs = msgs;
        ioctl_data.nmsgs = 2;
        return (ioctl(backend->linux_fd, I2C_RDWR, &ioctl_data) >= 0) ? 0 : -1;
    }

    return i2c_mem_read_ex(addr, reg, reg_width, data, len);
}

static int backend_mem_write(const cli_backend_t *backend, uint8_t addr, uint16_t reg, size_t reg_width, const uint8_t *data, size_t len)
{
    if (backend->use_linux_i2c) {
        uint8_t buffer[MAX_TRANSFER_SIZE + 2];
        struct i2c_msg msg;
        struct i2c_rdwr_ioctl_data ioctl_data;
        size_t offset = 0;

        if ((reg_width + len) > sizeof(buffer)) {
            return -1;
        }

        if (reg_width == 2) {
            buffer[offset++] = (uint8_t)((reg >> 8) & 0xFF);
        }
        buffer[offset++] = (uint8_t)(reg & 0xFF);
        memcpy(&buffer[offset], data, len);
        offset += len;

        msg.addr = addr;
        msg.flags = 0;
        msg.len = offset;
        msg.buf = buffer;
        ioctl_data.msgs = &msg;
        ioctl_data.nmsgs = 1;
        return (ioctl(backend->linux_fd, I2C_RDWR, &ioctl_data) >= 0) ? 0 : -1;
    }

    return i2c_mem_write_ex(addr, reg, reg_width, data, len);
}

static int backend_open(cli_backend_t *backend, const global_options_t *options)
{
    int bus_num = options->linux_bus_num;

    memset(backend, 0, sizeof(*backend));
    backend->linux_fd = -1;

    if (bus_num < 0) {
        bus_num = linux_i2c_detect_ch34x_bus();
    }

    if (bus_num >= 0) {
        backend->linux_fd = linux_i2c_open_bus(bus_num);
        if (backend->linux_fd < 0) {
            fprintf(stderr, "Error: failed to open /dev/i2c-%d\n", bus_num);
            return -1;
        }
        backend->use_linux_i2c = true;
        if (options->verbose) {
            fprintf(stderr, "Using Linux i2c-dev backend on /dev/i2c-%d\n", bus_num);
        }
        return 0;
    }

    if (init_device_ex(options->dev_index, options->clock_speed, options->verbose) == 0) {
        backend->use_linux_i2c = false;
        return 0;
    }

    fprintf(stderr,
            "Hint: no /dev/ch34x_pis* device was found, and no CH34x Linux I2C bus was detected.\n"
            "Load ch34x_mphsi_master.ko and use -b <bus>, or install the WCH userspace driver.\n");
    return -1;
}

static void backend_close(cli_backend_t *backend)
{
    if (backend->use_linux_i2c) {
        if (backend->linux_fd >= 0) {
            close(backend->linux_fd);
        }
    } else {
        deinit_device();
    }
}

static int handle_scan(const cli_backend_t *backend, int argc, char **argv)
{
    uint8_t start = 0x03;
    uint8_t end = 0x77;
    int found = 0;
    uint32_t parsed;
    uint32_t addr;

    if (argc >= 1) {
        if (!parse_u32(argv[0], &parsed) || parsed > 0x7F) {
            fprintf(stderr, "Error: invalid start address '%s'\n", argv[0]);
            return 1;
        }
        start = (uint8_t)parsed;
    }

    if (argc >= 2) {
        if (!parse_u32(argv[1], &parsed) || parsed > 0x7F) {
            fprintf(stderr, "Error: invalid end address '%s'\n", argv[1]);
            return 1;
        }
        end = (uint8_t)parsed;
    }

    if (argc > 2) {
        fprintf(stderr, "Error: too many arguments for scan\n");
        return 1;
    }

    if (start > end) {
        fprintf(stderr, "Error: start address must be <= end address\n");
        return 1;
    }

    printf("Scanning I2C bus from 0x%02X to 0x%02X ...\n", start, end);
    for (addr = start; addr <= end; ++addr) {
        if (backend_probe_device(backend, (uint8_t)addr)) {
            printf("0x%02X: FOUND\n", (unsigned int)addr);
            found++;
        }
    }

    if (found == 0) {
        printf("No I2C devices found.\n");
    } else {
        printf("Found %d device(s).\n", found);
    }

    return 0;
}

static int handle_mem(const cli_backend_t *backend, int argc, char **argv, const global_options_t *options)
{
    uint32_t device_addr;
    uint32_t mem_addr;
    uint32_t size_u32;
    size_t addr_width;
    uint8_t buffer[MAX_TRANSFER_SIZE];

    if (argc < 4) {
        fprintf(stderr,
                "Usage: mem <r|w> <device_addr> <mem_addr> <size> [data0,data1,...]\n");
        return 1;
    }

    if (!parse_u32(argv[1], &device_addr) || device_addr > 0x7F) {
        fprintf(stderr, "Error: invalid device address '%s'\n", argv[1]);
        return 1;
    }

    if (!parse_u32(argv[2], &mem_addr) || mem_addr > 0xFFFF) {
        fprintf(stderr, "Error: invalid memory address '%s'\n", argv[2]);
        return 1;
    }

    if (!parse_u32(argv[3], &size_u32) || size_u32 == 0 || size_u32 > MAX_TRANSFER_SIZE) {
        fprintf(stderr, "Error: invalid size '%s' (1-%d)\n", argv[3], MAX_TRANSFER_SIZE);
        return 1;
    }

    addr_width = resolve_addr_width(options->addr_width, mem_addr);

    if (strcmp(argv[0], "r") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Error: mem r does not take write data\n");
            return 1;
        }

        if (backend_mem_read(backend, (uint8_t)device_addr, (uint16_t)mem_addr, addr_width, buffer, size_u32) != 0) {
            fprintf(stderr, "Error: I2C read failed\n");
            return 1;
        }

        printf("Read %u bytes from dev:0x%02X mem:0x%0*X, data:\n",
               size_u32,
               device_addr,
               (addr_width == 2) ? 4 : 2,
               mem_addr);
        print_hex_data(buffer, size_u32);
        return 0;
    }

    if (strcmp(argv[0], "w") == 0) {
        if (argc != 5) {
            fprintf(stderr, "Error: mem w requires write data in the form 0x11,0x22,...\n");
            return 1;
        }

        if (!parse_data_list(argv[4], buffer, size_u32)) {
            fprintf(stderr, "Error: write data count/content mismatch, expected %u byte(s)\n", size_u32);
            return 1;
        }

        if (backend_mem_write(backend, (uint8_t)device_addr, (uint16_t)mem_addr, addr_width, buffer, size_u32) != 0) {
            fprintf(stderr, "Error: I2C write failed\n");
            return 1;
        }

        printf("Wrote %u bytes to dev:0x%02X mem:0x%0*X\n",
               size_u32,
               device_addr,
               (addr_width == 2) ? 4 : 2,
               mem_addr);
        return 0;
    }

    fprintf(stderr, "Error: invalid mem operation '%s' (use r or w)\n", argv[0]);
    return 1;
}

int main(int argc, char **argv)
{
    global_options_t options = {
        .dev_index = -1,
        .clock_speed = 1,
        .verbose = 0,
        .addr_width = 0,
        .linux_bus_num = -1,
    };
    cli_backend_t backend;
    int opt;
    int option_index = 0;
    int ret = 1;
    static const struct option long_options[] = {
        {"index", required_argument, NULL, 'i'},
        {"bus", required_argument, NULL, 'b'},
        {"speed", required_argument, NULL, 's'},
        {"addr-width", required_argument, NULL, 'A'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };

    while ((opt = getopt_long(argc, argv, "i:b:s:A:vh", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'i':
            options.dev_index = (int)strtol(optarg, NULL, 0);
            break;
        case 'b':
            if (!parse_bus_arg(optarg, &options.linux_bus_num)) {
                fprintf(stderr, "Error: invalid bus '%s'\n", optarg);
                return 1;
            }
            break;
        case 's':
            options.clock_speed = (int)strtol(optarg, NULL, 0);
            break;
        case 'A':
            if (!parse_addr_width(optarg, &options.addr_width)) {
                fprintf(stderr, "Error: invalid addr width '%s', use auto, 8, or 16\n", optarg);
                return 1;
            }
            break;
        case 'v':
            options.verbose = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (optind >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    if (backend_open(&backend, &options) != 0) {
        return 1;
    }

    if (strcmp(argv[optind], "scan") == 0) {
        ret = handle_scan(&backend, argc - optind - 1, &argv[optind + 1]);
    } else if (strcmp(argv[optind], "mem") == 0) {
        ret = handle_mem(&backend, argc - optind - 1, &argv[optind + 1], &options);
    } else {
        fprintf(stderr, "Error: unknown command '%s'\n", argv[optind]);
        print_usage(argv[0]);
    }

    backend_close(&backend);

    return ret;
}
