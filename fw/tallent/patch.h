/* Local: what eq.h's users take from upstream's patch.h, without the stock
 * firmware's addresses. */
#ifndef PATCH_H
#define PATCH_H

#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef int16_t s16;
typedef uint64_t u64;
typedef int64_t s64;

#ifdef __arm__
#define MEMORY_BARRIER() __asm__ volatile("dmb" ::: "memory")
#else
#define MEMORY_BARRIER() __sync_synchronize()  /* host tools */
#endif

#endif
