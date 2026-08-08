#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::dep {

// The .dep depth-buffer intermediate (Output.dep): a raw little-endian
// 1024x1024 uint16 dump with no header or magic — the terrain bake's hand-off
// from the depth pass to the CPT export. Not a shipped retail asset (the
// builder treats it as a deletable intermediate); the file knowledge lives
// here so it has exactly one home (ADR 0030). Correctness instrument: the
// dvd4 bake parity chain exercises write->read in place of a roundtrip test.
inline constexpr size_t kDepthDim = 1024;
inline constexpr size_t kDepthSamples = kDepthDim * kDepthDim;

// Read a .dep file. Always returns kDepthSamples values — a short or missing
// file reads as zero-filled from the first absent sample, matching the
// historical bake reader's behavior.
std::vector<uint16_t> read(const std::string &path);

// Write `count` u16 samples raw. Returns false when the file cannot be
// created.
bool write(const std::string &path, const uint16_t *samples, size_t count);

}  // namespace opennova::dep
