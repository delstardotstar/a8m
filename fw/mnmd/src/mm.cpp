#include "voice.hpp"
#include "swave.hpp"
#include "sid_6581.hpp"
#include "fmp.hpp"
#include "vo_6.hpp"
#include "gnd_sin.hpp"
#include "gnd_nois.hpp"

// Names, defaults and slots: MNMD's machines/<id>/definition.json (SYN page,
// top row then bottom, blanks dropped), defaults as round(127 x normalized).
MNMD_SYNTH(mnmd_voice_swave_saw, swave_saw, "SW-SAW", ("UNIL", "UNIW", "UNIX", "SUBX", "SUB1", "SUB2", "TUNE"),
           (0, 1, 2, 4, 5, 6, 7), (40, 30, 0, 0, 30, 0, 64))
MNMD_SYNTH(mnmd_voice_sid_6581, sid_6581, "SID-6581", ("PW", "PWAD", "PWRS", "WAVE", "MOD", "MSRC", "MFRQ", "TUNE"),
           (0, 1, 2, 3, 4, 5, 6, 7), (32, 0, 32, 38, 16, 32, 64, 64))
MNMD_SYNTH(mnmd_voice_swave_pulse, swave_pulse, "SW-PULSE", ("UNIL", "UNIW", "SUB1", "SUB2", "PW", "PWAD", "PWRS", "TUNE"),
           (0, 1, 2, 3, 4, 5, 6, 7), (30, 20, 30, 0, 64, 20, 32, 64))
MNMD_SYNTH(mnmd_voice_swave_ens, swave_ens, "SW-ENS", ("PCH2", "PCH3", "PCH4", "WAVE", "PW", "CHRL", "CHRW", "TUNE"),
           (0, 1, 2, 3, 4, 5, 6, 7), (84, 99, 62, 0, 64, 40, 40, 64))
MNMD_SYNTH(mnmd_voice_fmpstat, fmpstat, "FM-STAT", ("1FRQ", "1FIN", "1ENV", "1FB", "2FRQ", "2VOL", "TONE", "TUNE"),
           (0, 1, 2, 3, 4, 5, 6, 7), (82, 64, 92, 0, 114, 24, 80, 64))
MNMD_SYNTH(mnmd_voice_fmppar, fmppar, "FM-PAR", ("1FRQ", "1ENV", "2FRQ", "2ENV", "3FRQ", "3ENV", "TONE", "TUNE"),
           (0, 1, 2, 3, 4, 5, 6, 7), (82, 48, 103, 90, 114, 104, 72, 64))
MNMD_SYNTH(mnmd_voice_fmpdyn, fmpdyn, "FM-DYN", ("1FRQ", "1FEN", "1VOL", "1VEN", "2FRQ", "2ENV", "2FB", "TUNE"),
           (0, 1, 2, 3, 4, 5, 6, 7), (64, 64, 40, 92, 64, 88, 0, 64))
MNMD_SYNTH(mnmd_voice_vo_6, vo_6, "VO-6", ("VOC1", "VOC2", "VSW", "VOIC", "CONS", "CLEN", "CVOL", "TUNE"),
           (0, 1, 2, 3, 4, 5, 6, 7), (120, 58, 95, 0, 3, 30, 80, 64))
MNMD_SYNTH(mnmd_voice_gnd_sin, gnd_sin, "GND-SIN", ("TUNE"), (7), (64))
MNMD_SYNTH(mnmd_voice_gnd_nois, gnd_nois, "GND-NOIS", ("ST", "RED", "STON", "TUNE", "RES"), (0, 1, 2, 7, 5),
           (0, 0, 32, 64, 0))

extern "C" int mnmd_swave_ens_step(unsigned v) { return v < 128 ? md::swave_ens::step_of[v] : 0; }
