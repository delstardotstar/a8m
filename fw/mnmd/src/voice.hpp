// An MNMD voice module (State, reset, trigger, render on 64-frame Q31 blocks)
// behind the mdkit_voice API: controls 0..127, float out at 44.1 kHz in any
// frame counts. Drum machines (Voice) or synth machines with a pitch input (Synth).
#pragma once
#include <opxy/audio.hpp>
#include "md_dsp.hpp"
#include "md_kit.hpp"
extern "C" {
#include "voices.h"
}

namespace mnmd {
using opxy::i32;
using opxy::u32;

// MNMD's output ceiling (half scale) as our full scale.
const float out_scale = 1.0f / 1073741824.0f;

extern const u32 middle_c[opxy::block_frames];
extern u32 owners;

template <class S>
struct Wrap {
    S voice;
    md::Common common;
    i32 controls[8];
    i32 block[opxy::block_frames];
    u32 pos, hits;
};

// Drum machines start each note from reset(), as MNMD's kits do; the owner
// (MNMD's seed source) counts hits, so noise differs hit to hit.
template <class S, void (*Reset)(S&, md::Common&, u32), void (*Trigger)(S&, md::Common&),
          void (*Render)(S&, md::Common&, opxy::Output, const i32*, opxy::Pitch)>
struct Voice {
    typedef Wrap<S> W;
    static void init(void* p) {
        W& w = *static_cast<W*>(p);
        w.voice = S();
        w.common = md::Common();
        for (unsigned c = 0; c < 8; ++c) w.controls[c] = 0;
        w.pos = opxy::block_frames;
        w.hits = mdkit_noise_salt;
        Reset(w.voice, w.common, w.hits);
    }
    static void set_control(void* p, unsigned index, unsigned value) {
        if (index < 8)
            static_cast<W*>(p)->controls[index] = value >= 127 ? 0x7fffffff : static_cast<i32>(value * 16909320u);
    }
    static void trigger(void* p) {
        W& w = *static_cast<W*>(p);
        Reset(w.voice, w.common, ++w.hits);
        Trigger(w.voice, w.common);
        w.pos = opxy::block_frames;
    }
    static void render(void* p, float* out, unsigned frames) {
        W& w = *static_cast<W*>(p);
        while (frames) {
            if (w.pos == opxy::block_frames) {
                Render(w.voice, w.common, w.block, w.controls, middle_c);
                w.pos = 0;
            }
            unsigned n = opxy::block_frames - w.pos;
            n = n < frames ? n : frames;
            const i32* b = w.block + w.pos;
            for (unsigned i = 0; i < n; ++i) out[i] = static_cast<float>(b[i]) * out_scale;
            out += n;
            frames -= n;
            w.pos += n;
        }
    }
};

template <class S>
struct SynthWrap {
    S voice;
    md::Common common;
    i32 slots[8];
    i32 block[opxy::block_frames];
    u32 pitch[opxy::block_frames];
    u32 pos;
};

// Synth machines play notes as MNMD's engines do on one OP-XY voice: reset
// once when the voice gets an owner (each init a new one), then trigger
// (start) per note. Control i drives slot Slots[i]; the pitch is the
// channel's, Q16 octaves from middle C, used from the next 64-frame block.
template <class S, void (*Reset)(S&, md::Common&, u32), void (*Trigger)(S&, md::Common&),
          void (*Render)(S&, md::Common&, opxy::Output, const i32*, opxy::Pitch), const unsigned char* Slots,
          unsigned N>
struct Synth {
    typedef SynthWrap<S> W;
    static void init(void* p) {
        W& w = *static_cast<W*>(p);
        w.voice = S();
        w.common = md::Common();
        for (unsigned c = 0; c < 8; ++c) w.slots[c] = 0;
        for (unsigned i = 0; i < opxy::block_frames; ++i) w.pitch[i] = md::kit::middle_c;
        w.pos = opxy::block_frames;
        Reset(w.voice, w.common, ++owners);
    }
    static void set_control(void* p, unsigned index, unsigned value) {
        if (index < N)
            static_cast<W*>(p)->slots[Slots[index]] = value >= 127 ? 0x7fffffff : static_cast<i32>(value * 16909320u);
    }
    static void set_pitch(void* p, i32 octaves) {
        W& w = *static_cast<W*>(p);
        const u32 inc = md::transpose(md::kit::middle_c, octaves);
        if (inc != w.pitch[0])
            for (unsigned i = 0; i < opxy::block_frames; ++i) w.pitch[i] = inc;
    }
    static void trigger(void* p) {
        W& w = *static_cast<W*>(p);
        Trigger(w.voice, w.common);
        w.pos = opxy::block_frames;
    }
    static void render(void* p, float* out, unsigned frames) {
        W& w = *static_cast<W*>(p);
        while (frames) {
            if (w.pos == opxy::block_frames) {
                Render(w.voice, w.common, w.block, w.slots, w.pitch);
                w.pos = 0;
            }
            unsigned n = opxy::block_frames - w.pos;
            n = n < frames ? n : frames;
            const i32* b = w.block + w.pos;
            for (unsigned i = 0; i < n; ++i) out[i] = static_cast<float>(b[i]) * out_scale;
            out += n;
            frames -= n;
            w.pos += n;
        }
    }
};
}

#define MNMD_LIST(...) __VA_ARGS__
// names and defaults (0..127) as parenthesised lists, in the module's slot order.
#define MNMD_VOICE(sym, ns, label, names, defaults)                                                  \
    static const char* const sym##_names[] = {MNMD_LIST names};                                      \
    static const unsigned char sym##_defaults[] = {MNMD_LIST defaults};                              \
    static_assert(sizeof(sym##_names) / sizeof(sym##_names[0]) == sizeof(sym##_defaults), #sym);     \
    typedef mnmd::Voice<md::ns::State, md::ns::reset, md::ns::trigger, md::ns::render> sym##_ops;    \
    extern "C" const mdkit_voice sym = {label, 0, sizeof(sym##_defaults), sym##_names,                \
                                        sizeof(mnmd::Wrap<md::ns::State>), sym##_ops::init,          \
                                        sym##_ops::set_control, sym##_ops::trigger, sym##_ops::render, \
                                        sym##_defaults};

// A synth (struct pitched_voice): slots lists the module slot each control
// drives (MNMD's SYN page without its blanks).
#define MNMD_SYNTH(sym, ns, label, names, slots, defaults)                                           \
    static const char* const sym##_names[] = {MNMD_LIST names};                                      \
    static const unsigned char sym##_slots[] = {MNMD_LIST slots};                                    \
    static const unsigned char sym##_defaults[] = {MNMD_LIST defaults};                              \
    static_assert(sizeof(sym##_names) / sizeof(sym##_names[0]) == sizeof(sym##_defaults), #sym);     \
    static_assert(sizeof(sym##_slots) == sizeof(sym##_defaults), #sym);                              \
    typedef mnmd::Synth<md::ns::State, md::ns::reset, md::ns::trigger, md::ns::render, sym##_slots,  \
                        sizeof(sym##_slots)> sym##_ops;                                              \
    extern "C" const pitched_voice sym = {{label, 0, sizeof(sym##_defaults), sym##_names,               \
                                           sizeof(mnmd::SynthWrap<md::ns::State>), sym##_ops::init,  \
                                           sym##_ops::set_control, sym##_ops::trigger, sym##_ops::render, \
                                           sym##_defaults},                                          \
                                          sym##_ops::set_pitch};
