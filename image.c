#include "image.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* SRAM1+SRAM2+SRAM3 are 192KB contiguous from 0x20000000; the APP stack top
 * lies inside it. */
#define SRAM_BASE 0x20000000u
#define SRAM_END 0x20030000u

/* A vector table needs at least the initial SP and the reset vector. */
#define IMAGE_MIN_SIZE 512u

#define SLOT_END (IMAGE_SLOT_A_BASE + 448u * 1024u)

const char *image_board_name(uint32_t board_id)
{
    switch (board_id) {
    case IMAGE_BOARD_D1_EVB:
        return "D1_EVB";
    case IMAGE_BOARD_D1_C2:
        return "D1_C2";
    case IMAGE_BOARD_D2_EVB:
        return "D2_EVB";
    case IMAGE_BOARD_D2_OAM:
        return "D2_OAM";
    default:
        return NULL;
    }
}

static bool fail(char *err, size_t err_len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool fail(char *err, size_t err_len, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
    return false;
}

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Fixed-length ASCII field, NUL padded, copied as a NUL-terminated string. */
static void decode_ascii(const uint8_t *raw, size_t len, char *out, size_t out_len)
{
    size_t i = 0;

    while (i + 1 < out_len && i < len && raw[i] != '\0') {
        out[i] = (char)raw[i];
        i++;
    }
    out[i] = '\0';
}

/* Standard reflected CRC-32 (poly 0xEDB88320, init 0, final xor), the same value
 * zlib/binascii and the firmware's crc32.c produce. */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

/* Where the relocation table starts, or 0 when the image has none. */
static uint64_t reloc_table_start(const uint8_t *buf, uint64_t size, uint32_t *count,
                                  char *err, size_t err_len)
{
    uint32_t magic, n;
    uint64_t start;

    if (size < IMAGE_HEADER_SIZE + 8) {
        fail(err, err_len, "too small to hold a slot relocation table");
        return 0;
    }
    n = rd32(buf + size - 8);
    magic = rd32(buf + size - 4);
    if (magic != IMAGE_RELOC_MAGIC) {
        fail(err, err_len, "no slot relocation table (magic 0x%08x at the end, expected 0x%08x); "
             "rebuild the firmware with the current build.py", magic, IMAGE_RELOC_MAGIC);
        return 0;
    }
    start = size - 8 - (uint64_t)n * 4u;
    if (n == 0 || start < IMAGE_HEADER_SIZE || start > size - 8) {
        fail(err, err_len, "slot relocation table is corrupt (%u entries in a %" PRIu64
             "-byte file)", n, size);
        return 0;
    }
    *count = n;
    return start;
}

/* Header and vectors shared by every check; fills *tmp except the CRC check. */
static bool check_format(const uint8_t *buf, uint64_t size, image_info_t *tmp,
                         char *err, size_t err_len)
{
    const char *canonical;
    uint32_t sp, pc;

    tmp->magic = (uint16_t)rd16(buf + 0);
    tmp->version = (uint16_t)rd16(buf + 2);
    tmp->crc32 = rd32(buf + 4);
    tmp->image_size = rd32(buf + 8);
    tmp->vector_addr = rd32(buf + 12);
    tmp->board_id = rd32(buf + 24);
    decode_ascii(buf + 16, IMAGE_GIT_SHA_LEN, tmp->git_sha, sizeof tmp->git_sha);
    decode_ascii(buf + 28, IMAGE_BOARD_NAME_LEN, tmp->board_name, sizeof tmp->board_name);

    if (tmp->magic != IMAGE_MAGIC) {
        return fail(err, err_len, "not an A510 image: magic 0x%04x, expected 0x%04x",
                    (unsigned)tmp->magic, (unsigned)IMAGE_MAGIC);
    }
    if (tmp->version != IMAGE_VERSION_CURRENT) {
        return fail(err, err_len, "unsupported image header version %u, expected %u",
                    (unsigned)tmp->version, (unsigned)IMAGE_VERSION_CURRENT);
    }
    canonical = image_board_name(tmp->board_id);
    if (canonical == NULL) {
        return fail(err, err_len,
                    "board_id %" PRIu32 " is not a known board (1=D1_EVB, 2=D1_C2, 3=D2_EVB, "
                    "4=D2_OAM)", tmp->board_id);
    }
    if (strcmp(tmp->board_name, canonical) != 0) {
        return fail(err, err_len, "board_name '%s' does not describe board_id %" PRIu32 " (%s)",
                    tmp->board_name, tmp->board_id, canonical);
    }
    if (tmp->image_size != size - IMAGE_HEADER_SIZE) {
        return fail(err, err_len, "image_size %" PRIu32 " does not match the %" PRIu64
                    " payload bytes (truncated or padded image)", tmp->image_size,
                    size - IMAGE_HEADER_SIZE);
    }
    if (tmp->image_size < IMAGE_MIN_SIZE) {
        return fail(err, err_len, "image_size %" PRIu32 " is too small to hold a vector table",
                    tmp->image_size);
    }
    if (size > IMAGE_SLOT_SIZE) {
        return fail(err, err_len, "the image is %" PRIu64 " bytes, more than a slot holds (%u)",
                    size, (unsigned)IMAGE_SLOT_SIZE);
    }
    /* Every image is linked for slot A; the host relocates it for slot B. */
    if (tmp->vector_addr != IMAGE_SLOT_A_BASE + IMAGE_VECTOR_OFFSET) {
        return fail(err, err_len, "vector_addr 0x%08x, expected 0x%08x: images must be linked for "
                    "slot A (0x%08x)", (unsigned)tmp->vector_addr,
                    (unsigned)(IMAGE_SLOT_A_BASE + IMAGE_VECTOR_OFFSET),
                    (unsigned)IMAGE_SLOT_A_BASE);
    }
    if ((uint64_t)IMAGE_VECTOR_OFFSET + 8 > size) {
        return fail(err, err_len, "vector table at 0x%08x is outside the image",
                    (unsigned)tmp->vector_addr);
    }
    sp = rd32(buf + IMAGE_VECTOR_OFFSET);
    pc = rd32(buf + IMAGE_VECTOR_OFFSET + 4);
    if (sp < SRAM_BASE || sp > SRAM_END || (sp & 3u) != 0) {
        return fail(err, err_len, "initial stack pointer 0x%08x is outside SRAM [0x%08x,0x%08x] "
                    "or unaligned", sp, (unsigned)SRAM_BASE, (unsigned)SRAM_END);
    }
    if ((pc & 1u) == 0) {
        return fail(err, err_len, "reset vector 0x%08x has no Thumb bit set", pc);
    }
    if ((pc & ~1u) < IMAGE_SLOT_A_BASE || (pc & ~1u) >= IMAGE_SLOT_A_BASE + size) {
        return fail(err, err_len, "reset vector 0x%08x is outside the image", pc);
    }
    return true;
}

/* Table entries must be aligned words inside the payload, holding slot A
 * addresses. Checked on the slot A copy so relocation can rely on it. */
static bool check_table(const uint8_t *buf, uint64_t start, uint32_t count,
                        char *err, size_t err_len)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = rd32(buf + start + 4u * i);
        uint32_t value;

        /* 12 is the header's vector_addr; CRC and size (4..11) are never addresses. */
        if ((off & 3u) != 0 || off < 12 || off > start - 4) {
            return fail(err, err_len, "slot relocation entry %" PRIu32 " has a bad offset 0x%x",
                        i, off);
        }
        value = rd32(buf + off);
        if (value < IMAGE_SLOT_A_BASE || value >= SLOT_END) {
            return fail(err, err_len,
                        "slot relocation entry %" PRIu32 " (offset 0x%x) holds 0x%08x, not a "
                        "slot A address; the image was not built by the current build.py", i, off,
                        value);
        }
    }
    return true;
}

bool image_load(const char *path, uint8_t buf[IMAGE_SLOT_SIZE], image_info_t *info,
                char *err, size_t err_len)
{
    image_info_t tmp;
    struct stat st;
    uint64_t table_start;
    uint32_t crc;
    FILE *fp;
    bool ok;

    memset(&tmp, 0, sizeof tmp);
    if (stat(path, &st) != 0) {
        return fail(err, err_len, "cannot stat '%s': %s", path, strerror(errno));
    }
    if (!S_ISREG(st.st_mode)) {
        return fail(err, err_len, "'%s' is not a regular file", path);
    }
    tmp.file_size = (uint64_t)st.st_size;
    if (tmp.file_size > IMAGE_SLOT_SIZE) {
        return fail(err, err_len, "'%s' is %" PRIu64 " bytes, more than a slot holds (%u)",
                    path, tmp.file_size, (unsigned)IMAGE_SLOT_SIZE);
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return fail(err, err_len, "cannot open '%s': %s", path, strerror(errno));
    }
    ok = fread(buf, 1, (size_t)tmp.file_size, fp) == (size_t)tmp.file_size;
    fclose(fp);
    if (!ok) {
        return fail(err, err_len, "cannot read '%s': %s", path, strerror(errno));
    }

    ok = check_format(buf, tmp.file_size, &tmp, err, err_len);
    if (ok) {
        crc = crc32_update(0, buf + IMAGE_HEADER_SIZE, tmp.image_size);
        if (crc != tmp.crc32) {
            ok = fail(err, err_len, "CRC32 mismatch: header 0x%08x, computed 0x%08x", tmp.crc32,
                      crc);
        }
    }
    if (ok) {
        table_start = reloc_table_start(buf, tmp.file_size, &tmp.reloc_count, err, err_len);
        ok = table_start != 0 &&
             check_table(buf, table_start, tmp.reloc_count, err, err_len);
    }
    if (ok) {
        *info = tmp;
    }
    return ok;
}

void image_relocate(const image_info_t *info, uint8_t *buf)
{
    uint64_t start = info->file_size - 8 - (uint64_t)info->reloc_count * 4u;

    for (uint32_t i = 0; i < info->reloc_count; i++) {
        uint8_t *word = buf + rd32(buf + start + 4u * i);

        wr32(word, rd32(word) + (IMAGE_SLOT_B_BASE - IMAGE_SLOT_A_BASE));
    }
    wr32(buf + 4, crc32_update(0, buf + IMAGE_HEADER_SIZE, info->image_size));
}

void image_print_summary(const char *label, const char *path, const image_info_t *info)
{
    printf("  %-5s %s: OK (%" PRIu64 " bytes, image_size=%" PRIu32 ", crc32=0x%08x, "
           "board=%s/%" PRIu32 ", vectors=0x%08x, %" PRIu32 " slot relocations, git_sha=%s)\n",
           label, path, info->file_size, info->image_size, info->crc32, info->board_name,
           info->board_id, info->vector_addr, info->reloc_count, info->git_sha);
}
