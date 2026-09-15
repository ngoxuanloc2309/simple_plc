#ifndef SX_FLASH_H
#define SX_FLASH_H


#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void sx_flash_read(uint32_t addr, uint8_t *buf, uint32_t len);
void sx_flash_write(uint32_t addr, const uint8_t *data, uint32_t len);
void sx_flash_erase(uint32_t addr, uint32_t len);
void sx_flash_unlock(void);
void sx_flash_lock(void);

#ifdef __cplusplus
}
#endif

#endif // SX_FLASH_H