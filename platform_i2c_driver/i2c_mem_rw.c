#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <getopt.h>
#include <dirent.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include "ch34x_lib.h"

// 声明为extern，不再重复定义
extern int dev_fd;

static int g_dev_index = -1;
static int g_clock_speed = 1;
static int g_verbose = 0;
static int g_linux_i2c_fd = -1;
static int g_linux_i2c_bus = -1;

typedef enum {
    I2C_BACKEND_NONE = 0,
    I2C_BACKEND_CH34X_CHARDEV,
    I2C_BACKEND_LINUX_I2CDEV,
} i2c_backend_t;

static i2c_backend_t g_backend = I2C_BACKEND_NONE;

// 定义最大数据缓冲区大小 (1MB)
#define MAX_DATA_SIZE (1024 * 1024)
#define MAX_DEVICES 10
// 单次传输最大字节数 (CH341A 硬件限制约 4KB，保守使用 1KB)
#define I2C_CHUNK_SIZE 1000
// FWU 场景下 Linux i2c-dev 路径更适合保守分块，优先稳而不是快。
#define LINUX_I2C_CHUNK_SIZE 128

static size_t get_linux_i2c_chunk_limit(void)
{
    const char *env = getenv("A510_I2C_CHUNK_SIZE");
    char *end = NULL;
    unsigned long value;

    if (env == NULL || *env == '\0') {
        return LINUX_I2C_CHUNK_SIZE;
    }

    value = strtoul(env, &end, 0);
    if (end == env || *end != '\0' || value == 0 || value > LINUX_I2C_CHUNK_SIZE) {
        return LINUX_I2C_CHUNK_SIZE;
    }

    return (size_t)value;
}

static int parse_bus_number(const char *text)
{
    const char *num_text = text;
    char *end = NULL;
    long value;

    if (text == NULL || *text == '\0') {
        return -1;
    }

    if (strncmp(text, "/dev/i2c-", 9) == 0) {
        num_text = text + 9;
    } else if (strncmp(text, "i2c-", 4) == 0) {
        num_text = text + 4;
    }

    errno = 0;
    value = strtol(num_text, &end, 0);
    if (errno != 0 || end == num_text || *end != '\0' || value < 0 || value > 4096) {
        return -1;
    }

    return (int)value;
}

static int detect_linux_i2c_bus(void)
{
    DIR *dir;
    struct dirent *entry;

    dir = opendir("/sys/class/i2c-dev");
    if (dir == NULL) {
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[256];
        FILE *fp;
        char name[256];
        int bus_num;

        if (strncmp(entry->d_name, "i2c-", 4) != 0) {
            continue;
        }

        bus_num = parse_bus_number(entry->d_name);
        if (bus_num < 0) {
            continue;
        }

        snprintf(path, sizeof(path), "/sys/class/i2c-dev/i2c-%d/name", bus_num);
        fp = fopen(path, "r");
        if (fp == NULL) {
            continue;
        }

        if (fgets(name, sizeof(name), fp) != NULL && strstr(name, "ch34x-mphsi-i2c") != NULL) {
            fclose(fp);
            closedir(dir);
            return bus_num;
        }

        fclose(fp);
    }

    closedir(dir);
    return -1;
}

static int init_linux_i2c_backend(int verbose)
{
    const char *bus_env = getenv("A510_I2C_BUS");
    char path[64];
    int bus_num;
    int requested_bus_num;

    requested_bus_num = (bus_env != NULL) ? parse_bus_number(bus_env) : -1;
    bus_num = requested_bus_num;
    if (bus_num < 0) {
        bus_num = detect_linux_i2c_bus();
    }
    if (bus_num < 0) {
        return -1;
    }

    snprintf(path, sizeof(path), "/dev/i2c-%d", bus_num);
    g_linux_i2c_fd = open(path, O_RDWR);
    if (g_linux_i2c_fd < 0) {
        int detected_bus_num = -1;

        if (requested_bus_num >= 0) {
            detected_bus_num = detect_linux_i2c_bus();
        }

        if (detected_bus_num >= 0 && detected_bus_num != requested_bus_num) {
            snprintf(path, sizeof(path), "/dev/i2c-%d", detected_bus_num);
            g_linux_i2c_fd = open(path, O_RDWR);
            if (g_linux_i2c_fd >= 0) {
                bus_num = detected_bus_num;
                if (verbose) {
                    fprintf(stderr,
                            "警告: 指定的 i2c-%d 不可用，已回退到 i2c-%d\n",
                            requested_bus_num,
                            detected_bus_num);
                }
            }
        }

        if (g_linux_i2c_fd < 0) {
            fprintf(stderr, "错误: 无法打开 %s\n", path);
            return -1;
        }
    }

    g_linux_i2c_bus = bus_num;
    g_backend = I2C_BACKEND_LINUX_I2CDEV;

    if (verbose) {
        fprintf(stderr, "已打开 Linux I2C 总线 %s\n", path);
    }

    return 0;
}

static int linux_i2c_rdwr(const struct i2c_msg *msgs, int nmsgs)
{
    struct i2c_rdwr_ioctl_data ioctl_data;

    if (g_linux_i2c_fd < 0) {
        return -1;
    }

    ioctl_data.msgs = (struct i2c_msg *)msgs;
    ioctl_data.nmsgs = nmsgs;
    return (ioctl(g_linux_i2c_fd, I2C_RDWR, &ioctl_data) >= 0) ? 0 : -1;
}

// 扫描可用的 CH34x 设备，返回设备数量，devices[] 存储设备索引
static int scan_ch34x_devices(int devices[], int max_count) {
    DIR *dir;
    struct dirent *entry;
    int count = 0;

    dir = opendir("/dev");
    if (!dir) {
        return 0;
    }

    while ((entry = readdir(dir)) != NULL && count < max_count) {
        if (strncmp(entry->d_name, "ch34x_pis", 9) == 0) {
            int index = entry->d_name[9] - '0';
            if (index >= 0 && index <= 9) {
                devices[count++] = index;
            }
        }
    }

    closedir(dir);
    return count;
}

// 自动选择设备，返回设备索引，失败返回 -1
static int auto_select_device(int specified_index) {
int devices[MAX_DEVICES];
int count = scan_ch34x_devices(devices, MAX_DEVICES);

if (count == 0) {
    fprintf(stderr, "错误: 未找到 CH34x 设备\n");
    return -1;
}

// 如果用户指定了索引
if (specified_index >= 0) {
    for (int i = 0; i < count; i++) {
        if (devices[i] == specified_index) {
            return specified_index;
        }
    }
    fprintf(stderr, "错误: 指定的设备 /dev/ch34x_pis%d 不存在\n", specified_index);
    fprintf(stderr, "可用设备: ");
    for (int i = 0; i < count; i++) {
        fprintf(stderr, "/dev/ch34x_pis%d ", devices[i]);
    }
    fprintf(stderr, "\n");
    return -1;
}

// 只有一个设备，自动选择
if (count == 1) {
    return devices[0];
}

// 多个设备，需要用户指定
fprintf(stderr, "错误: 发现 %d 个 CH34x 设备，请使用 -i 参数指定:\n", count);
for (int i = 0; i < count; i++) {
    fprintf(stderr, "  /dev/ch34x_pis%d\n", devices[i]);
}
return -1;
}

// 命令行选项
typedef struct {
    int operation;      // 0:读, 1:写
    int device_addr;    // 设备地址
    int reg_addr;       // 寄存器地址
    size_t length;      // 读取/写入长度
    int clock_speed;    // I2C时钟速度 (0-3)
    int dev_index;      // CH34x 设备索引 (-1 表示自动选择)
    unsigned char data[MAX_DATA_SIZE]; // 写入数据缓冲区
    int verbose;        // 详细输出
    char *input_file;   // 输入文件路径
} cmd_options_t;

// 从文件读取数据，返回读取字节数，失败返回 0
size_t load_file_data(const char *filepath, unsigned char *buffer, size_t max_size) {
FILE *fp = fopen(filepath, "rb");
if (!fp) {
    fprintf(stderr, "错误: 无法打开文件 %s\n", filepath);
    return 0;
}

size_t count = fread(buffer, 1, max_size, fp);
fclose(fp);

if (count == 0) {
    fprintf(stderr, "错误: 文件为空或读取失败\n");
}

return count;
}

// 显示使用帮助
void show_help(char *progname) {
    printf("用法: %s [选项]\n", progname);
    printf("选项:\n");
    printf("  -r          读操作\n");
    printf("  -w          写操作\n");
    printf("  -d ADDR     设备地址 (十六进制, 例如: 0x50)\n");
    printf("  -a ADDR     寄存器地址 (十六进制, 例如: 0x00)\n");
    printf("  -l LEN      读取长度 (十进制, 默认: 1)\n");
    printf("  -s SPEED    I2C时钟 (0:20k, 1:100k, 2:400k, 3:750k)\n");
    printf("  -i NUM      CH34x设备索引 (默认: 自动)\n");
    printf("  -v DATA     十六进制数据 (例如: 0x11,0x22)\n");
    printf("  -t TEXT     文本字符串 (例如: \"hello world\")\n");
    printf("  -f FILE     从文件读取数据\n");
    printf("  -V          详细输出\n");
    printf("  -h          显示帮助\n");
    printf("\n示例:\n");
    printf("  %s -r -d 0x50 -a 0x00 -l 8\n", progname);
    printf("  %s -w -d 0x50 -a 0x00 -v 0x11,0x22\n", progname);
    printf("  %s -w -d 0x50 -a 0x00 -t \"hello\"\n", progname);
    printf("  %s -w -d 0x50 -a 0x00 -f data.bin\n", progname);
}

// 解析命令行参数
int parse_options(int argc, char **argv, cmd_options_t *options) {
    int c;
    char *data_str = NULL;
    char *token;
    size_t count = 0;
    
    // 设置默认值
    options->operation = -1;
    options->device_addr = -1;
    options->reg_addr = -1;
    options->length = 1;
    options->clock_speed = 1; // 默认100kHz
    options->dev_index = -1;  // -1 表示自动选择
    options->verbose = 0;
    options->input_file = NULL;
    
    while ((c = getopt(argc, argv, "rwd:a:l:s:i:v:t:f:Vh")) != -1) {
        switch (c) {
            case 'r':
                options->operation = 0;
                break;
                
            case 'w':
                options->operation = 1;
                break;
                
            case 'd':
                options->device_addr = (int)strtol(optarg, NULL, 0);
                break;
                
            case 'a':
                options->reg_addr = (int)strtol(optarg, NULL, 0);
                break;
                
            case 'l':
                options->length = (size_t)strtoul(optarg, NULL, 0);
                if (options->length > MAX_DATA_SIZE) {
                    fprintf(stderr, "错误: 长度超过最大值 %zu\n", (size_t)MAX_DATA_SIZE);
                    return -1;
                }
                break;
                
            case 's':
                options->clock_speed = (int)strtol(optarg, NULL, 0);
                if (options->clock_speed < 0 || options->clock_speed > 3) {
                    fprintf(stderr, "错误: 时钟速度必须在0-3范围内\n");
                    return -1;
                }
                break;
            
            case 'i':
                options->dev_index = (int)strtol(optarg, NULL, 0);
                break;
                
            case 'v':
                data_str = strdup(optarg);
                token = strtok(data_str, ",");
                while (token != NULL && count < MAX_DATA_SIZE) {
                    options->data[count++] = (unsigned char)strtol(token, NULL, 0);
                    token = strtok(NULL, ",");
                }
                options->length = count;
                free(data_str);
                break;
            
            case 't':
                // 文本字符串直接复制为数据
                count = strlen(optarg);
                if (count > MAX_DATA_SIZE) count = MAX_DATA_SIZE;
                memcpy(options->data, optarg, count);
                options->length = count;
                break;
            
            case 'f':
                options->input_file = optarg;
                break;
                
            case 'V':
                options->verbose = 1;
                break;
                
            case 'h':
                show_help(argv[0]);
                return -2;
                
            case '?':
                return -1;
                
            default:
                abort();
        }
    }
    
    // 验证必要参数
    if (options->operation == -1) {
        fprintf(stderr, "错误: 必须指定操作类型 (-r 或 -w)\n");
        return -1;
    }
    
    if (options->device_addr == -1) {
        fprintf(stderr, "错误: 必须指定设备地址 (-d)\n");
        return -1;
    }
    
    if (options->reg_addr == -1) {
        fprintf(stderr, "错误: 必须指定寄存器地址 (-a)\n");
        return -1;
    }
    
    // 如果指定了文件，从文件加载数据
    if (options->input_file) {
        size_t len = load_file_data(options->input_file, options->data, MAX_DATA_SIZE);
        if (len == 0) return -1;
        options->length = len;
    }
    
    return 0;
}

int init_device_ex(int dev_index, int clock_speed, int verbose)
{
    int selected;

    if (init_linux_i2c_backend(verbose) == 0) {
        g_dev_index = -1;
        g_clock_speed = clock_speed;
        g_verbose = verbose;
        return 0;
    }

    if (clock_speed < 0 || clock_speed > 3) {
        fprintf(stderr, "错误: 时钟速度必须在0-3范围内\n");
        return -1;
    }

    selected = auto_select_device(dev_index);
    if (selected < 0) {
        return -1;
    }

    // 打开设备
    dev_fd = CH34xOpenDevice(selected);
    if (dev_fd <= 0) {
        fprintf(stderr, "打开CH341A设备失败\n");
        return -1;
    }
    
    // 获取设备版本信息
    PUCHAR VendorId = (PUCHAR)malloc(sizeof(long));
    if (VendorId == NULL) {
        fprintf(stderr, "错误: 内存分配失败\n");
        CH34xCloseDevice();
        return -1;
    }

    if (!CH34x_GetVendorId(VendorId)) {
        fprintf(stderr, "获取设备信息失败\n");
        free(VendorId);
        CH34xCloseDevice();
        return -1;
    }
    
    // 设置I2C模式和速度
    // 正确设置值为:
    // 0x00: 20kHz (默认)
    // 0x01: 100kHz
    // 0x02: 400kHz
    // 0x03: 750kHz
    if (!CH34xSetStream(clock_speed)) {
        fprintf(stderr, "设置I2C模式失败\n");
        free(VendorId);
        CH34xCloseDevice();
        return -1;
    }

    g_dev_index = selected;
    g_clock_speed = clock_speed;
    g_verbose = verbose;
    g_backend = I2C_BACKEND_CH34X_CHARDEV;

    if (g_verbose) {
        fprintf(stderr,
                "已打开 /dev/ch34x_pis%d, I2C速度=%d (%s)\n",
                g_dev_index,
                g_clock_speed,
                (g_clock_speed == 0) ? "20kHz" :
                (g_clock_speed == 1) ? "100kHz" :
                (g_clock_speed == 2) ? "400kHz" : "750kHz");
    }

    free(VendorId);
    
    return 0;
}

// 初始化CH341A设备
int init_device()
{
    return init_device_ex(-1, 1, 0);
}

int deinit_device()
{
    if (g_backend == I2C_BACKEND_LINUX_I2CDEV) {
        if (g_linux_i2c_fd >= 0) {
            close(g_linux_i2c_fd);
        }
        g_linux_i2c_fd = -1;
        g_linux_i2c_bus = -1;
    } else if (g_backend == I2C_BACKEND_CH34X_CHARDEV) {
        CH34xCloseDevice();
    }

    g_backend = I2C_BACKEND_NONE;
    return 0;
}

static int i2c_mem_transfer(uint8_t addr, uint16_t reg, size_t reg_width, const uint8_t *tx_data, size_t tx_len, uint8_t *rx_data, size_t rx_len)
{
    if (g_backend == I2C_BACKEND_LINUX_I2CDEV) {
        uint8_t reg_buf[2];
        uint8_t write_buf[I2C_CHUNK_SIZE + 2];
        struct i2c_msg msgs[2];
        int nmsgs = 0;

        if (tx_len > I2C_CHUNK_SIZE || rx_len > I2C_CHUNK_SIZE) {
            fprintf(stderr, "错误: Linux I2C 单次传输长度过大\n");
            return -1;
        }

        if (reg_width == 2) {
            reg_buf[0] = (uint8_t)((reg >> 8) & 0xFF);
            reg_buf[1] = (uint8_t)(reg & 0xFF);
        } else if (reg_width == 1) {
            reg_buf[0] = (uint8_t)(reg & 0xFF);
        } else {
            fprintf(stderr, "错误: 仅支持8位或16位寄存器地址\n");
            return -1;
        }

        if (tx_len > 0) {
            size_t write_len = 0;

            if ((reg_width + tx_len) > sizeof(write_buf)) {
                fprintf(stderr, "错误: Linux I2C 单次写入长度过大\n");
                return -1;
            }

            if (reg_width == 2) {
                write_buf[write_len++] = reg_buf[0];
            }
            write_buf[write_len++] = reg_buf[reg_width - 1];
            memcpy(&write_buf[write_len], tx_data, tx_len);
            write_len += tx_len;

            msgs[nmsgs].addr = addr;
            msgs[nmsgs].flags = 0;
            msgs[nmsgs].len = (uint16_t)write_len;
            msgs[nmsgs].buf = write_buf;
            nmsgs++;
        } else {
            msgs[nmsgs].addr = addr;
            msgs[nmsgs].flags = 0;
            msgs[nmsgs].len = (uint16_t)reg_width;
            msgs[nmsgs].buf = reg_buf;
            nmsgs++;
        }

        if (rx_len > 0) {
            msgs[nmsgs].addr = addr;
            msgs[nmsgs].flags = I2C_M_RD;
            msgs[nmsgs].len = (uint16_t)rx_len;
            msgs[nmsgs].buf = rx_data;
            nmsgs++;
        }

        return linux_i2c_rdwr(msgs, nmsgs);
    } else {
        uint8_t write_buf[I2C_CHUNK_SIZE + 3];
        size_t write_len = 1;

        if (reg_width != 1 && reg_width != 2) {
            fprintf(stderr, "错误: 仅支持8位或16位寄存器地址\n");
            return -1;
        }

        if ((tx_len + reg_width + 1) > sizeof(write_buf)) {
            fprintf(stderr, "错误: 单次I2C传输长度过大 (%zu)\n", tx_len + reg_width + 1);
            return -1;
        }

        write_buf[0] = (uint8_t)(addr << 1);
        if (reg_width == 2) {
            write_buf[write_len++] = (uint8_t)((reg >> 8) & 0xFF);
        }
        write_buf[write_len++] = (uint8_t)(reg & 0xFF);

        if (tx_len > 0 && tx_data != NULL) {
            memcpy(&write_buf[write_len], tx_data, tx_len);
            write_len += tx_len;
        }

        if (!CH34xStreamI2C((unsigned long)write_len, write_buf, (unsigned long)rx_len, rx_data)) {
            return -1;
        }

        return 0;
    }
}

// 写入I2C内存数据 (支持大文件分块传输)
int i2c_mem_write(unsigned char addr, unsigned char reg, unsigned char* data, size_t len)
{
    size_t offset = 0;
    size_t chunk_num = 0;
    size_t chunk_limit = (g_backend == I2C_BACKEND_LINUX_I2CDEV) ? get_linux_i2c_chunk_limit() : I2C_CHUNK_SIZE;
    
    while (offset < len) {
        size_t chunk_len = (len - offset > chunk_limit) ? chunk_limit : (len - offset);

        if (i2c_mem_transfer(addr,
                             (uint16_t)((uint16_t)reg + (uint16_t)offset),
                             1,
                             data + offset,
                             chunk_len,
                             NULL,
                             0) != 0) {
            fprintf(stderr, "I2C写入失败 (块 %zu, 偏移 %zu)\n", chunk_num, offset);
            return -1;
        }
        
        // 等待设备处理 (给MCU足够时间处理FIFO数据)
    //  CH34xSetDelaymS(50);
        
        offset += chunk_len;
        chunk_num++;
    }
    
    return 0;
}

// 读取I2C内存数据
int i2c_mem_read(unsigned char addr, unsigned char reg, unsigned char* data, size_t len)
{
    if (i2c_mem_transfer(addr, reg, 1, NULL, 0, data, len) != 0) {
        fprintf(stderr, "I2C读取失败\n");
        return -1;
    }
    
    return 0;
}

int i2c_mem_write_ex(uint8_t addr, uint16_t reg, size_t reg_width, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return 0;
    }

    return i2c_mem_transfer(addr, reg, reg_width, data, len, NULL, 0);
}

int i2c_mem_read_ex(uint8_t addr, uint16_t reg, size_t reg_width, uint8_t *data, size_t len)
{
    if (len == 0) {
        return 0;
    }

    return i2c_mem_transfer(addr, reg, reg_width, NULL, 0, data, len);
}

bool i2c_probe_device(uint8_t addr)
{
    if (g_backend == I2C_BACKEND_LINUX_I2CDEV) {
        struct i2c_msg msg = {
            .addr = addr,
            .flags = 0,
            .len = 0,
            .buf = NULL,
        };

        return linux_i2c_rdwr(&msg, 1) == 0;
    } else {
        uint8_t write_buf = (uint8_t)(addr << 1);

        return CH34xStreamI2C(1, &write_buf, 0, NULL) != 0;
    }
}

size_t i2c_transport_chunk_size(void)
{
    return (g_backend == I2C_BACKEND_LINUX_I2CDEV) ? get_linux_i2c_chunk_limit() : I2C_CHUNK_SIZE;
}
