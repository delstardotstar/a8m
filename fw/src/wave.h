#ifndef WAVE_H_INCLUDED
#define WAVE_H_INCLUDED

#include <stdint.h>
#include "samples.h"

/* Waveform views of loaded samples for the UI. Main loop only. */

/* Min/max per column of plane `side` of s (READY) over frames [a, b),
 * w columns, as Q31 >> 16; frames outside the sample read 0. Long spans
 * come from a per-sample overview (4096 buckets) built on first use, a
 * bounded number of frames per call: returns 0 while it is being built. */
int wave_columns(const struct sample *s, int side, int64_t a, int64_t b, int w, int16_t *lo, int16_t *hi);

/* One frame, Q31 >> 16; 0 outside the sample. */
int wave_at(const struct sample *s, int side, int64_t f);

#endif
