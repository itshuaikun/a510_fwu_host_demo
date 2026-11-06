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

/* A/B flash slots. Every image is linked for slot A; the FWU host relocates it
 * for slot B (image_relocate) just before sending it there. */
#define IMAGE_SLOT_A_BASE 0x08010000u
#define IMAGE_SLOT_B_BASE 0x08090000u
#define IMAGE_SLOT_SIZE (448u * 1024u - 16u)
#define IMAGE_VECTOR_OFFSET 0x200u

/* Relocation table appended to every released image by the firmware build
 * (a510_mcu/tools/slot_reloc.py): file offsets of the words that hold a slot A
 * address. Count and magic are the last 8 bytes. */
#define IMAGE_RELOC_MAGIC 0x4C455253u /* "SREL" */

/* The decoded header of a file that passed every check. */
typedef struct {
    uint16_t magic;
    uint16_t version;
    uint32_t crc32;
    uint32_t image_size;
    uint32_t vector_addr;
    uint32_t board_id;
    uint64_t file_size;
    uint32_t reloc_count; /* entries in the slot relocation table */
    char git_sha[IMAGE_GIT_SHA_LEN + 1];
    char board_name[IMAGE_BOARD_NAME_LEN + 1];
} image_info_t;

/* Canonical name of a board_id, or NULL when it is not a board identity. */
const char *image_board_name(uint32_t board_id);

/*
 * Check one released image file: header, CRC, board, slot A link address, the
 * slot relocation table and the vectors. This never touches the MCU: it only
 * reads `path`. Returns true when every check passed and filled `*info`; on
 * failure it writes a one-line reason (expected and actual values included) to
 * `err` and returns false.
 */
bool image_check_file(const char *path, image_info_t *info, char *err, size_t err_len);

/*
 * Copy `in` to `out` relocated for slot B: every table entry gets its stored
 * slot A address raised by one slot, then the header CRC is recomputed. `out`
 * must hold `info->file_size` bytes. Returns true, or false with a reason in
 * `err` when an entry does not hold the slot A address it must have.
 */
bool image_relocate(const image_info_t *info, const uint8_t *in, uint8_t *out,
                    char *err, size_t err_len);

/* One-line header summary, for the operator log. */
void image_print_summary(const char *label, const char *path, const image_info_t *info);
