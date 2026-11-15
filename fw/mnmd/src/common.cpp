#include "voice.hpp"

namespace mnmd {
u32 owners;
const u32 middle_c[opxy::block_frames] = {
#define C8 md::kit::middle_c, md::kit::middle_c, md::kit::middle_c, md::kit::middle_c, \
           md::kit::middle_c, md::kit::middle_c, md::kit::middle_c, md::kit::middle_c
    C8, C8, C8, C8, C8, C8, C8, C8
#undef C8
};
}
