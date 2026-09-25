#pragma once

#include <array>
#include <cstdint>

#include <godot_cpp/variant/vector3.hpp>

#include <runtime/particle/effect_scene.h>

namespace opennova::world {
class OcclusionWorld;
}

namespace godot {

class Simulation;

// The effect-group section gate's two inputs off a live simulation
// (particle::EffectSectionGate): the blink volumes containing a spawn point
// and the raw per-building section words of this frame's occlusion pass.
// Borrowed for one spawn or one advance; a null simulation answers no hits and
// leaves every group visible.
class EffectSectionSource final : public opennova::particle::EffectSectionMasks {
public:
	explicit EffectSectionSource(Simulation *p_simulation);

	// The descriptor spawn's blink stamp at a Godot-space point.
	void blink_hits(const Vector3 &p_godot_position, std::array<uint32_t, 4> &r_hits) const;
	// True when a simulation with occlusion state backs the masks.
	bool valid() const { return occlusion_ != nullptr; }
	std::uint32_t section_mask(std::int32_t p_pool_entity_index) const override;

private:
	Simulation *simulation_ = nullptr;
	const opennova::world::OcclusionWorld *occlusion_ = nullptr;
};

} // namespace godot
