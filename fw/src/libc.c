/* The few libc routines the compiler may call for struct copies and
 * initializers (we link without a C library). */
#include <stddef.h>
#include <stdint.h>

void *memset(void *d, int c, size_t n)
{
    uint8_t *p = d;
    uint32_t w = (uint8_t)c * 0x01010101u;

    while (n && ((uintptr_t)p & 3)) {
        *p++ = (uint8_t)c;
        n--;
    }
    for (; n >= 16; n -= 16, p += 16) {
        ((uint32_t *)p)[0] = w;
        ((uint32_t *)p)[1] = w;
        ((uint32_t *)p)[2] = w;
        ((uint32_t *)p)[3] = w;
    }
    while (n--) {
        *p++ = (uint8_t)c;
    }
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *p = d;
    const uint8_t *q = s;

    while (n--) {
        *p++ = *q++;
    }
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *p = d;
    const uint8_t *q = s;

    if (p < q) {
        while (n--) {
            *p++ = *q++;
        }
    } else {
        while (n--) {
            p[n] = q[n];
        }
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a, *q = b;

    for (; n; n--, p++, q++) {
        if (*p != *q) {
            return *p - *q;
        }
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) {
            return (char *)s;
        }
        if (!*s) {
            return 0;
        }
    }
}

/* Double to 64-bit integer, truncating, as the ARM EABI helpers the
 * compiler calls for (int64_t)/(uint64_t) casts. libgcc's versions go
 * through soft-float steps; values in 32-bit range (nearly every call in
 * the voices) take one VFP conversion instead. Out-of-range values
 * saturate. The helpers use the base (soft-float) calling convention. */
#define AEABI __attribute__((pcs("aapcs")))
AEABI uint64_t __aeabi_d2ulz(double x);
AEABI int64_t __aeabi_d2lz(double x);
static uint64_t d2u64(double x)
{
    union { double d; uint64_t u; } v = { x };
    int e = (int)((v.u >> 52) & 0x7ff) - 1075;
    uint64_t m = (v.u & ((1ull << 52) - 1)) | (1ull << 52);

    if (e >= 12) {
        return ~0ull;
    }
    if (e >= 0) {
        return m << e;
    }
    return e <= -53 ? 0 : m >> -e;
}

AEABI uint64_t __aeabi_d2ulz(double x)
{
    if (x >= 0.0 && x < 4294967296.0) {
        return (uint32_t)x;
    }
    return x > 0.0 ? d2u64(x) : 0;
}

AEABI int64_t __aeabi_d2lz(double x)
{
    if (x > -2147483648.0 && x < 2147483648.0) {
        return (int32_t)x;
    }
    if (x != x) {
        return 0;
    }
    if (x < 0.0) {
        uint64_t u = d2u64(-x);
        return u >= (1ull << 63) ? INT64_MIN : -(int64_t)u;
    }
    uint64_t u = d2u64(x);
    return u >= (1ull << 63) ? INT64_MAX : (int64_t)u;
}
