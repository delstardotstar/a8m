#ifndef UI_H_INCLUDED
#define UI_H_INCLUDED

#include "panel.h"

/* A8-style modal UI. Page buttons open pages, the same button again goes
 * home; DATA1 moves between fields, click toggles fine edit (DATA1 steps
 * the value), runs an action, or opens a list field's popup; DATA2 sweeps
 * the focused value 0-100% once it is moved. Channel buttons select and
 * trigger (hold CHANNELS to only select). */
void ui_input(const struct panel *p);
void ui_draw(void);
/* Shows a short message over the page for 1.5 s. */
void ui_say(const char *msg);
void ui_say_for(const char *msg, uint32_t us);
/* Audio Health's LOOP MS: the longest gap between two redraws. */
uint32_t ui_loop_ms_max(void);
#ifdef COPY_TEST
void ui_copy_selftest(void);
#endif

#endif
