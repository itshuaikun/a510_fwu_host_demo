/*
 * Read-only A510 MCU status reader for FWU bring-up / stress-test checks.
 *
 * Prints machine-readable key=value lines and exits 0 only when the MCU
 * answers with DEVICE_ID 0xA5 while running the APP. Never writes.
 */
#include <stdio.h>
#include "platform_i2c_driver.h"

#define MCU_ADDR 0x12
#define MAP_SIZE 0x54

static void kv_u8(const char *key, const unsigned char *m, int reg)
{
    printf("%s=0x%02X\n", key, m[reg]);
}

static void kv_u16(const char *key, const unsigned char *m, int reg)
{
    printf("%s=0x%04X\n", key, m[reg] | (m[reg + 1] << 8));
}

int main(void)
{
    unsigned char m[MAP_SIZE];
    const char *hash;

    if (platform_i2c_driver.init() != 0)
        return 1;
    if (platform_i2c_driver.read(MCU_ADDR, 0x00, m, sizeof m) != 0) {
        platform_i2c_driver.deinit();
        return 1;
    }
    platform_i2c_driver.deinit();

    kv_u8("device_id", m, 0x00);
    kv_u8("boot_state", m, 0x01);
    printf("app=%d\n", m[0x01] == 0x01);
    kv_u16("version", m, 0x02);
    /* 0x52 is the BOARD_ID register the FWU host compares its images against;
     * firmware older than memmap version 0x0201 has no descriptor there and
     * reads back 0x0000. */
    kv_u16("board_id", m, 0x52);
    kv_u16("total_power", m, 0x04);
    kv_u16("junction_temp", m, 0x06);
    kv_u8("alert_status", m, 0x2C);
    printf("project_version=%.16s\n", &m[0x32]);
    hash = (const char *)&m[0x42];
    printf("git_hash=%.16s\n", hash);
    printf("a55_reset_count=%u\n",
           m[0x2E] | (m[0x2F] << 8) | (m[0x30] << 16) | ((unsigned)m[0x31] << 24));

    return (m[0x00] == 0xA5 && m[0x01] == 0x01) ? 0 : 2;
}
