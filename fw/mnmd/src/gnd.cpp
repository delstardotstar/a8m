#include "voice.hpp"
#include "gnd_sn.hpp"
#include "gnd_ns.hpp"
#include "gnd_im.hpp"

// Defaults: MNMD's design (tools/art/design.mjs), as its kit starts them.
MNMD_VOICE(mnmd_voice_gnd_sn, gnd_sn, "GND-SN", ("PTCH", "DEC", "RAMP", "RDEC"), (64, 50, 40, 40))
MNMD_VOICE(mnmd_voice_gnd_ns, gnd_ns, "GND-NS", ("DEC"), (50))
MNMD_VOICE(mnmd_voice_gnd_im, gnd_im, "GND-IM", ("UP", "UVAL", "DOWN", "DVAL"), (64, 127, 64, 0))
