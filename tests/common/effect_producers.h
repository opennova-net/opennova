// A world's effect scene opened to its producers (world::ItemEmitterSystem::open_producers,
// D-PTL-26): the script effects and the +0x1CC positioned emitters (a hit's, a corpse's decay)
// spawn into it where their event runs, as the shell's scene receives them past a load's warm
// reset. A world left closed spawns none of them (a dedicated host's, a headless kernel's).
#pragma once

#include <runtime/particle/effect_scene.h>
#include <runtime/world/item_effects.h>
#include <runtime/world/world.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace test_effects {

// A scene whose catalog holds each named effect over one forever-emitting particle, bound to the
// world and opened to its producers.
inline std::shared_ptr<opennova::particle::EffectScene> open_producers(
		opennova::world::World &world, const std::vector<std::string> &effects) {
	namespace p = opennova::particle;
	p::EffectSceneConfig config;
	config.documents.push_back({"producers.ptl", {}});
	p::ParticleDef particle;
	particle.id = "producer_particle";
	particle.flags = p::particle_flag::ForeverEmit;
	particle.emit_burst = 1;
	particle.emit_rate = 20;
	particle.age = 0.05f;
	config.documents[0].file.particles.push_back(particle);
	for (const std::string &name : effects)
		config.documents[0].file.effects.push_back({name, {"producer_particle"}});
	auto scene = std::make_shared<p::EffectScene>();
	scene->open(config);
	world.item_emitters.bind_scene(scene);
	world.item_emitters.open_producers(world, {});
	return scene;
}

// The live group an entity's +0x1CC slot holds, as the scene inspects it; none for an empty slot.
inline std::optional<opennova::particle::EffectGroupDebugSnapshot> slot_row(
		const opennova::particle::EffectScene &scene, opennova::world::EntityHandle entity) {
	const opennova::particle::EffectGroupId group =
			scene.slot_group(opennova::world::entity_effect_slot(entity.packed));
	if (!group) return std::nullopt;
	for (const auto &row : scene.inspect(false).groups)
		if (row.id == group && !row.detached) return row;
	return std::nullopt;
}

// The live (undetached) groups of the effect named `name`.
inline int live_groups_named(const opennova::particle::EffectScene &scene, const std::string &name) {
	int count = 0;
	for (const auto &row : scene.inspect(false).groups)
		if (!row.detached && row.effect_name == name) ++count;
	return count;
}

// Every group of the effect named `name` the scene still holds, a detached one draining too: the
// spawns since the last reap.
inline int groups_named(const opennova::particle::EffectScene &scene, const std::string &name) {
	int count = 0;
	for (const auto &row : scene.inspect(false).groups)
		if (row.effect_name == name) ++count;
	return count;
}

} // namespace test_effects
