#ifndef DRIVER_EXAMPLES_H_INCLUDED
#define DRIVER_EXAMPLES_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#include <hpl_i2c_m_sync.h>

void MPU5060_initialize(void);
void MPU5060_read(uint8_t *buf);

#ifdef __cplusplus
}
#endif
#endif // DRIVER_EXAMPLES_H_INCLUDED