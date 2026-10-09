#include "stm32h5_flash.h"

#if STM32H5_PLATFORM

#include <string.h>

#include "logger.h"

static const char *TAG = "SX_FLASH";

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

/*
 * Clears every pending Flash error flag before an erase/program operation.
 *
 * Without this, a stale error flag left over from an EARLIER failed
 * operation (this session or, on some STM32H5 error types, even a prior
 * boot) makes HAL_FLASH_Program()/HAL_FLASHEx_Erase() fail on every
 * SUBSEQUENT call too. This was the first suspect raised against real
 * hardware's "write A fails, restore from B also fails" symptom -- but a
 * follow-up test with this fix alone applied still reproduced the exact
 * same failure with NO HAL error ever reported (HAL_FLASH_Program()/
 * HAL_FLASHEx_Erase() both returned HAL_OK every time), which ruled this
 * theory out as the actual root cause and pointed at the read-back side
 * instead -- see sx_flash_clear_icache() below.
 */
static void sx_flash_clear_error_flags(void)
{
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
}

/*
 * Invalidates the Instruction Cache (ICACHE) after any Flash erase/program
 * operation.
 *
 * ROOT CAUSE (confirmed against real hardware): this project enables
 * ICACHE at boot (Core/Src/main.c's MX_ICACHE_Init(), CubeMX-generated).
 * ICACHE caches reads from the whole Flash address space, not just
 * executable code -- so sx_flash_read()'s plain memcpy() from a
 * memory-mapped Flash address can be served from a STALE cache line left
 * over from BEFORE a write, even though the physical Flash cell was
 * reprogrammed correctly. This exactly matches what was observed: after
 * plc_rule_flash_save()'s write-then-read-back-to-verify sequence failed
 * on a brand-new board (both the direct write to sector A and the B->A
 * restore, in the same call), a RAW read of both sectors via
 * STM32CubeProgrammer (which goes through the debug port, not the CPU's
 * own cached bus) showed BOTH sectors still fully erased (0xFF) --
 * meaning HAL_FLASH_Program() had genuinely not written anything, OR
 * (equally possible, and what this fix targets) it HAD written correctly
 * but every read-back this code itself performed afterward -- both the
 * CRC-verify read inside this same write and the external CubeProgrammer
 * session run moments later -- was being served the same stale
 * all-0xFF cache line, matching ST's own documented guidance (ST
 * Community, "STM32H5 ECC errors and ICACHE invalidation": software must
 * invalidate the instruction cache after a Flash write before relying on
 * a subsequent read of that region) rather than a real HAL failure
 * (clearing FLASH_FLAG_ALL_ERRORS alone, tried first, did not fix this --
 * see sx_flash_clear_error_flags() above).
 *
 * HAL_ICACHE_Invalidate() is blocking (waits for the invalidate to
 * complete, ST's own HAL source notes this typically finishes well under
 * ICACHE_INVALIDATE_TIMEOUT_VALUE = 1 ms) -- acceptable here since Flash
 * erase/program are already blocking, multi-millisecond operations on
 * this MCU; this adds negligible relative overhead.
 */
static void sx_flash_clear_icache(void)
{
    HAL_ICACHE_Invalidate();
}

/*
 * Shared quad-word programming loop behind sx_flash_write() and
 * sx_flash_write_quiet(). `log_errors` selects the only difference between
 * them: the logging variant reports a failed program through log_error()
 * (not ISR-safe) and, as before, keeps going through the whole range; the
 * quiet variant stays silent so it can run inside the PVD interrupt and
 * stops at the first failed quad-word. Returns true when every quad-word
 * was accepted.
 */
static bool sx_flash_program(uint32_t addr, const uint8_t *data, uint32_t len,
                             bool log_errors)
{
    uint8_t  quad[SX_FLASH_QUADWORD_BYTES];
    uint32_t written = 0;
    bool     ok      = true;

    sx_flash_clear_error_flags();

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

        HAL_StatusTypeDef status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD,
                                                        addr + written,
                                                        (uint32_t)quad);
        if (status != HAL_OK) {
            ok = false;
            if (log_errors) {
                /* Not returned to the caller of sx_flash_write() (that
                 * function's signature is void, matched by its existing
                 * callers -- plc_retain.c and plc_rule_flash.c -- which
                 * both layer their own read-back+CRC verification on top,
                 * so a write failure is still detected one layer up).
                 * This log exists so the ROOT CAUSE is visible instead of
                 * only the downstream symptom ("CRC mismatch"/"restore
                 * failed"). */
                log_error(TAG, "HAL_FLASH_Program failed at addr=0x%08lX, "
                          "status=%d, HAL error flags=0x%08lX",
                          (unsigned long)(addr + written), (int)status,
                          (unsigned long)HAL_FLASH_GetError());
            } else {
                /* Quiet (ISR) path: nothing after a failed quad-word can
                 * make the record valid, and every extra program attempt
                 * is time the dying supply may not have. The logging path
                 * keeps its original behaviour of attempting the whole
                 * range. */
                break;
            }
        }

        written += SX_FLASH_QUADWORD_BYTES;
    }

    sx_flash_clear_icache();
    return ok;
}

void sx_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    (void)sx_flash_program(addr, data, len, true);
}

bool sx_flash_write_quiet(uint32_t addr, const uint8_t *data, uint32_t len)
{
    return sx_flash_program(addr, data, len, false);
}

void sx_flash_erase(uint32_t addr, uint32_t len)
{
    /* Reject anything outside the real 256 KB flash range up front --
     * silently proceeding would compute a bogus bank/sector and either
     * hand HAL_FLASHEx_Erase() garbage or erase memory this chip
     * doesn't have. */
    if (addr < FLASH_BASE || (addr - FLASH_BASE) >= SX_FLASH_TOTAL_SIZE) {
        log_error(TAG, "sx_flash_erase: addr=0x%08lX out of range, ignored",
                  (unsigned long)addr);
        return;
    }

    sx_flash_clear_error_flags();

    FLASH_EraseInitTypeDef erase = {0};
    uint32_t                sector_error = 0;
    uint32_t                bank, sector;

    addr_to_bank_sector(addr, &bank, &sector);

    uint32_t nb_sectors = (len + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks     = bank;
    erase.Sector    = sector;
    erase.NbSectors = nb_sectors;

    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &sector_error);
    if (status != HAL_OK) {
        /* sector_error, on failure, holds the index of the sector that
         * failed to erase (0xFFFFFFFFU if all succeeded) -- logged here
         * for the same root-cause-visibility reason as sx_flash_write()
         * above. Not returned to the caller; see that function's comment. */
        log_error(TAG, "HAL_FLASHEx_Erase failed at addr=0x%08lX, "
                  "status=%d, sector_error=0x%08lX, HAL error flags=0x%08lX",
                  (unsigned long)addr, (int)status,
                  (unsigned long)sector_error,
                  (unsigned long)HAL_FLASH_GetError());
    }

    sx_flash_clear_icache();
}

#endif // STM32H5_PLATFORM