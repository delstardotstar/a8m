#ifndef STORAGE_H_INCLUDED
#define STORAGE_H_INCLUDED

#include "preset.h"

/*
 * The front card (FatFs drive 1): one folder in RAM, its presets
 * <folder>/prstNNN.yml (slots 1..PRESET_SLOTS), and 1:A8M-PREFS.YML.
 * The live preset is `preset` at slot preset_number; every other slot
 * keeps its RAM copy (edits survive switching) next to the version last
 * read from or written to the card. Without a card nothing is written.
 * Main loop only.
 */

enum { SLOT_EMPTY, SLOT_PENDING, SLOT_READY };
enum { WARN_ALWAYS, WARN_DIRTY, WARN_NEVER };

extern int pref_browse_preview;
extern int pref_auto_audition;
extern int pref_load_clean;      /* preset loads stop everything (else carry on, as stock) */
extern int pref_load_warn;
extern int pref_gate_delay;     /* 0.1 ms, 0..50 */
extern int pref_brightness;     /* 1..11 */
extern int pref_saver;          /* minutes 2..60, 0 = never */
extern uint32_t pref_hang_resets;   /* WDT1 resets counted (prefs HangResets) */
/* Time from the last Inc/Dec/Reset CV edge to its preset being live with
 * its machines prepared, us. */
extern uint32_t storage_cv_latency_us;
/* Why the last call failed, or 0. */
extern const char *storage_error;

/* Mounts the card, reads the prefs and loads the last folder and preset
 * into `preset` (else the first folder; without a card or folder, an INIT
 * preset at slot 1). The folder's other presets load in storage_poll. */
void storage_init(void);
/* Loads a few pending presets (time-bounded) and writes the prefs when due. */
void storage_poll(void);
/* Writes the prefs files that are due now (before a reset). */
void storage_flush(void);
int storage_card(void);
/* Card check (LOAD/SAVE): a pulled card is unmounted (RAM kept), an
 * inserted one mounted; the loaded folder stays. 1 when a card was mounted. */
int storage_refresh(void);
/* Schedules a prefs write (debounced). */
void storage_prefs_changed(void);
/* After a WDT1 reset: counts it in the prefs. */
void storage_count_hang(void);
/* The folder's Inc/Dec/Reset preset CV inputs (engine_preset_cv_in) are
 * read from <folder>/folderprefs.yml on load; this schedules a write. */
void storage_folder_cv_changed(void);
/* Applies Inc/Dec/Reset preset CV rises counted by the engine; 1 if the
 * live preset changed. Main loop. */
int storage_cv_poll(void);

/* Root folders of the card, sorted by name; storage_scan re-reads them. */
int storage_scan(void);
int storage_folders(void);
const char *storage_folder_name(int i);
/* The loaded folder, "" if none. */
const char *storage_folder(void);
/* Appends n bytes to a front-card file (created if missing); 1 done,
 * -1 a storage job or install is running, 0 failed (storage_error). */
int storage_append(const char *path, const char *text, unsigned n);
/* Folder utilities on the card root; each returns 1, or 0 with
 * storage_error set, and refreshes the folder list. The loaded folder can't
 * be renamed or deleted ("FOLDER IN USE"). */
int storage_folder_in_use(const char *name);
/* Whether a new folder could be created under this name. */
int storage_name_ok(const char *name);
int storage_new_folder(const char *name);
int storage_rename_folder(const char *from, const char *to);
/* Duplicate and delete run as a job stepped by storage_poll (a few ms, one
 * 32 KB chunk at a time). A failed or cancelled duplicate removes its
 * partial copy (JOB_CLEANING) before ending. Delete can't be cancelled.
 * The result stays until storage_job_clear. */
enum { JOB_NONE, JOB_COPYING, JOB_DELETING, JOB_CLEANING, JOB_DONE, JOB_FAILED, JOB_CANCELLED, JOB_IMPORTING, JOB_TRUNCATING, JOB_RECORDING };
int storage_dup_start(const char *src, const char *dst);
int storage_delete_folder(const char *name);
int storage_job_active(void);
int storage_job_state(void);
int storage_job_percent(void);
/* The folder being written or removed. */
const char *storage_job_name(void);
void storage_job_cancel(void);
void storage_job_clear(void);
/* Loads a folder (edits in RAM are dropped), starting with slot `want`
 * (or its first preset). */
int storage_load_folder(const char *name, int want);
/* A folder load is in progress (presets or the samples they need) and can
 * be cancelled: storage_cancel_load reloads the folder loaded before it
 * and its live preset, as stock does (the cancelled folder's edits are
 * gone; a boot load goes to the first other folder). A same-folder reload
 * and the reload a cancel starts can't be cancelled. */
int storage_loading(void);
int storage_cancel_load(void);
/* Presets found in the folder and how many are still loading. */
int storage_total(void);
int storage_pending(void);
/* Time the last folder took to load completely, us (0 while loading). */
uint32_t storage_load_us(void);
/* Samples (samples.h): the samples[] index a zone of a slot's preset
 * plays (zone_cfg.smp, edits included), -1 for none. The folder's WAV headers are read
 * once its presets are; then the samples of the live preset load, then
 * those of the presets after it and before it (stock's order; a sample
 * in progress finishes first). Machine channels and unused WAVs load nothing. */
int storage_sample(int slot, int ch, int zone);
/* Loads sample i (an unused WAV or a preview) next, as soon as the current
 * chunk is done. */
void storage_sample_demand(int i);
/* Whether any preset of the folder uses sample i. */
int storage_sample_used(int i);
/* Frees a sample's memory again if it was the last one loaded. */
void storage_sample_unload(int i);
void storage_preview_release(int i);
void storage_preview_release_all(void);
/* Renames the WAV on the card and in the table; presets using it become
 * changed (their files name the old one). */
int storage_sample_rename(int i, const char *to);
/* Deletes the WAV and removes it from every preset of the folder. */
int storage_sample_erase(int i);

/* The card browser: dir is relative to the card root ("" = root). Entries:
 * subfolders, then .wav files, each sorted. */
int storage_bopen(const char *dir);
int storage_bcount(void);
const char *storage_bname(int i);
int storage_bisdir(int i);
uint32_t storage_bsize(int i);
/* Entry i as a preview (samples_preview_at), -1 if it can't be used. */
int storage_bpreview(int i);

/* Import (copy into the loaded folder, a job stepped by storage_poll):
 * queue files, start. A file already there with the same size and bytes is
 * used as it is; a different one under the same name gets "~1", "~2"...
 * before the extension. Files from the folder itself are not copied.
 * After JOB_DONE, import_result gives each queue entry's sample index. */
void storage_import_clear(void);
int storage_import_add(const char *dir, const char *name);
int storage_import_start(void);
int storage_import_result(int idx);
/* Truncation (a job like import; the result stays until storage_job_clear):
 * frames [from, to) of sample i, in its format, written as a new file named
 * <name>T00.wav (T01...) that is added to the table, or over the sample's
 * own file (in_place; the sample is then loaded again). storage_trunc_result
 * is the sample index of the new file (or i) once JOB_DONE, -1 otherwise. */
int storage_trunc_start(int i, uint32_t from, uint32_t to, int in_place);
int storage_trunc_result(void);
/* Recording: writes sample i (a take in memory, samples_add_ram) to the
 * folder as a job; whether a file of that name is in the folder; the
 * sampling setup file is written a little after a change. */
int storage_rec_write(int i);
int storage_wav_exists(const char *name);
void storage_ssetup_changed(void);
/* Zones of the folder's presets playing sample i. */
int storage_sample_uses(int i);
/* Sample i was cut in place to [from, to) of old_frames: every zone using
 * it in every preset has its points moved to match (the presets become
 * changed). */
void storage_sample_trimmed(int i, uint32_t from, uint32_t to, uint32_t old_frames);
#define PI_HEAVY_PROFILE 94    /* PROFILE_VOICE: PI HEAVY, all 8 hit together */
#define PI_SCENE_PROFILE 93    /* ... plus CV routes, Bits, machine envelope */
#define SAMPLER_PROFILE 99      /* PROFILE_VOICE: 8 sample voices */
#define SAMPLER_MOD_PROFILE 98  /* the same with audio-rate pitch/level CV routes */
#define SAMPLER_PM_PROFILE 97   /* the same with phase mod, aliasing, smoothing, crossfades */
#define CARD_PROFILE 92         /* the front card's last folder and preset (emu/worst.py CARD=img) */
/* Folder sample loading done, %, or -1 when nothing is loading. */
int storage_samples_percent(void);
/* The sample loading now, or -1. */
int storage_sample_loading(void);
/* Folder load to all needed samples in RAM, us (0 until then). */
uint32_t storage_sample_load_us(void);
/* Front card size and free space, KB; 0 without a card. */
int storage_usage(uint32_t *total_kb, uint32_t *free_kb);

int storage_state(int slot);
/* Name of a loaded slot (the live one: preset.name), else 0. */
const char *storage_name(int slot);
int storage_changed(int slot);
/* The live preset is a fresh one, not on the card and not edited. */
int storage_blank(void);
/* Changed presets in the folder, the live one included. */
int storage_changed_count(void);
/* Next slot after `from` in direction dir holding a preset (any slot when
 * all); `from` if there is none. */
int storage_next(int from, int dir, int all);
/* The preset in a slot as the folder holds it now (edits included), loading
 * it first if still pending; 0 for an empty slot. */
const struct preset *storage_peek(int slot);
/* A free slot for a new preset (after the last one if possible), 0 if full. */
int storage_new_slot(void);
/* Makes slot the live preset, keeping the current one's edits in RAM; an
 * empty slot starts as INIT. 0 if it can't be read. */
int storage_select(int slot);

/* Another folder's presets, for importing: storage_xopen lists the
 * folder's presets (by slot, ascending); names and contents are read on
 * demand. The pointer stays valid until the next storage_xpreset. Samples
 * are not imported yet (there are none). */
int storage_xopen(const char *name);
int storage_xcount(void);
int storage_xslot(int i);
const char *storage_xname(int i);
const struct preset *storage_xpreset(int i);
/* Puts a copy of p in slot of the folder, as an unsaved change (an empty
 * slot becomes a new preset). 0 if the slot can't be read. */
int storage_put(int slot, const struct preset *p);

/* Name of zone z of channel ch in the preset last returned by
 * storage_xpreset ("" if none); the WAV is in the folder storage_xopen named. */
const char *storage_xsample(int ch, int z);

/* Writes the live preset to slot, which becomes the live slot (the old
 * one goes back to its saved version). */
int storage_save(int slot);
/* Writes every changed preset; the number written, -1 on a failure. */
int storage_save_all(void);
void storage_revert(void);
void storage_revert_channel(int ch);
/* Deletes the live slot's file; the slot becomes a fresh INIT preset. */
int storage_erase(void);

#endif
