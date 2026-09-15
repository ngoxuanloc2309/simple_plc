#ifndef SX_FLASH_H
#define SX_FLASH_H


#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*
 * Reads len bytes starting at addr directly from memory-mapped flash
 * (no unlock needed for reads).
 */
void sx_flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

/*
 * Programs len bytes starting at addr. Caller must sx_flash_unlock()
 * first and sx_flash_lock() after, and addr must point into a sector
 * that has already been erased (sx_flash_erase()) -- flash can only
 * clear bits (1 -> 0), never set them back, without an erase.
 *
 * STM32H5 only programs in fixed 16-byte (quad-word) units. If len is
 * not a multiple of 16, the final partial quad-word is padded with 0xFF
 * (the erased-flash value) -- safe because 0xFF bytes read back as
 * "still erased", so callers reading exactly len bytes back out never
 * see the padding.
 */
void sx_flash_write(uint32_t addr, const uint8_t *data, uint32_t len);

/*
 * Erases whole sectors covering [addr, addr+len). addr should be
 * sector-aligned; len is rounded up to a whole number of sectors.
 * This erases the ENTIRE covering sector(s), not just len bytes --
 * anything else sharing that sector is also wiped.
 */
void sx_flash_erase(uint32_t addr, uint32_t len);

void sx_flash_unlock(void);
void sx_flash_lock(void);

#ifdef __cplusplus
}
#endif

#endif // SX_FLASH_H