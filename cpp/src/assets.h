// Files packed into the executable at build time (see cmake/embed.cmake).
#pragma once
#include <cstddef>

extern const unsigned char ASSET_FONT[];        // pygame's default font (freesansbold.ttf)
extern const size_t ASSET_FONT_LEN;
extern const unsigned char ASSET_GROUND[];      // the title screen's mossy stone ground
extern const size_t ASSET_GROUND_LEN;
