#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool eden_servo_runtime_available(void);
const char *eden_servo_version(void);

#ifdef __cplusplus
}
#endif
