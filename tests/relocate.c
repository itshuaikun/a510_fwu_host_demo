/*
 * Print the tool's slot B relocation of one released image to stdout, so
 * tests/image_precheck.py can compare it with a510_mcu/tools/slot_reloc.py and
 * with the slot B link the firmware build keeps for the same sources.
 */
#include <stdio.h>
#include <stdlib.h>

#include "image.h"

int main(int argc, char **argv)
{
    static uint8_t in[IMAGE_SLOT_SIZE];
    uint8_t *out;
    image_info_t info;
    char err[512];
    FILE *fp;

    if (argc != 2) {
        fprintf(stderr, "usage: relocate <app.bin>\n");
        return 1;
    }
    if (!image_check_file(argv[1], &info, err, sizeof err)) {
        fprintf(stderr, "image rejected: %s\n", err);
        return 2;
    }
    fp = fopen(argv[1], "rb");
    if (fp == NULL || fread(in, 1, (size_t)info.file_size, fp) != info.file_size) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    fclose(fp);
    out = malloc((size_t)info.file_size);
    if (out == NULL || !image_relocate(&info, in, out, err, sizeof err)) {
        fprintf(stderr, "relocation failed: %s\n", err);
        return 1;
    }
    fwrite(out, 1, (size_t)info.file_size, stdout);
    return 0;
}
