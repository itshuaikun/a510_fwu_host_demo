#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
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

/* Exit code contract: 2 means "the image was rejected and the MCU was never
 * touched", 1 means "the files were fine but the device or the invocation
 * failed". Scripts use the difference to tell a bad file from a bad fixture. */
#define EXIT_IMAGE_CHECK_FAILED 2

#define I2C_ADDR 0x12
#define DEVICE_ID_ADDR 0
#define BOOT_STATE_ADDR 1
#define MEM_DESC_VERSION_ADDR 2
#define BOARD_ID_ADDR 0x52
#define FWU_STATUS_ADDR 0xfb
#define FWU_FIFO_LENGTH_ADDR 0xfc
#define FWU_FIFO_ADDR 0xfe
#define FWU_CONTROL_ADDR 0xff

/* Memmap version that added the BOARD_ID register; header version 2 images can
 * only be programmed by firmware that reports at least this. */
#define MEM_DESC_VERSION_BOARD_ID 0x0201

#define FWU_CMD_REBOOT 0x00
#define FWU_CMD_REQUEST 0x01
#define FWU_CMD_RESEND 0x02
#define FWU_CMD_FILE_SEND_DONE 0x03

#include "image.h"
#include "platform_i2c_driver.h"

static inline bool is_fwu_ready(void) {
    uint8_t status = 0xFF;
    if (platform_i2c_driver.read(I2C_ADDR, FWU_STATUS_ADDR, &status, 1) != 0) {
        return false;
    }
    return status == 0x00;
}

#define FWU_WAIT_TIMEOUT_US (5 * 1000 * 1000)

/*
 * The app phase is the point of no return: the MCU is erasing and rewriting the
 * only application slot, and stopping there leaves a half-written image that
 * will not boot. Ctrl-C and a dropped terminal are therefore held off for
 * exactly that window, with a one-time notice so the operator learns the key was
 * seen and ignored instead of reaching for the power switch.
 *
 * SIGTERM and SIGKILL are deliberately left alone: a supervisor (and the
 * fault-injection suite, which SIGKILLs the tool on purpose) must still be able
 * to stop it, and a genuine mid-programming crash is something the MCU has to
 * survive anyway. Interrupting the earlier "BL => FWU_SRAM" phase also stays
 * allowed: that one only writes SRAM, leaving the installed app intact.
 */
static volatile sig_atomic_t interrupt_notice_shown = 0;
static struct sigaction saved_sigint, saved_sighup;

static void hold_interrupt(int signo) {
    (void)signo;
    if (interrupt_notice_shown) {
        return;
    }
    interrupt_notice_shown = 1;
    static const char notice[] =
        "\n     Ctrl-C ignored while the application image is being written.\n"
        "     Stopping here would leave the MCU with a half-written image; let it finish.\n";
    ssize_t written = write(STDERR_FILENO, notice, sizeof notice - 1);
    (void)written;
}

/* SA_RESTART keeps an interrupted I2C transfer going instead of failing it with
 * EINTR, so handling the signal cannot cause the very abort it prevents. */
static void hold_interrupts_start(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = hold_interrupt;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, &saved_sigint);
    sigaction(SIGHUP, &sa, &saved_sighup);
}

static void hold_interrupts_end(void) {
    sigaction(SIGINT, &saved_sigint, NULL);
    sigaction(SIGHUP, &saved_sighup, NULL);
}

static inline void wait_fwu_ready(const char *what) {
    int64_t waited_us = 0;
    while (!is_fwu_ready()) {
        usleep(1000);
        waited_us += 1000;
        ASSERT_FATAL(waited_us < FWU_WAIT_TIMEOUT_US,
                     "Timeout waiting for FWU FIFO to drain (%s)", what);
    }
}

// FWU_CONTROL REQUEST resets the MCU and the bootloader takes a while to
// bring its I2C slave up, so poll until FWU_FIFO_LENGTH reads back a usable
// value. The register only carries the ring capacity, and the APP state serves
// the same 0x03FF as BL/FWU_SRAM, so "non-zero" is the whole test; the poll is
// what matters, not the value. A fixed sleep here is not enough on real
// hardware.
static uint16_t fwu_fifo_length = 0;
static uint8_t* fwu_fifo_buf = NULL;
static inline void update_fwu_fifo_length(void) {
    union {
        uint8_t length8[2];
        uint16_t length16;
    } length;
    int64_t waited_us = 0;
    // The MCU is off the bus while it resets, so a NACK here is the expected
    // answer: the retry itself is the progress report, no need to log it.
    while (true) {
        if (platform_i2c_driver.read(I2C_ADDR, FWU_FIFO_LENGTH_ADDR, length.length8, 2) == 0 &&
            length.length16 != 0) {
            fwu_fifo_length = LE16_TO_HOST(length.length16);
            break;
        }
        usleep(5000);
        waited_us += 5000;
        ASSERT_FATAL(waited_us < FWU_WAIT_TIMEOUT_US,
                     "Timeout waiting for FWU bootloader FIFO");
    }
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
        if (fread(fwu_fifo_buf, 1, to_read, fp) != to_read) {
            fprintf(stderr, "Failed to read %zu bytes at offset %zu from %s\n",
                    to_read, offset, filename);
            fclose(fp);
            return -1;
        }
        wait_fwu_ready("before FIFO write");
        int rc;
        do {
            rc = platform_i2c_driver.write(I2C_ADDR, FWU_FIFO_ADDR, fwu_fifo_buf, to_read);
        } while (rc != 0 && errno == EINTR); // a held-off Ctrl-C must not abort the write
        if (rc != 0) {
            fprintf(stderr, "I2C write failed: reg=0x%02x offset=%zu len=%zu: %s\n",
                    FWU_FIFO_ADDR, offset, to_read, strerror(errno));
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
    wait_fwu_ready("final drain"); // make sure sent data is processed
    return 0;
}

static inline void send_fwu_cmd(uint8_t cmd) {
    ASSERT_FATAL(platform_i2c_driver.write(I2C_ADDR, FWU_CONTROL_ADDR, (uint8_t[]){cmd}, 1) == 0,
                 "Failed to send command 0x%02X: %s", cmd, strerror(errno));
}

static uint16_t read_u16_le(uint8_t reg, const char *what) {
    uint8_t bytes[2];
    ASSERT_FATAL(platform_i2c_driver.read(I2C_ADDR, reg, bytes, sizeof bytes) == 0,
                 "Failed to read %s (reg 0x%02x): %s", what, reg, strerror(errno));
    return LE16_TO_HOST((uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8)));
}

// A wrong-board image still passes magic/version/CRC, and the bootloader would
// program it without asking. Ask the MCU who it is before resetting it into the
// bootloader, and refuse when the answer disagrees with either image: the
// bootloader also jumps into the fwu_sram image without validating it, and that
// image carries its own board_id, so both files have to match.
static void check_mcu_board(const image_info_t *app, const image_info_t *sram) {
    uint16_t version = read_u16_le(MEM_DESC_VERSION_ADDR, "memmap version");

    if (version < MEM_DESC_VERSION_BOARD_ID) {
        fprintf(stderr,
                "MCU firmware memmap version is 0x%04x, but header version %u images need "
                ">= 0x%04x (the version that added BOARD_ID).\n"
                "That firmware cannot accept this image: flash the matching bootloader+app "
                "once over SWD (a510_mcu build/<board>/flash.bin), then use FWU again.\n",
                version, (unsigned)IMAGE_VERSION_CURRENT, MEM_DESC_VERSION_BOARD_ID);
        exit(EXIT_FAILURE);
    }

    uint16_t board = read_u16_le(BOARD_ID_ADDR, "board id");
    const char *mcu_name = image_board_name(board);

    if (mcu_name == NULL) {
        fprintf(stderr,
                "MCU firmware reports memmap version 0x%04x, so BOARD_ID (0x%02x) must exist, "
                "but it reads 0x%04x, which is not a board identity\n",
                version, BOARD_ID_ADDR, board);
        exit(EXIT_FAILURE);
    }

    struct {
        const char *label;
        const image_info_t *image;
    } roles[] = {
        { "app", app },
        { "fwu_sram", sram },
    };
    for (size_t i = 0; i < sizeof roles / sizeof roles[0]; i++) {
        if (roles[i].image->board_id != board) {
            fprintf(stderr,
                    "board mismatch: the MCU is a %s (BOARD_ID %u), but the %s image is for "
                    "%s (%u)\n",
                    mcu_name, board, roles[i].label, roles[i].image->board_name,
                    roles[i].image->board_id);
            exit(EXIT_IMAGE_CHECK_FAILED);
        }
    }

    printf("     board: %s (BOARD_ID=0x%04x, memmap version=0x%04x), both images match\n",
           mcu_name, board, version);
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <app.bin> <fwu_sram.bin>\n", prog);
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
    image_info_t app_info, sram_info;
    char err[512];

    if (!image_check_file(argv[1], IMAGE_ROLE_APP, &app_info, err, sizeof err)) {
        fprintf(stderr, "app image rejected: %s\n", err);
        return EXIT_IMAGE_CHECK_FAILED;
    }
    if (!image_check_file(argv[2], IMAGE_ROLE_FWU_SRAM, &sram_info, err, sizeof err)) {
        fprintf(stderr, "fwu_sram image rejected: %s\n", err);
        return EXIT_IMAGE_CHECK_FAILED;
    }

    /*
     * Both files go into the same board and each carries the board it was built
     * for, so a pair that names two different boards is wrong whatever the MCU
     * turns out to be. Checking it here refuses the pair before any I2C traffic
     * at all, which is what the MCU comparison below cannot do without a device.
     */
    if (app_info.board_id != sram_info.board_id) {
        fprintf(stderr,
                "image pair mismatch: the app image is for %s (%u), but the fwu_sram image is "
                "for %s (%u); both must be built for the same board\n",
                app_info.board_name, app_info.board_id, sram_info.board_name,
                sram_info.board_id);
        return EXIT_IMAGE_CHECK_FAILED;
    }

    printf("image check passed:\n");
    image_print_summary("app", argv[1], &app_info);
    image_print_summary("fwu_sram", argv[2], &sram_info);
    if (strcmp(app_info.git_sha, sram_info.git_sha) != 0) {
        printf("  note: app and fwu_sram come from different commits (%s vs %s)\n",
               app_info.git_sha, sram_info.git_sha);
    }

    ASSERT_FATAL(platform_i2c_driver.init() == 0, "Failed to initialize I2C driver");
    check_mcu_board(&app_info, &sram_info);

    send_fwu_cmd(FWU_CMD_REQUEST);
    update_fwu_fifo_length();

    printf("     BL => FWU_SRAM... ");
    ASSERT_FATAL(send_file(argv[2], true) == 0, "Failed to send fwu_sram");
    send_fwu_cmd(FWU_CMD_FILE_SEND_DONE);
    usleep(10000);

    printf("     programming... ");
    hold_interrupts_start();
    ASSERT_FATAL(send_file(argv[1], true) == 0, "Failed to send file: %s", argv[1]);

    send_fwu_cmd(FWU_CMD_REBOOT);
    hold_interrupts_end();

    ASSERT_FATAL(platform_i2c_driver.deinit() == 0, "Failed to deinitialize I2C driver");
    free(fwu_fifo_buf);
    return 0;
}
