#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "image.h"
#include "platform_i2c_driver.h"

/*
 * A/B slot FWU host (MCU memmap VERSION >= 0x0300).
 *
 * The MCU writes the slot it is not running from, in the background, and the
 * switch is a warm restart: the A510 keeps its power and the PCIe link stays
 * up. Interrupting this tool at any point only leaves the inactive slot
 * unfinished; the running firmware is never touched until ACTIVATE.
 *
 * One image is released per build, linked for slot A. When the MCU runs from
 * slot A this tool sends it as is; when it runs from slot B the image is
 * relocated in memory (image_relocate) before it is sent.
 * Protocol: a510_mcu/doc/interface/host_fwu_guide.md.
 */

#define ASSERT_FATAL(expr, fmt, ...) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        exit(EXIT_FAILURE); \
    } \
} while(0)

/* Exit code contract: 2 means "the image was rejected and the MCU was never
 * touched", 1 means "the files were fine but the device or the invocation
 * failed". Scripts use the difference to tell a bad file from a bad fixture. */
#define EXIT_IMAGE_CHECK_FAILED 2

#define I2C_ADDR 0x12
#define BOOT_STATE_ADDR 0x01
#define MEM_DESC_VERSION_ADDR 0x02
#define BOARD_ID_ADDR 0x52
#define FWU_SLOT_ADDR 0xfa
#define FWU_STATUS_ADDR 0xfb
#define FWU_FIFO_LENGTH_ADDR 0xfc
#define FWU_FIFO_ADDR 0xfe
#define FWU_CONTROL_ADDR 0xff

#define BOOT_STATE_APP 0x01

/* Memmap version of the A/B slot protocol (FWU_SLOT, FWU_CMD_ACTIVATE). */
#define MEM_DESC_VERSION_AB_FWU 0x0300

#define FWU_CMD_REQUEST 0x01
#define FWU_CMD_FILE_SEND_DONE 0x03
#define FWU_CMD_ACTIVATE 0x04

#define FWU_STATUS_FIFO_NOT_EMPTY (1u << 0)
#define FWU_STATUS_FIFO_OVERFLOW (1u << 1)
#define FWU_STATUS_BUSY (1u << 2)
#define FWU_STATUS_READY (1u << 3)
#define FWU_STATUS_ERROR (1u << 4)

/* REQUEST waits until the running slot has confirmed itself (up to 30 s after
 * the MCU started) and then erases the whole target slot. */
#define REQUEST_TIMEOUT_MS 120000
#define DRAIN_TIMEOUT_MS 5000
#define VERIFY_TIMEOUT_MS 30000
#define ACTIVATE_TIMEOUT_MS 30000

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int read_u8(uint8_t reg, uint8_t *value)
{
    return platform_i2c_driver.read(I2C_ADDR, reg, value, 1);
}

static uint16_t read_u16_le(uint8_t reg, const char *what)
{
    uint8_t bytes[2];

    ASSERT_FATAL(platform_i2c_driver.read(I2C_ADDR, reg, bytes, sizeof bytes) == 0,
                 "Failed to read %s (reg 0x%02x): %s", what, reg, strerror(errno));
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void send_fwu_cmd(uint8_t cmd)
{
    ASSERT_FATAL(platform_i2c_driver.write(I2C_ADDR, FWU_CONTROL_ADDR, (uint8_t[]){cmd}, 1) == 0,
                 "Failed to send command 0x%02X: %s", cmd, strerror(errno));
}

/* Poll FWU_STATUS until none of `busy_bits` is set; any error bit is fatal. */
static uint8_t wait_status(uint8_t busy_bits, int timeout_ms, const char *what)
{
    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    uint8_t status = 0;

    for (;;) {
        if (read_u8(FWU_STATUS_ADDR, &status) == 0) {
            ASSERT_FATAL(!(status & (FWU_STATUS_ERROR | FWU_STATUS_FIFO_OVERFLOW)),
                         "MCU reported FWU_STATUS=0x%02x (%s); the running firmware is "
                         "untouched, run the update again", status, what);
            if (!(status & busy_bits)) {
                return status;
            }
        }
        ASSERT_FATAL(now_ms() < deadline, "Timeout waiting for the MCU (%s, FWU_STATUS=0x%02x)",
                     what, status);
        usleep(1000);
    }
}

static void send_buffer(uint8_t *data, size_t size, uint16_t chunk, const char *what)
{
    int last_percent = -1;

    for (size_t offset = 0; offset < size; offset += chunk) {
        size_t n = (size - offset > chunk) ? chunk : size - offset;

        wait_status(FWU_STATUS_FIFO_NOT_EMPTY | FWU_STATUS_BUSY, DRAIN_TIMEOUT_MS,
                    "FIFO drain");
        ASSERT_FATAL(platform_i2c_driver.write(I2C_ADDR, FWU_FIFO_ADDR, data + offset, n) == 0,
                     "I2C write failed (%s): reg=0x%02x offset=%zu len=%zu: %s", what,
                     FWU_FIFO_ADDR, offset, n, strerror(errno));
        int percent = (int)((offset + n) * 100 / size);
        if (percent != last_percent) {
            printf("\r%3d%%", percent);
            fflush(stdout);
            last_percent = percent;
        }
    }
    printf("\n");
}

/* FWU_SLOT holds the running slot: 0 = A, 1 = B, 0x0F = bootloader. The MCU
 * always writes the other slot (slot A from the bootloader). */
static unsigned check_mcu(const image_info_t *image)
{
    uint16_t version = read_u16_le(MEM_DESC_VERSION_ADDR, "memmap version");

    if (version < MEM_DESC_VERSION_AB_FWU) {
        fprintf(stderr,
                "MCU firmware memmap version is 0x%04x; the A/B slot protocol needs >= 0x%04x.\n"
                "The flash layout changed: flash a510_mcu build/<board>/flash.bin over SWD "
                "once, then use this tool.\n",
                version, MEM_DESC_VERSION_AB_FWU);
        exit(EXIT_FAILURE);
    }

    uint16_t board = read_u16_le(BOARD_ID_ADDR, "board id");
    const char *mcu_name = image_board_name(board);

    ASSERT_FATAL(mcu_name != NULL, "BOARD_ID reads 0x%04x, which is not a board identity", board);
    if (image->board_id != board) {
        fprintf(stderr, "board mismatch: the MCU is a %s (BOARD_ID %u), but the image is for "
                "%s (%u)\n", mcu_name, board, image->board_name, image->board_id);
        exit(EXIT_IMAGE_CHECK_FAILED);
    }

    uint8_t running;
    ASSERT_FATAL(read_u8(FWU_SLOT_ADDR, &running) == 0, "Failed to read FWU_SLOT: %s",
                 strerror(errno));
    ASSERT_FATAL((running <= 1) || (running == 0x0Fu), "FWU_SLOT 0x%02x is not a slot",
                 running);

    /* Bootloader rescue restores slot A; otherwise the slot not running. */
    unsigned target = (running == 0x0Fu) ? 0U : 1U - running;
    printf("     board: %s (BOARD_ID=0x%04x, memmap version=0x%04x), running %s, "
           "writing slot %c\n", mcu_name, board, version,
           running == 0x0Fu ? "the bootloader (rescue)" : (running ? "slot B" : "slot A"),
           'A' + target);
    return target;
}

/* After ACTIVATE the MCU is off the bus for a moment; wait for the new slot. */
static void wait_activated(unsigned target)
{
    uint64_t deadline = now_ms() + ACTIVATE_TIMEOUT_MS;
    uint8_t state, slot;

    while (now_ms() < deadline) {
        usleep(100000);
        if (read_u8(BOOT_STATE_ADDR, &state) == 0 && state == BOOT_STATE_APP &&
            read_u8(FWU_SLOT_ADDR, &slot) == 0 && slot == target) {
            printf("     slot %c running; it confirms itself after 30 s, a crash before that "
                   "rolls back\n", 'A' + target);
            return;
        }
    }
    fprintf(stderr, "slot %c did not come up; the bootloader kept the previous firmware\n",
            'A' + target);
    exit(EXIT_FAILURE);
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <app.bin>\n"
                    "  app.bin: a510_mcu build/<board>/app/app.bin\n", prog);
}

int main(int argc, char **argv)
{
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc != 2) {
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    // The image is checked before the first I2C access, so a bad file leaves the
    // board running whatever it already has.
    static uint8_t data[IMAGE_SLOT_SIZE];
    image_info_t image;
    char err[512];

    if (!image_load(argv[1], data, &image, err, sizeof err)) {
        fprintf(stderr, "image rejected: %s\n", err);
        return EXIT_IMAGE_CHECK_FAILED;
    }
    printf("image check passed:\n");
    image_print_summary("app", argv[1], &image);

    ASSERT_FATAL(platform_i2c_driver.init() == 0, "Failed to initialize I2C driver");
    unsigned target = check_mcu(&image);
    uint16_t chunk = read_u16_le(FWU_FIFO_LENGTH_ADDR, "FIFO length");
    ASSERT_FATAL(chunk != 0, "FWU_FIFO_LENGTH reads 0");

    /* Slot A gets the file as built; slot B the same bytes relocated in memory.
     * The file on disk is never modified. */
    if (target == 1U) {
        image_relocate(&image, data);
        printf("     relocated %" PRIu32 " addresses for slot B\n", image.reloc_count);
    }

    printf("     erasing slot %c (waits for the running slot to confirm itself)... ",
           'A' + target);
    fflush(stdout);
    send_fwu_cmd(FWU_CMD_REQUEST);
    wait_status(FWU_STATUS_BUSY, REQUEST_TIMEOUT_MS, "erase");
    printf("done\n     writing... ");
    send_buffer(data, (size_t)image.file_size, chunk, target ? "relocated image" : argv[1]);

    send_fwu_cmd(FWU_CMD_FILE_SEND_DONE);
    uint8_t status = wait_status(FWU_STATUS_FIFO_NOT_EMPTY | FWU_STATUS_BUSY,
                                 VERIFY_TIMEOUT_MS, "verify");
    ASSERT_FATAL(status & FWU_STATUS_READY, "MCU did not accept the image (FWU_STATUS=0x%02x)",
                 status);

    printf("     verified, activating slot %c\n", 'A' + target);
    send_fwu_cmd(FWU_CMD_ACTIVATE);
    wait_activated(target);

    ASSERT_FATAL(platform_i2c_driver.deinit() == 0, "Failed to deinitialize I2C driver");
    return 0;
}
