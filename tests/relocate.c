/*
 * Print the tool's slot B relocation of one released image to stdout, so
 * tests/image_precheck.py can compare it with a510_mcu/tools/slot_reloc.py and
 * with the slot B link the firmware build keeps for the same sources.
 */
#include <stdio.h>

#include "image.h"

int main(int argc, char **argv)
{
    static uint8_t buf[IMAGE_SLOT_SIZE];
    image_info_t info;
    char err[512];

    if (argc != 2) {
        fprintf(stderr, "usage: relocate <app.bin>\n");
        return 1;
    }
    if (!image_load(argv[1], buf, &info, err, sizeof err)) {
        fprintf(stderr, "image rejected: %s\n", err);
        return 2;
    }
    image_relocate(&info, buf);
    return fwrite(buf, 1, (size_t)info.file_size, stdout) == info.file_size ? 0 : 1;
}
