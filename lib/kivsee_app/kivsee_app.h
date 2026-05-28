#pragma once

// Networked-animation app (kivsee env only). Driven by a FreeRTOS task that
// calls setup() once then loop() repeatedly.

#ifdef __cplusplus
extern "C" {
#endif

void kivsee_app_setup(void);
void kivsee_app_loop(void);

#ifdef __cplusplus
}
#endif
