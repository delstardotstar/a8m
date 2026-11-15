/* What upstream ext/grains.c's engine (its lines up to render()) takes from
 * ext.h, for tools/graintest.c: the stock engine's words come from
 * shim_word(). */
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef int16_t s16;
typedef unsigned long long u64;
typedef signed long long s64;

#define grain_routes up_grain_routes
#define grain_ratio_q16 up_grain_ratio_q16
#define grain_ranges up_grain_ranges

u32 *shim_word(u32 address);
#define REG32(address) (*shim_word((u32)(address)))
#define MEMORY_BARRIER() ((void)0)
#define CACHE_LINE_BYTES 64u
#define AUDIO_STEP_SHIFT 25u
#define ZONES 8u
#define PRESET_CHANNELS 8u
#define PLAYHEAD_FRAME_BASE 0x9ffff9c4u
#define PLAYHEAD_FRAME_STRIDE 8u
#define LEVEL_WORD_BASE 0x9ffffce0u
#define LEVEL_WORD_STRIDE 4u
#define LEVEL_SILENT 0x8000
#define EQ_CV_COUNTS_FULL 32768
#define EQ_CV_AMOUNT_MAX 100
s32 eq_cv_counts(s32 source, u32 channel);
static inline u32 cpu_irq_save(void) { return 0; }
static inline void cpu_irq_restore(u32 s) { (void)s; }

enum { GRAIN_ON, GRAIN_SIZE, GRAIN_OVERLAP, GRAIN_SPRAY, GRAIN_DETUNE, GRAIN_OCTAVE, GRAIN_REVERSE,
       GRAIN_PAN, GRAIN_SHAPE, GRAIN_SCAN, GRAIN_SLIDE, GRAIN_SKEW, GRAIN_FIELDS };
enum { GRAIN_ROUTE_SIZE, GRAIN_ROUTE_OVERLAP, GRAIN_ROUTE_SPRAY, GRAIN_ROUTE_DETUNE, GRAIN_ROUTE_SLIDE,
       GRAIN_ROUTE_SCAN, GRAIN_ROUTE_SHAPE, GRAIN_ROUTE_PAN, GRAIN_ROUTES };
struct grain_settings {
    s16 value[GRAIN_FIELDS];
    signed char source[GRAIN_ROUTES], amount[GRAIN_ROUTES];
};
struct grain_range { s16 low, high, initial; };
extern const struct grain_range grain_ranges[GRAIN_FIELDS];
extern struct grain_settings patch_grains[8];
struct grain_route { u8 field; u16 span; };
extern const struct grain_route grain_routes[GRAIN_ROUTES];
