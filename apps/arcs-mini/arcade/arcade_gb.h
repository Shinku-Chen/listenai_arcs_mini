#ifndef __ARCADE_GB_H__
#define __ARCADE_GB_H__

#include <stdint.h>
#include <stdbool.h>

int arcade_gb_start(const char *path);
void arcade_gb_stop(void);
bool arcade_gb_running(void);
void arcade_gb_run_frame(void);
void arcade_gb_set_input(uint32_t mask);

#endif /* __ARCADE_GB_H__ */
