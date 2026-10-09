#pragma once

#include <formats/pcx/pcx.h>

namespace opennova {

// An RGBA image reduced to the 256-color palette a PCX carries (ADR 0046 d10, S8):
// the distinct opaque colors as they are when 256 or fewer (a hand-made palette
// survives untouched), else a median cut over the color population in RGB, each
// pixel mapped to its nearest palette entry. Alpha is dropped: the format has none.
// Deterministic, so the same source imports to the same bytes.
IndexedImage8 quantize_to_256(const RgbaImage &image);

} // namespace opennova
