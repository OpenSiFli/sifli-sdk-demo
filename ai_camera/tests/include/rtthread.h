#ifndef AI_CAMERA_TEST_RTTHREAD_H
#define AI_CAMERA_TEST_RTTHREAD_H

#include <string.h>

typedef int rt_err_t;
typedef void *rt_device_t;

#define RT_ERROR 1
#define RT_NULL ((void *)0)
#define rt_memcpy memcpy
#define rt_memset memset

int rt_kprintf(const char *format, ...);
rt_device_t rt_device_find(const char *name);

#endif
