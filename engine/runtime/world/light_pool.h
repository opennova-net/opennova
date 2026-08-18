#pragma once

#include <cstdint>
#include <vector>

namespace opennova::world {

// THE DYNAMIC LIGHT POOL — retail's `Light_InstanceTable`, the one place every runtime
// light in Joint Operations lives: the ammo `light_impact` flash, the ammo `light_move`
// in-flight round glow, and the `MF_Light` muzzle glow all allocate out of it.
//
// WITNESSED SHAPE [orig: Light_InstanceTable @0x2732E28 — 4096 slots x 176 B]
//   +0x00  flags word (1 = live; bit 0x2 = disabled-but-kept; bit 0x10000 cleared at spawn)
//   +0x04  position x, y, z (Q16 fixed point)
//   +0x10  AABB min x,y,z  = position - radius   (Q16)
//   +0x1C  AABB max x,y,z  = position + radius   (Q16)
//   +0x28  radius (Q16)
//   +0x2C  float r, g, b   = colour byte / 256
//   +0x38  float blend     (the fade multiplier; 1.0 at spawn)
//   +0x3C  state
//   +0x40  ticks_left      (-1 = never expires)
//   +0x44  ticks_total
//   +0x48  rgbgen block pointer (UNPORTED — no producer witnessed for our three feeds)
//   +0x4C  group entity, +0x50 group section (interior light grouping)
//
// SPAWN [orig: LightPool_SpawnGlowEffect @0x5A8D50] — first slot whose flags word is 0
// among the active prefix, else append while the prefix is < 4096; the handle returned is
// `slot_index | 0x8000`. Retail additionally returns 0 unless three device-caps bits are
// set (`Render_DeviceCapsFlags & 7`); the SETTERS of those bits were not located, so the
// port treats the caps gate as satisfied (recorded gap).
//
// TICK, once per 62 Hz game tick [orig: EffectWorld_TickInstancesAndLightScale @0x5AA170,
// called from Game_ProcessMainFrame @0x5267A1]:
//   - a slot with ticks_left > 0 decrements; the tick that takes it to 0 either sets the
//     disabled flag 0x2 and KEEPS the slot (state 5) or memsets the whole record (any
//     other state);
//   - states 2 and 5 then recompute `blend = ticks_left / ticks_total` — LINEAR ON
//     INTENSITY ONLY. The radius never changes.
//   - the active prefix is compacted to `last_live_index + 1`.
//
// RENDER [orig: Light_FillD3DPointLight @0x5AA450] — a D3D point light whose
// Diffuse = rgb x blend x EffectWorld_AmbientScale x 1.5 and whose
// Range = 1.25 x radius_units, with attenuation (1, 0, 15/Range^2) => exactly 1/16
// brightness at Range. A slot with flags == 0 or flags & 0x2 fills nothing.
//
// This object is the pure state machine only: allocation, the tick, the fade, and the
// derived render parameters. Which lights a given draw call collects, and the
// four-simultaneous-enable cap [orig: update_light_slots @0x5ABC50], are the host's job.
class LightPool {
public:
	// [orig: the 4096 bound @0x5A8D50 `if (active >= 4096) return 0`]
	static constexpr int kMaxLights = 4096;
	// [orig: handle = index | 0x8000 @0x5A8D50]
	static constexpr int kHandleBit = 0x8000;
	static constexpr int kHandleIndexMask = 0x7FFF;
	// Flags word bits [orig: spawn sets 1 @0x5A8D50; the tick ORs 2 @0x5AA170; the render
	// path rejects on `!(word)flags || (flags & 2)` @0x5AA450].
	static constexpr uint32_t kFlagLive = 0x1;
	static constexpr uint32_t kFlagDisabled = 0x2;
	// The render-flag bit AmmoDef_ProcessImpactEffect sets on the impact flash right after
	// spawning it [orig: CEffectInstance_ModifyRenderFlags(handle, 256, 0) @0x40A2C0].
	// NO READER of this bit was found anywhere in jointops.exe's .text — it is carried so
	// the record is faithful, and it gates nothing here (recorded gap).
	static constexpr uint32_t kFlagImpactRender = 0x100;

	// The witnessed state values our three feeds use.
	//   1 = constant, no fade  (`light_move`, the in-flight round glow)
	//   2 = fade then FREE     (`light_impact`, the impact flash)
	//   3 = the muzzle glow's spawn state (with ticks -1, i.e. parked)
	//   4 = the muzzle glow's per-shot re-arm state
	//   5 = fade then DISABLE, keeping the slot
	// [orig: light_move @0x4EC8DA state 1 / ticks -1; light_impact @0x40A2B3 state 2;
	//  muzzle spawn @0x56C987 state 3 / ticks -1, re-arm @0x56C9A2 state 4 / ticks 5]
	static constexpr int kStateConstant = 1;
	static constexpr int kStateFadeFree = 2;
	static constexpr int kStateMuzzleParked = 3;
	static constexpr int kStateMuzzleShot = 4;
	static constexpr int kStateFadeDisable = 5;

	// The Diffuse multiplier and the Range factor of the D3D fill
	// [orig: Light_FillD3DPointLight @0x5AA450 — `* 1.5` on each channel; the radius
	// scale 0.000019073486 = 1.25 / 65536].
	static constexpr float kDiffuseScale = 1.5f;
	static constexpr float kRangeScale = 1.25f;
	// The quadratic attenuation numerator: atten2 = 15 / Range^2, so brightness at
	// Range is 1/(1 + 15) = 1/16 [orig: @0x5AA450].
	static constexpr float kAttenQuadratic = 15.0f;

	struct Light {
		uint32_t flags = 0;      // +0x00
		int32_t pos[3] = { 0, 0, 0 }; // +0x04 Q16
		int32_t bbox_min[3] = { 0, 0, 0 }; // +0x10 Q16
		int32_t bbox_max[3] = { 0, 0, 0 }; // +0x1C Q16
		int32_t radius = 0;      // +0x28 Q16
		float rgb[3] = { 0.0f, 0.0f, 0.0f }; // +0x2C..0x34 (byte / 256)
		float blend = 0.0f;      // +0x38
		int32_t state = 0;       // +0x3C
		int32_t ticks_left = 0;  // +0x40 (-1 = never expires)
		int32_t ticks_total = 0; // +0x44
		int32_t group_entity = 0; // +0x4C
		int32_t group_section = 0; // +0x50

		bool live() const { return (flags & 0xFFFFu) != 0; }
		// The render gate [orig: @0x5AA450 head].
		bool renderable() const { return live() && (flags & kFlagDisabled) == 0; }
	};

	LightPool() : slots_(static_cast<size_t>(kMaxLights)) {}

	// Allocate a glow. `position`/`radius` are Q16; `packed_color` is 0xRRGGBB;
	// `ticks` of -1 never expires. Returns `index | 0x8000`, or 0 when the pool is full
	// (retail's own "no light" answer) [orig: LightPool_SpawnGlowEffect @0x5A8D50].
	int spawn(const int32_t position[3], int32_t radius, uint32_t packed_color, int32_t state,
	          int32_t ticks);

	// The two post-spawn mutators the muzzle glow uses every shot
	// [orig: sub_5A8F80 @0x5A8F80 sets +0x3C state and +0x40/+0x44 = ticks;
	//  sub_5A8FB0 @0x5A8FB0 sets the +0x4C/+0x50 group pair]. Both no-op on a handle
	// without the 0x8000 bit, exactly as the originals do.
	void set_state_and_ticks(int handle, int32_t state, int32_t ticks);
	void set_group(int handle, int32_t entity, int32_t section);
	// [orig: CEffectInstance_SetBlendAmount @0x5A8EE0] — ALSO drives the disabled bit:
	// blend >= 0.001 clears 0x2, below that sets it. The muzzle glow calls it with 1.0
	// every shot, which is what un-disables a slot the fade had switched off.
	void set_blend(int handle, float blend);
	// [orig: CEffectInstance_SetPositionAndBounds @0x5A9070 — position + the +-radius AABB]
	void set_position(int handle, const int32_t position[3]);
	// [orig: CEffectInstance_ModifyRenderFlags @0x5A8F20, called on the impact flash
	// @0x40A2C0 — clears first, then sets, on the 16-bit flags word]
	void modify_render_flags(int handle, uint32_t set_bits, uint32_t clear_bits);

	// Explicit release (the `light_move` glow is cleared with its round)
	// [orig: Projectile_ReleaseEffects @0x4E8308 clears round+0x1B4].
	void release(int handle);

	// One 62 Hz game tick [orig: EffectWorld_TickInstancesAndLightScale @0x5AA170].
	void tick();

	void reset();

	// Handle -> slot, or nullptr when the handle is 0/out of range/not live.
	const Light *get(int handle) const;

	int active_count() const { return active_; }
	// Live slots in the active prefix — diagnostics.
	int live_count() const;

	const std::vector<Light> &slots() const { return slots_; }

	// The derived D3D point-light parameters for one slot, host-agnostic
	// [orig: Light_FillD3DPointLight @0x5AA450]. `ambient_scale` is retail's
	// EffectWorld_AmbientScale{R,G,B} triple (@0x840B24..0x840B2C); its PRODUCER was not
	// located, so hosts pass 1.0 and record the gap. Returns false when the slot fills
	// nothing (free or disabled).
	struct PointLight {
		float diffuse[3] = { 0.0f, 0.0f, 0.0f };
		float position[3] = { 0.0f, 0.0f, 0.0f }; // world units (Q16 -> float, no axis map)
		float range = 0.0f;      // 1.25 x radius_units
		float atten_const = 1.0f;
		float atten_linear = 0.0f;
		float atten_quadratic = 0.0f; // 15 / range^2
	};
	bool fill_point_light(int handle, const float ambient_scale[3], PointLight *out) const;

	// The per-frame render selection: every renderable slot in the active prefix,
	// sorted NEAREST-FIRST by squared centre distance, clamped to `max_lights` —
	// retail's own cap is FOUR [orig: update_light_slots @0x5ABC50
	// `if (active_count >= 4) goto disable` -> LightEnable]. Retail's order comes from
	// collect_nearby_zones_by_aabb @0x5AA250, whose tail bubble-sorts the collected
	// slots by squared centre distance ascending before storing them.
	// RECORDED DIVERGENCE (D-RLIT catalog): retail re-collects PER DRAW against each
	// object's AABB centre (<=4 candidates for an entity [orig: @0x5D6B00 `push 4`],
	// <=16 for a terrain block [orig: @0x609658 `push 0x10`]); a global-uniform set has
	// no per-draw seam, so the host selects once per frame against the CAMERA.
	// `camera_pos` is in world units (floats); `ambient_scale` as fill_point_light.
	// Returns the number of rows written to `out` (at most max_lights).
	int collect_render_lights(const float camera_pos[3], const float ambient_scale[3],
	                          PointLight *out, int max_lights) const;

	// Retail's simultaneous-enable cap [orig: update_light_slots @0x5ABC50].
	static constexpr int kMaxSimultaneous = 4;

private:
	Light *resolve(int handle);
	const Light *resolve(int handle) const;

	std::vector<Light> slots_;
	// Retail's `dword_2732DE4`: the ACTIVE PREFIX length, not a live count — the tick
	// compacts it down to the last live index + 1 [orig: @0x5AA170].
	int active_ = 0;
};

} // namespace opennova::world
