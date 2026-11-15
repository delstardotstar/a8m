// MNMD voices from the untouched upstream headers (ref/OP-XY-MNMD, ref/open-xy),
// built natively, for emu/voicecost.py: what fw/mnmd's wrappers should output.
// usage: mnmdref SLUG [controls...]  (default controls, one trigger, 22050 frames;
// synths at middle C, then from frame MOVE up OCTAVES (Q16), as fw/src/main.c checks)
#include <opxy/audio.hpp>
#include "md_dsp.hpp"
#include "md_kit.hpp"
#include "gnd_sn.hpp"
#include "gnd_ns.hpp"
#include "gnd_im.hpp"
#include "swave.hpp"
#include "sid_6581.hpp"
#include "fmp.hpp"
#include "vo_6.hpp"
#include "gnd_sin.hpp"
#include "gnd_nois.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" {
#include "mdkit.h"
}
using opxy::i32;
using opxy::u32;

static u32 tuned[opxy::block_frames];

template <class S, void (*Reset)(S&, md::Common&, u32), void (*Trigger)(S&, md::Common&),
          void (*Render)(S&, md::Common&, opxy::Output, const i32*, opxy::Pitch)>
struct Ref {
    struct St { S s; md::Common c; i32 ctl[8]; i32 blk[64]; unsigned pos, hits; };
    static void init(void* p) {
        St* r = static_cast<St*>(p);
        std::memset(r, 0, sizeof *r);
        r->pos = 64;
        Reset(r->s, r->c, 0);
    }
    static void set(void* p, unsigned i, unsigned v) {
        static_cast<St*>(p)->ctl[i] = v >= 127 ? 0x7fffffff : static_cast<i32>(v * 16909320u);
    }
    static void trig(void* p) {
        St* r = static_cast<St*>(p);
        Reset(r->s, r->c, ++r->hits);
        Trigger(r->s, r->c);
        r->pos = 64;
    }
    static void render(void* p, float* out, unsigned n) {
        St* r = static_cast<St*>(p);
        for (unsigned f = 0; f < n; ++f) {
            if (r->pos == 64) { Render(r->s, r->c, r->blk, r->ctl, tuned); r->pos = 0; }
            out[f] = static_cast<float>(r->blk[r->pos++] / 1073741824.0);
        }
    }
};

// A synth as one OP-XY voice: initialize (owner 1, the firmware's first), then
// start per note; controls are slots 0.. in order unless slot-mapped.
const unsigned MOVE = 11008;
const i32 OCTAVES = 38229;
static unsigned frame;

template <class S, void (*Reset)(S&, md::Common&, u32), void (*Trigger)(S&, md::Common&),
          void (*Render)(S&, md::Common&, opxy::Output, const i32*, opxy::Pitch), const unsigned char* Slots>
struct SynthRef {
    struct St { S s; md::Common c; i32 slot[8]; i32 blk[64]; u32 pitch[64]; unsigned pos; };
    static void init(void* p) {
        St* r = static_cast<St*>(p);
        std::memset(r, 0, sizeof *r);
        r->pos = 64;
        for (unsigned i = 0; i < 64; ++i) r->pitch[i] = md::kit::middle_c;
        Reset(r->s, r->c, 1);
    }
    static void set(void* p, unsigned i, unsigned v) {
        static_cast<St*>(p)->slot[Slots[i]] = v >= 127 ? 0x7fffffff : static_cast<i32>(v * 16909320u);
    }
    static void trig(void* p) {
        St* r = static_cast<St*>(p);
        Trigger(r->s, r->c);
        r->pos = 64;
        frame = 0;
    }
    static void render(void* p, float* out, unsigned n) {
        St* r = static_cast<St*>(p);
        if (frame == MOVE)
            for (unsigned i = 0; i < 64; ++i) r->pitch[i] = md::transpose(md::kit::middle_c, OCTAVES);
        frame += n;
        for (unsigned f = 0; f < n; ++f) {
            if (r->pos == 64) { Render(r->s, r->c, r->blk, r->slot, r->pitch); r->pos = 0; }
            out[f] = static_cast<float>(r->blk[r->pos++] / 1073741824.0);
        }
    }
};
static const unsigned char saw_slots[] = {0, 1, 2, 4, 5, 6, 7}, sid_slots[] = {0, 1, 2, 3, 4, 5, 6, 7},
    seq8_slots[] = {0, 1, 2, 3, 4, 5, 6, 7}, sin_slots[] = {7}, nois_slots[] = {0, 1, 2, 7, 5};

#define SYNTH(sym, ns, label, slots)                                                                  \
    typedef SynthRef<md::ns::State, md::ns::reset, md::ns::trigger, md::ns::render, slots> sym##_r;    \
    extern "C" const mdkit_voice sym = {label, 0, sizeof(slots), 0, sizeof(sym##_r::St), sym##_r::init, \
                                        sym##_r::set, sym##_r::trig, sym##_r::render, 0};
SYNTH(mnmdref_swave_saw, swave_saw, "SWAVE-SAW", saw_slots)
SYNTH(mnmdref_sid_6581, sid_6581, "SID-6581", sid_slots)
SYNTH(mnmdref_swave_pulse, swave_pulse, "SWAVE-PULSE", seq8_slots)
SYNTH(mnmdref_swave_ens, swave_ens, "SWAVE-ENS", seq8_slots)
SYNTH(mnmdref_fmpstat, fmpstat, "FMPSTAT", seq8_slots)
SYNTH(mnmdref_fmppar, fmppar, "FMPPAR", seq8_slots)
SYNTH(mnmdref_fmpdyn, fmpdyn, "FMPDYN", seq8_slots)
SYNTH(mnmdref_vo_6, vo_6, "VO-6", seq8_slots)
SYNTH(mnmdref_gnd_sin, gnd_sin, "GND-SIN", sin_slots)
SYNTH(mnmdref_gnd_nois, gnd_nois, "GND-NOIS", nois_slots)

#define REF(sym, ns, label, n)                                                                       \
    typedef Ref<md::ns::State, md::ns::reset, md::ns::trigger, md::ns::render> sym##_r;                \
    extern "C" const mdkit_voice sym = {label, 0, n, 0, sizeof(sym##_r::St), sym##_r::init, sym##_r::set, \
                                        sym##_r::trig, sym##_r::render, 0};
REF(mnmdref_gnd_sn, gnd_sn, "GND-SN", 4)
REF(mnmdref_gnd_ns, gnd_ns, "GND-NS", 1)
REF(mnmdref_gnd_im, gnd_im, "GND-IM", 4)

extern "C" const mdkit_voice* mnmdref_find(const char* slug) {
    for (unsigned i = 0; i < opxy::block_frames; ++i) tuned[i] = md::kit::middle_c;
    static const mdkit_voice* const all[] = {&mnmdref_gnd_sn, &mnmdref_gnd_ns, &mnmdref_gnd_im, &mnmdref_swave_saw,
                                             &mnmdref_sid_6581, &mnmdref_swave_pulse, &mnmdref_swave_ens, &mnmdref_fmpstat,
                                             &mnmdref_fmppar, &mnmdref_fmpdyn, &mnmdref_vo_6, &mnmdref_gnd_sin,
                                             &mnmdref_gnd_nois};
    for (const mdkit_voice* v : all) {
        char s[16];
        unsigned k = 0;
        for (const char* c = v->name; *c && k < 15; ++c) s[k++] = *c == '-' ? '_' : static_cast<char>(*c | 0x20);
        s[k] = 0;
        if (!std::strcmp(s, slug)) return v;
    }
    return 0;
}

#ifndef MNMDREF_NO_MAIN
int main(int argc, char** argv) {
    const mdkit_voice* v = argc > 1 ? mnmdref_find(argv[1]) : 0;
    if (!v) return 1;
    void* st = std::calloc(1, v->state_bytes);
    float buf[32];
    v->init(st);
    for (int i = 2; i < argc; ++i) v->set_control(st, static_cast<unsigned>(i - 2), static_cast<unsigned>(std::atoi(argv[i])));
    v->trigger(st);
    for (int n = 0; n < 22050; n += 32) {
        v->render(st, buf, 32);
        std::fwrite(buf, 4, 32, stdout);
    }
    return 0;
}
#endif
