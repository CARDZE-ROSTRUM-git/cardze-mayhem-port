/*
 * Host replacement for the ChibiOS chprintf helper.
 *
 * The USB serial layer is replaced by a plain host stream (stdout / a future
 * virtual COM port). Only the formatting entry points used by the portable
 * code are provided.
 */
#ifndef __CHPRINTF_H__
#define __CHPRINTF_H__

#include "ch.h"

#include <cstddef>
#include <cstdint>

/* Forward declaration only: the USB serial driver header supplies the full
 * definition (usb_serial_device_to_host.h). */
struct BaseSequentialStream;

#ifdef __cplusplus
extern "C" {
#endif

void chprintf(BaseSequentialStream* chp, const char* fmt, ...);
void chSequentialStreamWrite(BaseSequentialStream* chp, const uint8_t* buf, size_t n);
uint8_t chSequentialStreamGet(BaseSequentialStream* chp);
int chSequentialStreamPut(BaseSequentialStream* chp, uint8_t byte);

#ifdef __cplusplus
}
#endif

#endif /* __CHPRINTF_H__ */
