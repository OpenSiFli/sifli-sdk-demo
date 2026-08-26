#ifndef AI_CAMERA_TEST_RTTHREAD_H
#define AI_CAMERA_TEST_RTTHREAD_H

#include <string.h>

typedef int rt_err_t;

#define RT_ERROR 1
#define rt_memcpy memcpy
#define rt_memset memset

int rt_kprintf(const char *format, ...);

#endif
