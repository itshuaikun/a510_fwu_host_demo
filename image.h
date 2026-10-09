#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The A510 FWU image header, mirrored from the firmware's common/image.h.
 * Three places describe this one wire format and must change together:
 *   a510_mcu/common/image.h, a510_mcu/tools/patch_image_header.py, this file.
 */
#define IMAGE_MAGIC 0x9ca3
#define IMAGE_VERSION_1 1
#define IMAGE_VERSION_2 2
#define IMAGE_VERSION_CURRENT IMAGE_VERSION_2
#define IMAGE_HEADER_SIZE 36
#define IMAGE_GIT_SHA_LEN 8
#define IMAGE_BOARD_NAME_LEN 8

/* board_id values (image_board_t in the firmware). 0 means "no identity". */
typedef enum {
    IMAGE_BOARD_NONE = 0,
    IMAGE_BOARD_D1_EVB = 1,
    IMAGE_BOARD_D1_C2 = 2,
    IMAGE_BOARD_D2_EVB = 3,
    IMAGE_BOARD_D2_OAM = 4,
} image_board_t;

/*
 * Which slot a file is being flashed into. The two images are not
 * interchangeable: the bootloader copies the fwu_sram image into SRAM and jumps
 * straight to the vector table it finds there without validating anything, so
 * handing it the app image (or the other way round) hangs the board instead of
 * failing safely.
 */
typedef enum {
    IMAGE_ROLE_APP = 0,  /* FWU_SRAM programs it at 0x08008000 in flash */
    IMAGE_ROLE_FWU_SRAM, /* the bootloader copies it to 0x20001800 in SRAM */
} image_role_t;

typedef struct {
    const char *name;
    uint32_t base; /* window base address */
    uint32_t size; /* window size in bytes */
} image_region_t;

/* The decoded header of a file that passed every check. */
typedef struct {
    uint16_t magic;
    uint16_t version;
    uint32_t crc32;
    uint32_t image_size;
    uint32_t vector_addr;
    uint32_t board_id;
    uint64_t file_size;
    char git_sha[IMAGE_GIT_SHA_LEN + 1];
    char board_name[IMAGE_BOARD_NAME_LEN + 1];
} image_info_t;

/* The flash/SRAM window an image of this role must live in. */
const image_region_t *image_role_region(image_role_t role);

/* Canonical name of a board_id, or NULL when it is not a board identity. */
const char *image_board_name(uint32_t board_id);

/*
 * Check one image file for the given role. This never touches the MCU: it only
 * reads `path`. Returns true when every check passed and filled `*info`; on
 * failure it writes a one-line reason (expected and actual values included) to
 * `err` and returns false.
 */
bool image_check_file(const char *path, image_role_t role, image_info_t *info,
                      char *err, size_t err_len);

/* One-line header summary, for the operator log. */
void image_print_summary(const char *label, const char *path, const image_info_t *info);
