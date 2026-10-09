#ifndef SX_FLASH_H
#define SX_FLASH_H


#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
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
 * Same programming operation as sx_flash_write(), but safe to call from
 * interrupt context: it never logs (sx_flash_write() calls log_error() on
 * a failed program, which is not ISR-safe), never erases, and reports the
 * outcome through its return value instead. Returns true only if every
 * quad-word was accepted by the Flash controller; programming stops at the
 * first failure. A true return does NOT mean the data reads back
 * correctly -- the caller still verifies (CRC) as sx_flash_write()'s
 * callers do.
 *
 * Same preconditions as sx_flash_write(): the caller has unlocked the
 * Flash, the target range is already erased, and no other Flash operation
 * is in progress (the HAL serialises with a process lock and returns BUSY
 * instead of waiting, so an overlapping call fails rather than corrupts --
 * see services/plc_retain/plc_retain.c's flash-operation guard).
 */
bool sx_flash_write_quiet(uint32_t addr, const uint8_t *data, uint32_t len);

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