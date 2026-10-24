// A host that presents no entity (a dedicated host: inmatch::Session::presents_entities)
// drops what the entity presenter's passes drain each ticked frame, undrawn
// (world::discard_entity_presentation): the fire ring, the ready fire sounds, the slot
// sounds, the sound emitters, the destruction events and the vehicle effects. The
// fixed-size state and every other queue stay for their own drains: the fire sounds'
// countdown slots, the tips, the script sounds. The session's predicate: every role
// presents its entities but the dedicated host.

#include <runtime/inmatch/session.h>
#include <runtime/world/present_drains.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace {
namespace w = opennova::world;
namespace im = opennova::inmatch;

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                          \
		if (!(condition)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);           \
			++failures;                                                                \
		}                                                                           \
	} while (0)

// Every queue the passes drain, filled once, plus three a dedicated host's other
// consumers drain (the status page's tips, the shell's script sounds) or keep (the
// fire sounds' countdown slots).
void fill(w::World &world) {
	w::FireEvent fired;
	fired.ammo_index = 1;
	world.round_sim.fired.push_back(fired);
	// A stamped listener readies a near play; a far one takes a countdown slot.
	world.out.fire_sounds.set_listener(w::Vec3{0.0f, 0.0f, 0.0f});
	world.out.fire_sounds.play_immediate("AI_FIRE", w::Vec3{1.0f, 0.0f, 0.0f}, 0);
	world.out.fire_sounds.play_with_distance_delay("AI_FIRE", w::Vec3{200.0f, 0.0f, 0.0f}, 0);
	w::SoundSlotEvent slot;
	std::snprintf(slot.set_name, sizeof(slot.set_name), "%s", "FSP_DIRT_L");
	world.out.slot_sounds.push_back(slot);
	w::SoundEmitterEvent emitter;
	emitter.source_spawn_id = 7;
	emitter.source_handle = 0x1001;
	emitter.emitted_tick = 1;
	emitter.lifetime_ticks = 30;
	emitter.set_name = "V_TRUCK_ILP";
	CHECK(world.out.sound_emitters.publish(emitter));
	w::HuskSwapEvent husk;
	husk.net_id = 41;
	husk.item_id = 1291;
	world.out.destruction.husk_swaps.push_back(husk);
	world.out.destruction.effects.emplace_back();
	world.out.destruction.sounds.emplace_back();
	world.out.vehicle_effects.emplace_back();
	world.out.tip_events.push_back(3);
	w::ScriptSoundEvent script;
	script.name = "GS_END";
	world.out.script_sounds.push_back(script);
}

void test_the_passes_queues_drop_and_the_rest_stay() {
	auto world = std::make_unique<w::World>();
	fill(*world);
	CHECK(world->round_sim.fired.size() == 1);
	CHECK(world->out.fire_sounds.pending_count() == 1);
	CHECK(world->out.sound_emitters.size() == 1);

	w::discard_entity_presentation(*world);

	CHECK(world->round_sim.fired.empty());
	CHECK(world->out.fire_sounds.drain().empty());
	CHECK(world->out.slot_sounds.empty());
	CHECK(world->out.sound_emitters.empty());
	CHECK(world->out.destruction.husk_swaps.empty());
	CHECK(world->out.destruction.effects.empty());
	CHECK(world->out.destruction.sounds.empty());
	CHECK(world->out.vehicle_effects.empty());
	// The fixed-size countdown slot is state, not a queue: it plays when it expires.
	CHECK(world->out.fire_sounds.pending_count() == 1);
	// The status page's tips and the shell's script sounds have their own drains.
	CHECK(world->out.tip_events.size() == 1);
	CHECK(world->out.script_sounds.size() == 1);
	// A second call on an empty world is a no-op.
	w::discard_entity_presentation(*world);
	CHECK(world->round_sim.fired.empty());
}

// The fire ring is presentation alone: dropping it leaves the fire the wire sends.
void test_the_wire_half_stays() {
	auto world = std::make_unique<w::World>();
	world->out.entity_events.emplace_back(w::EntityRemoveEvent{});
	world->out.hud_relays.emplace_back();
	w::discard_entity_presentation(*world);
	CHECK(world->out.entity_events.size() == 1);
	CHECK(world->out.hud_relays.size() == 1);
}

// A role of a given kind and nothing else: the predicate reads the kind alone.
struct KindRole final : im::Role {
	im::RoleKind role_kind;
	explicit KindRole(im::RoleKind kind) : role_kind(kind) {}
	im::RoleKind kind() const override { return role_kind; }
	void run_tick(const im::TickInput &) override {}
	bool reset_to_baseline(im::SessionError &) override { return true; }
	void close() override {}
};

// Every role presents its entities but the dedicated host, whose render callback never
// takes the scene arm [orig: GameLoop_RenderFrame @0x521310 - is_mp_session_peer
// @0x52134E, `jz` @0x521355].
void test_the_dedicated_host_alone_presents_no_entity() {
	for (const im::RoleKind kind : {im::RoleKind::SinglePlayer, im::RoleKind::ListenHost,
				 im::RoleKind::Joiner, im::RoleKind::DedicatedHost}) {
		KindRole role(kind);
		const auto session = std::make_unique<im::Session>(role);
		CHECK(session->presents_entities() == (kind != im::RoleKind::DedicatedHost));
	}
}

} // namespace

int main() {
	test_the_passes_queues_drop_and_the_rest_stay();
	test_the_wire_half_stays();
	test_the_dedicated_host_alone_presents_no_entity();
	if (failures != 0) {
		std::printf("entity_presentation_discard_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("entity_presentation_discard_test passed\n");
	return 0;
}
