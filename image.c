#include "image.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Memory layout, from the firmware's common/memory.h. The MCU never
 * bound-checks the bytes it receives (fwu_fifo_process.c writes APP_BASE+count
 * for as long as the host keeps sending), so the host is the only place that can
 * catch an over-long image.
 */
#define APP_BASE 0x08008000u
#define APP_SIZE (992u * 1024u)
#define FWU_SRAM_BASE 0x20001800u
#define FWU_SRAM_SIZE (250u * 1024u)

/* The STM32F429 has 256KB of SRAM at 0x20000000; the FWU_SRAM image's stack top
 * is the very end of it. */
#define SRAM_BASE 0x20000000u
#define SRAM_END 0x20040000u

/*
 * The vector table sits IMAGE_VECTOR_OFFSET bytes into the image: the firmware's
 * linker script keeps .image_hdr first and puts .isr_vector at ALIGN(512), and
 * the 36-byte header is smaller than that alignment.
 */
#define IMAGE_VECTOR_OFFSET 0x200u

/* A vector table needs at least the initial SP and the reset vector. */
#define IMAGE_MIN_SIZE 512u

static const image_region_t regions[] = {
    [IMAGE_ROLE_APP] = { "app", APP_BASE, APP_SIZE },
    [IMAGE_ROLE_FWU_SRAM] = { "fwu_sram", FWU_SRAM_BASE, FWU_SRAM_SIZE },
};

const image_region_t *image_role_region(image_role_t role)
{
    return &regions[role];
}

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

/*
 * Standard reflected CRC-32 (poly 0xEDB88320, init 0, final xor), the same value
 * zlib/binascii and the firmware's crc32.c produce. The running value is passed
 * in and out so the payload can be streamed.
 */
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

static bool addr_in(const image_region_t *region, uint32_t addr)
{
    return addr >= region->base && addr < region->base + region->size;
}

bool image_check_file(const char *path, image_role_t role, image_info_t *info,
                      char *err, size_t err_len)
{
    const image_region_t *region = image_role_region(role);
    const image_region_t *other = image_role_region(
        role == IMAGE_ROLE_APP ? IMAGE_ROLE_FWU_SRAM : IMAGE_ROLE_APP);
    image_info_t tmp;
    struct stat st;
    uint8_t hdr[IMAGE_HEADER_SIZE];
    uint8_t vec[8];
    uint64_t payload;
    uint32_t vector_off, crc, sp, pc;
    const char *canonical;
    FILE *fp = NULL;
    bool ok = false;

    memset(&tmp, 0, sizeof tmp);

    if (stat(path, &st) != 0) {
        fail(err, err_len, "cannot stat '%s': %s", path, strerror(errno));
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        fail(err, err_len, "'%s' is not a regular file", path);
        return false;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        fail(err, err_len, "cannot open '%s': %s", path, strerror(errno));
        return false;
    }
    tmp.file_size = (uint64_t)st.st_size;

    if (fread(hdr, 1, sizeof hdr, fp) != sizeof hdr) {
        fail(err, err_len, "only %" PRIu64 " bytes: too small for the %u-byte image header",
             tmp.file_size, (unsigned)IMAGE_HEADER_SIZE);
        goto done;
    }

    tmp.magic = (uint16_t)rd16(hdr + 0);
    tmp.version = (uint16_t)rd16(hdr + 2);
    tmp.crc32 = rd32(hdr + 4);
    tmp.image_size = rd32(hdr + 8);
    tmp.vector_addr = rd32(hdr + 12);
    tmp.board_id = rd32(hdr + 24);
    decode_ascii(hdr + 16, IMAGE_GIT_SHA_LEN, tmp.git_sha, sizeof tmp.git_sha);
    decode_ascii(hdr + 28, IMAGE_BOARD_NAME_LEN, tmp.board_name, sizeof tmp.board_name);

    if (tmp.magic != IMAGE_MAGIC) {
        fail(err, err_len, "not an A510 image: magic 0x%04x, expected 0x%04x",
             (unsigned)tmp.magic, (unsigned)IMAGE_MAGIC);
        goto done;
    }
    if (tmp.version != IMAGE_VERSION_CURRENT) {
        fail(err, err_len,
             "unsupported image header version %u, expected %u (version 1 has no board "
             "fields and is rejected by this tool)", (unsigned)tmp.version,
             (unsigned)IMAGE_VERSION_CURRENT);
        goto done;
    }
    canonical = image_board_name(tmp.board_id);
    if (canonical == NULL) {
        fail(err, err_len,
             "board_id %" PRIu32 " is not a known board (1=D1_EVB, 2=D1_C2, 3=D2_EVB, 4=D2_OAM)",
             tmp.board_id);
        goto done;
    }
    if (strcmp(tmp.board_name, canonical) != 0) {
        fail(err, err_len, "board_name '%s' does not describe board_id %" PRIu32 " (%s)",
             tmp.board_name, tmp.board_id, canonical);
        goto done;
    }

    payload = tmp.file_size - IMAGE_HEADER_SIZE;
    if (tmp.image_size != payload) {
        fail(err, err_len,
             "image_size %" PRIu32 " does not match the %" PRIu64 " payload bytes "
             "(truncated or padded image)", tmp.image_size, payload);
        goto done;
    }
    if (tmp.image_size < IMAGE_MIN_SIZE) {
        fail(err, err_len,
             "image_size %" PRIu32 " is too small to hold a vector table (minimum %u bytes)",
             tmp.image_size, (unsigned)IMAGE_MIN_SIZE);
        goto done;
    }
    if (tmp.image_size > region->size) {
        fail(err, err_len,
             "the image is %" PRIu32 " bytes, more than the %s region holds (%u bytes from "
             "0x%08x); the MCU does not bound-check the stream", tmp.image_size, region->name,
             (unsigned)region->size, (unsigned)region->base);
        goto done;
    }

    crc = 0;
    {
        uint8_t buf[64 * 1024];
        size_t n;

        while ((n = fread(buf, 1, sizeof buf, fp)) > 0)
            crc = crc32_update(crc, buf, n);
        if (ferror(fp)) {
            fail(err, err_len, "cannot read '%s': %s", path, strerror(errno));
            goto done;
        }
    }
    if (crc != tmp.crc32) {
        fail(err, err_len, "CRC32 mismatch: header 0x%08x, computed 0x%08x", tmp.crc32, crc);
        goto done;
    }

    /*
     * Both images carry a vector table at their window base + 0x200, so the
     * address tells us which image this is. A swapped pair is the failure the
     * MCU cannot survive: it jumps to whatever it finds there.
     */
    if (!addr_in(region, tmp.vector_addr)) {
        if (addr_in(other, tmp.vector_addr)) {
            fail(err, err_len,
                 "this is a %s image (vector table at 0x%08x), not the %s image expected in "
                 "this position", other->name, (unsigned)tmp.vector_addr, region->name);
        } else {
            fail(err, err_len,
                 "vector_addr 0x%08x is in neither the app flash window [0x%08x,0x%08x) nor "
                 "the FWU SRAM window [0x%08x,0x%08x)", (unsigned)tmp.vector_addr,
                 (unsigned)APP_BASE, (unsigned)(APP_BASE + APP_SIZE), (unsigned)FWU_SRAM_BASE,
                 (unsigned)(FWU_SRAM_BASE + FWU_SRAM_SIZE));
        }
        goto done;
    }
    if (tmp.vector_addr != region->base + IMAGE_VECTOR_OFFSET) {
        fail(err, err_len,
             "vector_addr 0x%08x is not the expected 0x%08x for the %s image (the vector "
             "table is 512-byte aligned at the window base + 0x%x)",
             (unsigned)tmp.vector_addr, (unsigned)(region->base + IMAGE_VECTOR_OFFSET),
             region->name, (unsigned)IMAGE_VECTOR_OFFSET);
        goto done;
    }

    vector_off = tmp.vector_addr - region->base;
    if ((uint64_t)vector_off + sizeof vec > tmp.file_size) {
        fail(err, err_len, "vector table at 0x%08x is outside the image (%" PRIu64 " bytes)",
             (unsigned)tmp.vector_addr, tmp.file_size);
        goto done;
    }
    if (fseeko(fp, (off_t)vector_off, SEEK_SET) != 0 ||
        fread(vec, 1, sizeof vec, fp) != sizeof vec) {
        fail(err, err_len, "cannot read the vector table at offset 0x%x: %s", vector_off,
             strerror(errno));
        goto done;
    }
    sp = rd32(vec);
    pc = rd32(vec + 4);

    if (sp < SRAM_BASE || sp > SRAM_END || (sp & 3u) != 0) {
        fail(err, err_len,
             "initial stack pointer 0x%08x is outside SRAM [0x%08x,0x%08x] or unaligned",
             sp, (unsigned)SRAM_BASE, (unsigned)SRAM_END);
        goto done;
    }
    if ((pc & 1u) == 0) {
        fail(err, err_len, "reset vector 0x%08x has no Thumb bit set", pc);
        goto done;
    }
    if ((pc & ~1u) < region->base || (pc & ~1u) >= region->base + tmp.file_size) {
        fail(err, err_len, "reset vector 0x%08x is outside the image (0x%08x..0x%08x)", pc,
             (unsigned)region->base, (unsigned)(region->base + tmp.file_size));
        goto done;
    }

    *info = tmp;
    ok = true;

done:
    fclose(fp);
    return ok;
}

void image_print_summary(const char *label, const char *path, const image_info_t *info)
{
    printf("  %-9s %s: OK (%" PRIu64 " bytes, image_size=%" PRIu32 ", crc32=0x%08x, "
           "board=%s/%" PRIu32 ", vectors=0x%08x, git_sha=%s)\n",
           label, path, info->file_size, info->image_size, info->crc32, info->board_name,
           info->board_id, info->vector_addr, info->git_sha);
}
