#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#define USBD_MAX_NUM_INTERFACES 4U
#define USBD_MAX_NUM_CONFIGURATION 1U
#define USBD_MAX_STR_DESC_SIZ 512U
#define USBD_LPM_ENABLED 0U
#define USBD_SELF_POWERED 1U
#define USBD_DEBUG_LEVEL 0U
#define USBD_malloc malloc
#define USBD_free free
#define USBD_memset memset
#define USBD_memcpy memcpy

#define __IO volatile
#define __PACKED __attribute__((packed))

#define __STATIC_INLINE static inline
#define UNUSED(value) ((void)(value))
