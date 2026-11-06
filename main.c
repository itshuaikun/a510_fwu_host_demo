#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
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

static int send_file(const char *filename, uint16_t chunk)
{
    FILE *fp = fopen(filename, "rb");
    uint8_t *buf = malloc(chunk);
    size_t file_size, offset = 0;
    int last_percent = -1;

    ASSERT_FATAL(fp != NULL && buf != NULL, "Failed to open %s", filename);
    fseek(fp, 0, SEEK_END);
    file_size = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    while (offset < file_size) {
        size_t n = (file_size - offset > chunk) ? chunk : file_size - offset;

        ASSERT_FATAL(fread(buf, 1, n, fp) == n, "Failed to read %s at offset %zu",
                     filename, offset);
        wait_status(FWU_STATUS_FIFO_NOT_EMPTY | FWU_STATUS_BUSY, DRAIN_TIMEOUT_MS,
                    "FIFO drain");
        ASSERT_FATAL(platform_i2c_driver.write(I2C_ADDR, FWU_FIFO_ADDR, buf, n) == 0,
                     "I2C write failed: reg=0x%02x offset=%zu len=%zu: %s", FWU_FIFO_ADDR,
                     offset, n, strerror(errno));
        offset += n;
        int percent = (int)(offset * 100 / file_size);
        if (percent != last_percent) {
            printf("\r%3d%%", percent);
            fflush(stdout);
            last_percent = percent;
        }
    }
    printf("\n");
    fclose(fp);
    free(buf);
    return 0;
}

/* Returns the slot the MCU will write (FWU_SLOT low nibble). */
static unsigned check_mcu(const image_info_t images[2], uint8_t *running)
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
    if (images[0].board_id != board) {
        fprintf(stderr, "board mismatch: the MCU is a %s (BOARD_ID %u), but the images are for "
                "%s (%u)\n", mcu_name, board, images[0].board_name, images[0].board_id);
        exit(EXIT_IMAGE_CHECK_FAILED);
    }

    uint8_t slot;
    ASSERT_FATAL(read_u8(FWU_SLOT_ADDR, &slot) == 0, "Failed to read FWU_SLOT: %s",
                 strerror(errno));
    unsigned target = slot & 0x0Fu;
    ASSERT_FATAL(target < 2, "FWU_SLOT 0x%02x names no target slot", slot);
    *running = (uint8_t)(slot >> 4);

    printf("     board: %s (BOARD_ID=0x%04x, memmap version=0x%04x), running %s, "
           "writing slot %c\n", mcu_name, board, version,
           *running == 0x0F ? "the bootloader (rescue)" : (*running ? "slot B" : "slot A"),
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
            read_u8(FWU_SLOT_ADDR, &slot) == 0 && (slot >> 4) == target) {
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
    fprintf(stderr, "Usage: %s <slot_a.bin> <slot_b.bin>\n"
                    "  slot_a.bin: a510_mcu build/<board>/app/app.bin\n"
                    "  slot_b.bin: a510_mcu build/<board>/app_b/app.bin\n", prog);
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

    // Both files are checked before the first I2C access, so a bad file leaves
    // the board running whatever it already has.
    image_info_t images[2];
    char err[512];

    for (unsigned i = 0; i < 2; i++) {
        if (!image_check_file(argv[1 + i], (image_role_t)i, &images[i], err, sizeof err)) {
            fprintf(stderr, "slot %c image rejected: %s\n", 'A' + i, err);
            return EXIT_IMAGE_CHECK_FAILED;
        }
    }
    if (images[0].board_id != images[1].board_id) {
        fprintf(stderr,
                "image pair mismatch: slot A is for %s (%u), slot B for %s (%u); both must be "
                "built for the same board\n", images[0].board_name, images[0].board_id,
                images[1].board_name, images[1].board_id);
        return EXIT_IMAGE_CHECK_FAILED;
    }

    printf("image check passed:\n");
    image_print_summary("slot A", argv[1], &images[0]);
    image_print_summary("slot B", argv[2], &images[1]);
    if (strcmp(images[0].git_sha, images[1].git_sha) != 0) {
        printf("  note: the two images come from different commits (%s vs %s)\n",
               images[0].git_sha, images[1].git_sha);
    }

    ASSERT_FATAL(platform_i2c_driver.init() == 0, "Failed to initialize I2C driver");
    uint8_t running;
    unsigned target = check_mcu(images, &running);
    uint16_t chunk = read_u16_le(FWU_FIFO_LENGTH_ADDR, "FIFO length");
    ASSERT_FATAL(chunk != 0, "FWU_FIFO_LENGTH reads 0");

    printf("     erasing slot %c (waits for the running slot to confirm itself)... ",
           'A' + target);
    fflush(stdout);
    send_fwu_cmd(FWU_CMD_REQUEST);
    wait_status(FWU_STATUS_BUSY, REQUEST_TIMEOUT_MS, "erase");
    printf("done\n     writing... ");
    ASSERT_FATAL(send_file(argv[1 + target], chunk) == 0, "Failed to send %s", argv[1 + target]);

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
