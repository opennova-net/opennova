// The loader strip decode every renderer pass performs over a parsed
// Threedi3di3 LOD, plus the material-id -> material-array lookup that goes with
// it. One implementation for the portable terrain static-shadow geometry
// (engine/runtime/terrain) and the CPU model mesh preparation
// (engine/runtime/renderer) — previously two verbatim copies.
// [orig: the STRP decode as the OED reader walks it, basic loop @ 0x474CAF,
//  skinned @ 0x474B60 (ModSuperOed.exe); record fields in
//  docs/threedi/3di-gp-format-re.md STRP/ROBJ].
#pragma once

#include <formats/threedi/threedi_3di3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::threedi {

// The material-array slot for an authored material index: the first material
// whose .index matches, else the index itself when it is in range, else -1.
inline int threedi_material_array_index_for_id(const Threedi3di3 &model,
                                               int32_t material_index) {
	for (uint32_t i = 0; i < model.material_count; ++i) {
		if (model.materials[i].index == material_index) {
			return static_cast<int>(i);
		}
	}
	if (material_index >= 0 &&
			static_cast<uint32_t>(material_index) < model.material_count) {
		return material_index;
	}
	return -1;
}

// Pick the relative/absolute index convention that stays in the strip's
// vertex window, unroll with strip parity winding, and drop degenerates.
// False when the strip's ranges fall outside the LOD buffers or an index
// escapes the vertex window under either convention.
inline bool threedi_decode_strip_indices(const ThreediLod &lod,
		const ThreediTriangleStrip &strip, std::vector<uint16_t> &out) {
	out.clear();
	if (lod.indices.indices == nullptr || lod.vertices.items == nullptr ||
			strip.num_indices == 0 || strip.num_vertices <= 0) {
		return false;
	}
	if (strip.index_offset < 0 || strip.start_vertex < 0) {
		return false;
	}
	const uint32_t index_offset = static_cast<uint32_t>(strip.index_offset);
	const uint32_t index_count = strip.num_indices;
	const uint32_t vertex_offset = static_cast<uint32_t>(strip.start_vertex);
	const uint32_t vertex_count = static_cast<uint32_t>(strip.num_vertices);
	if (index_offset + index_count > lod.indices.count ||
			vertex_offset + vertex_count > lod.vertices.count) {
		return false;
	}

	const uint16_t *raw = lod.indices.indices + index_offset;
	uint16_t min_idx = 0xffffu;
	uint16_t max_idx = 0;
	for (uint32_t i = 0; i < index_count; ++i) {
		const uint16_t idx = raw[i];
		min_idx = std::min(min_idx, idx);
		max_idx = std::max(max_idx, idx);
	}
	const bool relative_valid = max_idx < vertex_count;
	const bool absolute_valid = min_idx >= vertex_offset &&
			static_cast<uint32_t>(max_idx) - vertex_offset < vertex_count;
	const bool use_absolute = absolute_valid && !relative_valid;

	auto to_local = [&](uint16_t idx, bool &ok) -> uint16_t {
		if (!use_absolute) {
			if (idx >= vertex_count) {
				ok = false;
				return 0;
			}
			return idx;
		}
		if (idx < vertex_offset) {
			ok = false;
			return 0;
		}
		const uint32_t local = static_cast<uint32_t>(idx) - vertex_offset;
		if (local >= vertex_count) {
			ok = false;
			return 0;
		}
		return static_cast<uint16_t>(local);
	};

	bool ok = true;
	if (!strip.is_strip) {
		out.reserve(index_count);
		for (uint32_t i = 0; i + 2 < index_count; i += 3) {
			const uint16_t a = to_local(raw[i], ok);
			const uint16_t b = to_local(raw[i + 1], ok);
			const uint16_t c = to_local(raw[i + 2], ok);
			if (!ok) return false;
			if (a == b || b == c || a == c) continue;
			out.push_back(a);
			out.push_back(b);
			out.push_back(c);
		}
	} else {
		out.reserve(static_cast<std::size_t>(index_count) * 3);
		for (uint32_t i = 0; i + 2 < index_count; ++i) {
			const bool odd = (i & 1u) != 0u;
			const uint16_t a = to_local(raw[i], ok);
			const uint16_t b = to_local(raw[i + (odd ? 2 : 1)], ok);
			const uint16_t c = to_local(raw[i + (odd ? 1 : 2)], ok);
			if (!ok) return false;
			if (a == b || b == c || a == c) continue;
			out.push_back(a);
			out.push_back(b);
			out.push_back(c);
		}
	}
	return true;
}


} // namespace opennova::threedi
