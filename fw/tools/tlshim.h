/* What upstream ext/timelock.c's interrupt engine and ext/joins.c take from
 * ext.h, for tools/tltest.c (built by tools/tltest.sh): the stock engine's
 * words come from shim_word(), the symbols renamed up_*. */
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef int16_t s16;
typedef unsigned long long u64;
typedef signed long long s64;

#define timelock_tables up_timelock_tables
#define timelock_live up_timelock_live
#define timelock_locks up_timelock_locks
#define timelock_stats up_timelock_stats
#define timelock_clocks up_timelock_clocks
#define timelock_after up_timelock_after
#define timelock_frame up_timelock_frame
#define timelock_init up_timelock_init
#define joins_plan up_joins_plan
#define joins_planned up_joins_planned
#define joins_late up_joins_late

u32 *shim_word(u32 address);
#define REG32(address) (*shim_word((u32)(address)))
#define MEMORY_BARRIER() ((void)0)
#define CACHE_LINE_BYTES 64u
#define AUDIO_STEP_SHIFT 25u
#define ZONES 8u
#define PRESET_CHANNELS 8u
#define HIT_FLAG 0x80000000u
#define LEVEL_WORD_BASE 0x9ffffce0u
#define LEVEL_WORD_STRIDE 4u
#define LEVEL_SILENT 0x8000
extern s16 shim_cv[24];
#define EQ_CV_SAMPLES shim_cv
extern u32 patch_midi_clock_ticks;
extern s32 patch_grain_steps[8];
static inline u32 cpu_cycles(void) { return 0; }
