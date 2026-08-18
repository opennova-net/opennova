#include "world/light_pool.h"

#include <algorithm>

namespace opennova::world {

namespace {
// The three colour channels are the packed 0xRRGGBB bytes divided by 256 — NOT 255
// [orig: LightPool_SpawnGlowEffect @0x5A8D50 `* 0.00390625` on BYTE2/BYTE1/BYTE0].
constexpr float kByteToFloat = 1.0f / 256.0f;
} // namespace

int LightPool::spawn(const int32_t position[3], int32_t radius, uint32_t packed_color,
                     int32_t state, int32_t ticks) {
	// The free-slot walk over the ACTIVE PREFIX, then the append
	// [orig: @0x5A8D50 — scan `while (*flags)`, else `if (active >= 4096) return 0`].
	int index = -1;
	for (int i = 0; i < active_; ++i) {
		if (!slots_[static_cast<size_t>(i)].live()) {
			index = i;
			break;
		}
	}
	if (index < 0) {
		if (active_ >= kMaxLights) return 0;
		index = active_++;
	}

	Light &e = slots_[static_cast<size_t>(index)];
	e = Light{};
	// `*(_WORD *)entry = 1; *(_DWORD *)entry &= ~0x10000` — the low word is the flags,
	// bit 0x10000 of the same dword is cleared [orig: @0x5A8D50].
	e.flags = kFlagLive;
	for (int a = 0; a < 3; ++a) {
		e.pos[a] = position[a];
		e.bbox_min[a] = position[a] - radius;
		e.bbox_max[a] = position[a] + radius;
	}
	e.radius = radius;
	e.rgb[0] = static_cast<float>((packed_color >> 16) & 0xFFu) * kByteToFloat;
	e.rgb[1] = static_cast<float>((packed_color >> 8) & 0xFFu) * kByteToFloat;
	e.rgb[2] = static_cast<float>(packed_color & 0xFFu) * kByteToFloat;
	e.blend = 1.0f;   // [orig: `*((float *)entry + 14) = 1.0` @0x5A8D50]
	e.state = state;
	e.ticks_left = ticks;
	e.ticks_total = ticks;
	e.group_entity = 0;   // [orig: `*((_DWORD *)entry + 18) = 0`]
	e.group_section = 0;
	return index | kHandleBit;
}

LightPool::Light *LightPool::resolve(int handle) {
	if ((handle & kHandleBit) == 0) return nullptr;
	const int index = handle & kHandleIndexMask;
	if (index < 0 || index >= kMaxLights) return nullptr;
	return &slots_[static_cast<size_t>(index)];
}

const LightPool::Light *LightPool::resolve(int handle) const {
	if ((handle & kHandleBit) == 0) return nullptr;
	const int index = handle & kHandleIndexMask;
	if (index < 0 || index >= kMaxLights) return nullptr;
	return &slots_[static_cast<size_t>(index)];
}

const LightPool::Light *LightPool::get(int handle) const {
	const Light *e = resolve(handle);
	return (e != nullptr && e->live()) ? e : nullptr;
}

void LightPool::set_state_and_ticks(int handle, int32_t state, int32_t ticks) {
	// [orig: sub_5A8F80 @0x5A8F80 — v3[15] = state, v3[17] = v3[16] = ticks]
	Light *e = resolve(handle);
	if (e == nullptr) return;
	e->state = state;
	e->ticks_left = ticks;
	e->ticks_total = ticks;
}

void LightPool::set_group(int handle, int32_t entity, int32_t section) {
	// [orig: sub_5A8FB0 @0x5A8FB0 — +0x4C / +0x50]
	Light *e = resolve(handle);
	if (e == nullptr) return;
	e->group_entity = entity;
	e->group_section = section;
}

void LightPool::set_blend(int handle, float blend) {
	// [orig: CEffectInstance_SetBlendAmount @0x5A8EE0 — the 0.001 threshold drives 0x2]
	Light *e = resolve(handle);
	if (e == nullptr) return;
	e->blend = blend;
	if (blend >= 0.001f) {
		e->flags &= ~kFlagDisabled;
	} else {
		e->flags |= kFlagDisabled;
	}
}

void LightPool::set_position(int handle, const int32_t position[3]) {
	// [orig: CEffectInstance_SetPositionAndBounds @0x5A9070 — the AABB rides the radius]
	Light *e = resolve(handle);
	if (e == nullptr) return;
	for (int a = 0; a < 3; ++a) {
		e->pos[a] = position[a];
		e->bbox_min[a] = position[a] - e->radius;
		e->bbox_max[a] = position[a] + e->radius;
	}
}

void LightPool::modify_render_flags(int handle, uint32_t set_bits, uint32_t clear_bits) {
	// [orig: CEffectInstance_ModifyRenderFlags @0x5A8F20 — clear, then set, 16-bit word]
	Light *e = resolve(handle);
	if (e == nullptr) return;
	e->flags &= ~(clear_bits & 0xFFFFu);
	e->flags |= (set_bits & 0xFFFFu);
}

void LightPool::release(int handle) {
	// The explicit free: retail's owners drop the handle and the record is memset by the
	// same path the tick uses [orig: Projectile_ReleaseEffects @0x4E8308 clears round+0x1B4].
	Light *e = resolve(handle);
	if (e == nullptr) return;
	*e = Light{};
}

void LightPool::tick() {
	// [orig: EffectWorld_TickInstancesAndLightScale @0x5AA170]
	const int scanned = active_;
	int new_count = 0;
	for (int i = 0; i < scanned; ++i) {
		Light &e = slots_[static_cast<size_t>(i)];
		if (!e.live()) continue;
		new_count = i + 1;   // the compaction: last live index + 1
		if (e.ticks_left > 0) {
			const int32_t before = e.ticks_left;
			e.ticks_left = before - 1;
			if (before == 1) {
				if (e.state == kStateFadeDisable) {
					e.flags |= kFlagDisabled;   // state 5: keep the slot, switch it off
				} else {
					e = Light{};                // every other state: free the record
				}
			}
		}
		// Read AFTER the possible free, exactly as the original does — a freed slot has
		// state 0 and takes no blend write.
		if (e.state != kStateConstant &&
		    (e.state == kStateFadeFree || e.state == kStateFadeDisable)) {
			// LINEAR ON INTENSITY. The radius is untouched.
			e.blend = e.ticks_total != 0
			                  ? static_cast<float>(e.ticks_left) / static_cast<float>(e.ticks_total)
			                  : 0.0f;
		}
	}
	active_ = new_count;
	// NOTE: retail also recomputes EffectWorld_AmbientScale{R,G,B} here from
	// `Env_TerrainColorRecip @0x26C67F8` (byte / 128) [orig: @0x5AA170 tail]. The
	// PRODUCER of that packed global was not located, so the ambient triple is not
	// modelled here — hosts pass 1.0 to fill_point_light() and the gap is recorded.
}

void LightPool::reset() {
	for (int i = 0; i < active_; ++i) slots_[static_cast<size_t>(i)] = Light{};
	active_ = 0;
}

int LightPool::live_count() const {
	int n = 0;
	for (int i = 0; i < active_; ++i)
		if (slots_[static_cast<size_t>(i)].live()) ++n;
	return n;
}

bool LightPool::fill_point_light(int handle, const float ambient_scale[3],
                                 PointLight *out) const {
	// [orig: Light_FillD3DPointLight @0x5AA450]
	const Light *e = resolve(handle);
	if (e == nullptr || out == nullptr) return false;
	if (!e->renderable()) return false;   // `!(word)flags || (flags & 2)` -> -1
	for (int a = 0; a < 3; ++a) {
		out->diffuse[a] = e->rgb[a] * e->blend * ambient_scale[a] * kDiffuseScale;
		out->position[a] = static_cast<float>(e->pos[a]) / 65536.0f;
	}
	// `*((float *)outParams + 19) = radius * 0.000019073486` = 1.25 x radius / 65536.
	out->range = static_cast<float>(e->radius) / 65536.0f * kRangeScale;
	out->atten_const = 1.0f;
	out->atten_linear = 0.0f;
	out->atten_quadratic = out->range > 0.0f ? kAttenQuadratic / (out->range * out->range) : 0.0f;
	// NOT PORTED: the optional RgbGen modulation (`slot+0x48` -> Light_TickGenBlock +
	// RgbGen_EvaluateColor @0x5AA450). None of our three feeds sets that pointer — the
	// producer is unwitnessed, so no code here can honestly drive it.
	return true;
}

int LightPool::collect_render_lights(const float camera_pos[3], const float ambient_scale[3],
                                     PointLight *out, int max_lights) const {
	if (out == nullptr || max_lights <= 0) return 0;
	struct Row {
		float dist_sq;
		PointLight pl;
	};
	std::vector<Row> rows;
	for (int i = 0; i < active_; ++i) {
		const Light &e = slots_[static_cast<size_t>(i)];
		if (!e.renderable()) continue;
		Row r;
		if (!fill_point_light(i | kHandleBit, ambient_scale, &r.pl)) continue;
		float d2 = 0.0f;
		for (int a = 0; a < 3; ++a) {
			const float d = r.pl.position[a] - camera_pos[a];
			d2 += d * d;
		}
		r.dist_sq = d2;
		rows.push_back(r);
	}
	// Squared centre distance ascending — the order retail's bubble sort produces
	// [orig: collect_nearby_zones_by_aabb @0x5AA250 tail]; then the hard cap
	// [orig: update_light_slots @0x5ABC50].
	std::sort(rows.begin(), rows.end(),
	          [](const Row &a, const Row &b) { return a.dist_sq < b.dist_sq; });
	const int n = std::min(static_cast<int>(rows.size()), max_lights);
	for (int i = 0; i < n; ++i) out[i] = rows[i].pl;
	return n;
}

} // namespace opennova::world
