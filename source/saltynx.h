#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void saltynx_start(void);
void saltynx_stop(void);
void saltynx_set_resolution(uint32_t width, uint32_t height);
void saltynx_frame(uint8_t api);

#ifdef __cplusplus
}
#endif
