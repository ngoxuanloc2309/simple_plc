#include "stm32h5_flash.h"

#if STM32H5_PLATFORM

#include <string.h>

/*
 * STM32H523CCU6 hard facts (per ST datasheet DS14540 / RM0481) --
 * DO NOT derive these from FLASH_SIZE/FLASH_BANK_SIZE in the CMSIS
 * header. Those macros read the flash-size data register at
 * FLASHSIZE_BASE (0x08FFF80C), which is confirmed by ST's own
 * community forum to cause a Hard Fault on STM32H5 parts -- and even
 * if it didn't fault, FLASH_SIZE_DEFAULT in this CMSIS header falls
 * back to 512 KB (0x80000), which is wrong for this 256 KB part
 * (STM32H523CC = category C = 256 KB, confirmed against ST's product
 * page and datasheet -- NOT the 512 KB variant the header's default
 * assumes).
 *
 * STM32H523CC is dual-bank even at 256 KB total (ST blog, April 2024):
 * 2 banks x 128 KB, 16 sectors/bank x 8 KB/sector = 32 sectors total.
 * This still matches FLASH_SECTOR_SIZE (0x2000 = 8 KB) from the CMSIS
 * header -- only the total/bank size macros are unsafe to use here.
 */
#define SX_FLASH_TOTAL_SIZE   0x40000U   /* 256 KB */
#define SX_FLASH_BANK_SIZE    0x20000U   /* 128 KB per bank */

#define SX_FLASH_QUADWORD_BYTES 16U

static void addr_to_bank_sector(uint32_t addr, uint32_t *bank, uint32_t *sector)
{
    uint32_t offset = addr - FLASH_BASE;

    if (offset >= SX_FLASH_BANK_SIZE) {
        *bank   = FLASH_BANK_2;
        *sector = (offset - SX_FLASH_BANK_SIZE) / FLASH_SECTOR_SIZE;
    } else {
        *bank   = FLASH_BANK_1;
        *sector = offset / FLASH_SECTOR_SIZE;
    }
}

void sx_flash_lock(void)
{
    HAL_FLASH_Lock();
}

void sx_flash_unlock(void)
{
    HAL_FLASH_Unlock();
}

void sx_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    /* Flash is memory-mapped -- reading is a plain load, no HAL call and
     * no unlock needed. */
    memcpy(buf, (const void *)addr, len);
}

void sx_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint8_t  quad[SX_FLASH_QUADWORD_BYTES];
    uint32_t written = 0;

    while (written < len) {
        uint32_t chunk = len - written;

        if (chunk >= SX_FLASH_QUADWORD_BYTES) {
            memcpy(quad, data + written, SX_FLASH_QUADWORD_BYTES);
        } else {
            /* Final partial quad-word: pad with 0xFF (erased-flash value)
             * so callers reading back exactly len bytes never see it. */
            memset(quad, 0xFF, SX_FLASH_QUADWORD_BYTES);
            memcpy(quad, data + written, chunk);
        }

        HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD,
                           addr + written,
                           (uint32_t)quad);

        written += SX_FLASH_QUADWORD_BYTES;
    }
}

void sx_flash_erase(uint32_t addr, uint32_t len)
{
    /* Reject anything outside the real 256 KB flash range up front --
     * silently proceeding would compute a bogus bank/sector and either
     * hand HAL_FLASHEx_Erase() garbage or erase memory this chip
     * doesn't have. */
    if (addr < FLASH_BASE || (addr - FLASH_BASE) >= SX_FLASH_TOTAL_SIZE) {
        return;
    }

    FLASH_EraseInitTypeDef erase = {0};
    uint32_t                sector_error = 0;
    uint32_t                bank, sector;

    addr_to_bank_sector(addr, &bank, &sector);

    uint32_t nb_sectors = (len + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks     = bank;
    erase.Sector    = sector;
    erase.NbSectors = nb_sectors;

    HAL_FLASHEx_Erase(&erase, &sector_error);
}

#endif // STM32H5_PLATFORM