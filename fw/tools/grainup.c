/* Appended to upstream's grain engine (tools/graintest.sh): runs it as its
 * start() and audio hook would, on channel 1 (and 2 for a pair). */
const struct grain_range grain_ranges[GRAIN_FIELDS] = {
    {0, 1, 0}, {5, 20000, 600}, {25, 800, 400}, {0, 2000, 20}, {0, 1200, 0}, {0, 100, 0},
    {0, 100, 0}, {0, 100, 50}, {1, 100, 100}, {-400, 400, 100}, {-1000, 1000, 0}, {-100, 100, 0},
};
struct grain_settings patch_grains[8];
static u32 words[3][8];

u32 *shim_word(u32 a)
{
    if (a >= LEVEL_WORD_BASE && a < LEVEL_WORD_BASE + 32) return &words[0][(a - LEVEL_WORD_BASE) / 4];
    if (a >= PLAYHEAD_FRAME_BASE && a < PLAYHEAD_FRAME_BASE + 64) return &words[1][(a - PLAYHEAD_FRAME_BASE) / 8];
    if (a >= ZONE_SELECTOR_BASE && a < ZONE_SELECTOR_BASE + 32) return &words[2][(a - ZONE_SELECTOR_BASE) / 4];
    static u32 junk;
    return &junk;
}

void up_start(const struct grain_settings *g, const s32 *pcm, u32 first, u32 frames, u32 loop, u32 turn, s32 right,
              int pair)
{
    patch_grains[0] = *g;
    memset(&tables[0], 0, sizeof(tables[0]));
    tables[0].zone[0] = (struct range){pcm, first, frames, loop, turn, right};
    tables[0].pair = (u32)pair;
    live = 0;
    memset(grains, 0, sizeof(grains));
    countdown = 0; drift = 0; seed = RANDOM_SEED;
    heard = PLAYHEAD_PARKED;
    fields(0, values);
    derive();
    norm = (s32)tuning.norm;
    grain_channel = 0;
}

/* One frame: the voice at `at` in the zone (PLAYHEAD_PARKED: silent). */
void up_frame(u32 at, s32 step, s32 *left, s32 *right)
{
    s32 mix[8] = {0}, steps[8] = {0};
    words[0][0] = at == PLAYHEAD_PARKED ? 0 : 0x7fffffffu;
    words[1][0] = tables[0].zone[0].first + at;
    words[2][0] = 0;
    steps[0] = step;
    render(mix, steps);
    *left = mix[0];
    *right = mix[1];
}

/* The foreground's pass after a CV change. */
void up_modulate(void) { modulate(); }
