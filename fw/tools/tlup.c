/* Appended to upstream's Time Lock engine (tools/tltest.sh): runs channel 1
 * (and 2 for a pair) as its audio hook and foreground would. */
static u32 words[3][8], playhead[16];
s16 shim_cv[24];
u32 patch_midi_clock_ticks;
s32 patch_grain_steps[8];
const u8 timelock_events_start[64], timelock_events_end[1];

u32 *shim_word(u32 a)
{
    if (a >= LEVEL_WORD_BASE && a < LEVEL_WORD_BASE + 32) return &words[0][(a - LEVEL_WORD_BASE) / 4];
    if (a >= PLAYHEAD_VECTOR && a < PLAYHEAD_VECTOR + 64) return &playhead[(a - PLAYHEAD_VECTOR) / 4];
    if (a >= ZONE_SELECTOR_BASE && a < ZONE_SELECTOR_BASE + 32) return &words[2][(a - ZONE_SELECTOR_BASE) / 4];
    static u32 junk;
    return &junk;
}

void up_start(const void *zone, size_t bytes, int pair, int clock, int input)
{
    memset(&timelock_tables, 0, sizeof(timelock_tables));
    memset(timelock_locks, 0, sizeof(timelock_locks));
    memset(timelock_clocks, 0, sizeof(timelock_clocks));
    struct lock_table *t = &timelock_tables[0];
    memcpy(&t->zone[0][0], zone, bytes);
    memset(t->clock_input, CLOCK_INPUT_NONE, sizeof(t->clock_input));
    t->on[0] = 1;
    t->pair[0] = (u8)pair;
    t->clock[0] = (u8)clock;
    if (clock > 1) t->clock_input[clock - 1] = (u8)input;
    timelock_live = 0;
    up_timelock_init();
}

void up_clock_setup(int n, u8 window, u8 settle, u32 inverse_from_period)
{
    timelock_clocks[n].window = window;
    timelock_clocks[n].settle = settle;
    (void)inverse_from_period;
    u32 period = timelock_clocks[n].period_q8;
    timelock_clocks[n].inverse = period ? (u32)((1ull << 40) / period) : 0;
}
u32 up_clock_period(int n) { return timelock_clocks[n].period_q8; }

void up_own(u32 own) { timelock_tables[0].own[0] = own; }
void up_tick(s64 tick, s32 base, u32 inverse)
{
    timelock_tables[0].zone[0][0].tick = tick;
    timelock_tables[0].zone[0][0].base = base;
    timelock_tables[0].zone[0][0].inverse = inverse;
}

/* One frame: the voice read at pos (playing) with step; its output
 * left/right mixed in place; returns the playhead after the hook. */
s64 up_frame(int playing, s64 pos, s32 step, s32 *left, s32 *right)
{
    s32 mix[8] = {0}, steps[8] = {0};
    words[0][0] = playing ? 0x7fffffffu : 0;
    playhead[0] = (u32)pos; playhead[1] = (u32)((u64)pos >> 32);
    playhead[2] = (u32)pos; playhead[3] = (u32)((u64)pos >> 32);
    steps[0] = step;
    mix[0] = *left; mix[1] = *right;
    timelock_frame(mix, steps);
    *left = mix[0]; *right = mix[1];
    return (s64)(((u64)playhead[1] << 32) | playhead[0]);
}

void up_plan(s32 step)
{
    patch_grain_steps[0] = step;
    joins_plan(0, &timelock_tables[0]);
}

size_t up_zone_bytes(void) { return sizeof(struct lock_zone); }
size_t up_lock_bytes(void) { return sizeof(struct lock); }
const void *up_lock(void) { return &timelock_locks[0]; }
