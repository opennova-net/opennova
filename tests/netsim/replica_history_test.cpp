#include <netsim/client_replica_pipeline.h>
#include <netsim/replica_history.h>

#include <npwire/ingame_encode.h>
#include <npwire/ingame_message_id.h>

#include <cstdio>
#include <utility>

namespace {
namespace nw = opennova;
namespace ns = opennova::netsim;

constexpr uint16_t kPlayerType = 0x14B9;
constexpr uint16_t kInfantryType = 0x0311;
constexpr uint16_t kVehicleType = 0x054F;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

nw::EntityClass classify(uint16_t type_id) {
	if (type_id == kPlayerType) return nw::EntityClass::Player;
	if (type_id == kInfantryType) return nw::EntityClass::Infantry;
	if (type_id == kVehicleType) return nw::EntityClass::Vehicle;
	return nw::EntityClass::Unknown;
}

const nw::ReplayEntity *find(
		const nw::ReplayTimeline &history, uint16_t handle) {
	for (const nw::ReplayEntity &entity : history.entities)
		if (entity.handle == handle) return &entity;
	return nullptr;
}

nw::InGameMessage server_message(
		int frame, uint8_t tag, std::vector<uint8_t> payload) {
	nw::InGameMessage message;
	message.frame_index = frame;
	message.dir = 'S';
	message.tag = tag;
	message.payload = std::move(payload);
	return message;
}

bool run_person_and_vehicle_share_the_incremental_fold() {
	ns::ClientReplicaPipeline pipeline(classify);
	ns::ReplicaHistory history;
	if (!expect(history.revision() == 0 && history.topology_revision() == 0,
			"fresh history starts with zero revisions")) return false;

	nw::OrganicSpawnBatch organics;
	nw::OrganicSpawnRecord player;
	player.has_body = true;
	player.slot_id = 0x0001;
	player.item_type_id = kPlayerType;
	player.entity_name = "remote player";
	player.net_id = 41;
	player.team = 1;
	player.pos_x = 10 << 16;
	organics.records.push_back(player);
	nw::OrganicSpawnRecord infantry = player;
	infantry.slot_id = 0x0002;
	infantry.item_type_id = kInfantryType;
	infantry.entity_name = "ai person";
	infantry.net_id = 42;
	infantry.team = 2;
	infantry.pos_x = 20 << 16;
	organics.records.push_back(infantry);
	organics.entity_count = static_cast<uint16_t>(organics.records.size());
	if (!expect(history.apply(server_message(1, nw::s2c::ENTITY_SPAWN_BATCH,
				nw::encode_organic_spawn_batch(organics)), pipeline),
			"organic spawn changes history")) return false;
	const uint64_t organic_revision = history.revision();
	const uint64_t organic_topology = history.topology_revision();

	nw::PoolSpawnBatch vehicles;
	nw::PoolSpawnRecord vehicle;
	vehicle.slot_id = 0x1003;
	vehicle.item_type_id = kVehicleType;
	vehicle.entity_name = "truck";
	vehicle.pos_x = 30 << 16;
	vehicles.records.push_back(vehicle);
	if (!expect(history.apply(server_message(2, nw::s2c::POOL_SPAWN,
				nw::encode_pool_spawn_batch(vehicles)), pipeline) &&
				history.revision() > organic_revision &&
				history.topology_revision() > organic_topology,
			"pool spawn advances state and topology revisions")) return false;

	nw::FrameUpdate frame;
	frame.flags2 = 0x02;
	frame.anchor_x = 8 << 16;
	frame.mount_handle = 0xFFFF;
	frame.health = 100;
	frame.env.fog_dist = 1000;
	frame.env.fog_accel = 200;
	frame.env.tod_fixed = 0x4000;

	nw::FrameUpdateRecord player_motion;
	player_motion.handle = player.slot_id;
	player_motion.type_id = player.item_type_id;
	player_motion.cls = nw::EntityClass::Player;
	player_motion.player.carrier_handle = 0xFFFF;
	player_motion.player.pos_x_compressed = nw::network_compress_fixedpoint(4 << 16);
	player_motion.player.yaw_byte = 0x40;
	player_motion.player.anim_state_id = 9;
	player_motion.player.anim_def_index = 0xFF;
	player_motion.player.health_class_byte = 0x28;
	frame.records.push_back(player_motion);

	nw::FrameUpdateRecord infantry_motion;
	infantry_motion.handle = infantry.slot_id;
	infantry_motion.type_id = infantry.item_type_id;
	infantry_motion.cls = nw::EntityClass::Infantry;
	infantry_motion.infantry.vehicle_slot_handle = 0xFFFF;
	infantry_motion.infantry.pos_x_compressed =
			nw::network_compress_fixedpoint(6 << 16);
	infantry_motion.infantry.yaw_byte = 0x80;
	infantry_motion.infantry.anim_byte = 11;
	frame.records.push_back(infantry_motion);

	nw::FrameUpdateRecord vehicle_motion;
	vehicle_motion.handle = vehicle.slot_id;
	vehicle_motion.type_id = vehicle.item_type_id;
	vehicle_motion.cls = nw::EntityClass::Vehicle;
	vehicle_motion.vehicle.parent_slot_handle = 0xFFFF;
	vehicle_motion.vehicle.pos_x_compressed =
			nw::network_compress_fixedpoint(8 << 16);
	vehicle_motion.vehicle.euler_z = 0x2000;
	vehicle_motion.vehicle.health_word = 100;
	frame.records.push_back(vehicle_motion);

	history.apply(server_message(3, nw::s2c::PER_FRAME_UPDATE,
			nw::encode_frame_update(frame)), pipeline);

	const nw::ReplayTimeline &timeline = history.timeline();
	const nw::ReplayEntity *player_history = find(timeline, player.slot_id);
	const nw::ReplayEntity *infantry_history = find(timeline, infantry.slot_id);
	const nw::ReplayEntity *vehicle_history = find(timeline, vehicle.slot_id);
	if (!expect(player_history != nullptr && infantry_history != nullptr &&
				vehicle_history != nullptr,
			"one history contains Player Person, AI Person, and Vehicle replicas"))
		return false;
	if (player_history->track.size() != 2 || infantry_history->track.size() != 2 ||
			vehicle_history->track.size() != 2)
		std::fprintf(stderr, "tracks: player=%zu infantry=%zu vehicle=%zu\n",
				player_history->track.size(), infantry_history->track.size(),
				vehicle_history->track.size());
	if (!expect(player_history->track.size() == 2 &&
				infantry_history->track.size() == 2 &&
				vehicle_history->track.size() == 2,
			"every motion family journals one spawn and one compact sample"))
		return false;
	if (!expect(player_history->track.back().x == 12 << 16 &&
				infantry_history->track.back().x == 14 << 16 &&
				vehicle_history->track.back().x == 16 << 16,
			"all compact families use the canonical anchor/decompress fold"))
		return false;
	if (!expect(player_history->name == "remote player" &&
				infantry_history->name == "ai person" &&
				vehicle_history->name == "truck",
	            "load-stream identity survives into incremental history"))
		return false;
	if (!expect(player_history->team_known && infantry_history->team_known &&
				!vehicle_history->team_known,
			"history copies explicit team presence instead of inferring from value"))
		return false;
	if (!expect(player_history->spawn.has_heading &&
				infantry_history->spawn.has_heading &&
				!vehicle_history->spawn.has_heading,
			"history copies explicit heading presence into spawn samples"))
		return false;
	if (!expect(timeline.environment.size() == 1 &&
				timeline.environment[0].fog_dist == 1000,
			"environment history comes from the same folded frame"))
		return false;

	const uint64_t before_team_revision = history.revision();
	const uint64_t before_team_topology = history.topology_revision();
	nw::TeamAssign assign;
	assign.entity_handle = vehicle.slot_id;
	assign.team = 2;
	if (!expect(history.apply(server_message(4, nw::s2c::TEAM_ASSIGN,
				nw::encode_team_assign(assign)), pipeline),
			"raw team assignment changes history metadata")) return false;
	vehicle_history = find(history.timeline(), vehicle.slot_id);
	const ns::ClientEntityState *vehicle_replica =
			pipeline.state().find(vehicle.slot_id);
	if (!expect(vehicle_history != nullptr && vehicle_history->team_known &&
				vehicle_history->team == 2 && vehicle_replica != nullptr &&
				vehicle_replica->team_known && vehicle_replica->team == 2 &&
				history.revision() > before_team_revision &&
				history.topology_revision() == before_team_topology,
			"team assignment follows one canonical replica/history path")) return false;

	const uint64_t before_destroy_topology = history.topology_revision();
	nw::DestroyEntityList sweep;
	sweep.pool0_indices.push_back(player.slot_id);
	if (!expect(history.apply(server_message(5, nw::s2c::EMPTY_SLOT_SWEEP,
				nw::encode_destroy_entity_list(sweep)), pipeline) &&
				pipeline.state().find(player.slot_id) == nullptr &&
				history.topology_revision() > before_destroy_topology,
			"raw empty-slot sweep advances topology despite retained history"))
		return false;

	const std::size_t player_samples = player_history->track.size();
	history.apply(server_message(6, nw::s2c::WEAPON_RELOAD,
			nw::encode_weapon_reload({player.slot_id, 7})), pipeline);
	return expect(find(history.timeline(), player.slot_id)->track.size() ==
				player_samples,
			"event-only messages do not duplicate the latest pose") &&
			expect(pipeline.drain_weapon_reloads().empty(),
					"history drains reload notifications it does not journal");
}

bool run_reused_slot_starts_a_fresh_history_generation() {
	ns::ClientReplicaPipeline pipeline(classify);
	ns::ReplicaHistory history;

	nw::OrganicSpawnRecord first;
	first.has_body = true;
	first.slot_id = 0x0001;
	first.item_type_id = kPlayerType;
	first.entity_name = "departed player";
	first.net_id = 41;
	first.team = 1;
	first.pos_x = 10 << 16;
	nw::OrganicSpawnBatch first_batch;
	first_batch.records.push_back(first);
	first_batch.entity_count = 1;
	history.apply(server_message(1, nw::s2c::ENTITY_SPAWN_BATCH,
			nw::encode_organic_spawn_batch(first_batch)), pipeline);

	nw::FrameUpdate first_frame;
	first_frame.anchor_x = 8 << 16;
	nw::FrameUpdateRecord first_motion;
	first_motion.handle = first.slot_id;
	first_motion.type_id = first.item_type_id;
	first_motion.cls = nw::EntityClass::Player;
	first_motion.player.carrier_handle = 0xFFFF;
	first_motion.player.pos_x_compressed =
			nw::network_compress_fixedpoint(4 << 16);
	first_motion.player.anim_def_index = 0xFF;
	first_frame.records.push_back(first_motion);
	history.apply(server_message(2, nw::s2c::PER_FRAME_UPDATE,
			nw::encode_frame_update(first_frame)), pipeline);

	nw::DestroyEntityList sweep;
	sweep.pool0_indices.push_back(first.slot_id);
	history.apply(server_message(3, nw::s2c::EMPTY_SLOT_SWEEP,
			nw::encode_destroy_entity_list(sweep)), pipeline);

	nw::OrganicSpawnRecord replacement = first;
	replacement.item_type_id = kInfantryType;
	replacement.entity_name = "replacement person";
	replacement.net_id = 77;
	replacement.team = 2;
	replacement.pos_x = 40 << 16;
	nw::OrganicSpawnBatch replacement_batch;
	replacement_batch.records.push_back(replacement);
	replacement_batch.entity_count = 1;
	history.apply(server_message(4, nw::s2c::ENTITY_SPAWN_BATCH,
			nw::encode_organic_spawn_batch(replacement_batch)), pipeline);

	nw::FrameUpdate replacement_frame;
	replacement_frame.anchor_x = 32 << 16;
	nw::FrameUpdateRecord replacement_motion;
	replacement_motion.handle = replacement.slot_id;
	replacement_motion.type_id = replacement.item_type_id;
	replacement_motion.cls = nw::EntityClass::Infantry;
	replacement_motion.infantry.vehicle_slot_handle = 0xFFFF;
	replacement_motion.infantry.pos_x_compressed =
			nw::network_compress_fixedpoint(12 << 16);
	replacement_frame.records.push_back(replacement_motion);
	history.apply(server_message(5, nw::s2c::PER_FRAME_UPDATE,
			nw::encode_frame_update(replacement_frame)), pipeline);

	const nw::ReplayTimeline &timeline = history.timeline();
	if (!expect(timeline.entities.size() == 2,
			"slot reuse appends a fresh replay entity generation")) return false;
	const nw::ReplayEntity &departed = timeline.entities[0];
	const nw::ReplayEntity &current = timeline.entities[1];
	return expect(departed.handle == first.slot_id &&
				departed.type_id == kPlayerType &&
				departed.name == "departed player" && departed.track.size() == 2,
			"the departed generation remains intact") &&
			expect(current.handle == replacement.slot_id &&
					current.type_id == kInfantryType &&
					current.name == "replacement person" &&
					current.has_spawn && current.spawn.x == 40 << 16 &&
					current.track.size() == 2 &&
					current.track.back().x == 44 << 16,
				"the replacement generation records its own spawn and first compact");
}

bool run_spectator_reload_applies_remote_person_arms_dip() {
	ns::ClientReplicaPipeline pipeline(classify);
	ns::ReplicaHistory history;

	nw::OrganicSpawnRecord player;
	player.has_body = true;
	player.slot_id = 0x0001;
	player.item_type_id = kPlayerType;
	player.entity_name = "remote player";
	nw::OrganicSpawnRecord infantry = player;
	infantry.slot_id = 0x0002;
	infantry.item_type_id = kInfantryType;
	infantry.entity_name = "remote infantry";
	nw::OrganicSpawnBatch organics;
	organics.records = {player, infantry};
	organics.entity_count = static_cast<uint16_t>(organics.records.size());
	history.apply(server_message(1, nw::s2c::ENTITY_SPAWN_BATCH,
			nw::encode_organic_spawn_batch(organics)), pipeline);

	nw::PoolSpawnRecord vehicle;
	vehicle.slot_id = 0x1003;
	vehicle.item_type_id = kVehicleType;
	vehicle.entity_name = "remote vehicle";
	nw::PoolSpawnBatch vehicles;
	vehicles.records.push_back(vehicle);
	history.apply(server_message(2, nw::s2c::POOL_SPAWN,
			nw::encode_pool_spawn_batch(vehicles)), pipeline);

	history.apply(server_message(3, nw::s2c::WEAPON_RELOAD,
			nw::encode_weapon_reload({player.slot_id, 7})), pipeline);
	history.apply(server_message(4, nw::s2c::WEAPON_RELOAD,
			nw::encode_weapon_reload({infantry.slot_id, 8})), pipeline);
	history.apply(server_message(5, nw::s2c::WEAPON_RELOAD,
			nw::encode_weapon_reload({vehicle.slot_id, 9})), pipeline);

	const ns::ClientEntityState *player_replica =
			pipeline.state().find(player.slot_id);
	const ns::ClientEntityState *infantry_replica =
			pipeline.state().find(infantry.slot_id);
	const ns::ClientEntityState *vehicle_replica =
			pipeline.state().find(vehicle.slot_id);
	return expect(player_replica != nullptr &&
				player_replica->arms_dip_ticks == 80,
			"spectator reload stamps the player Person arms dip") &&
			expect(infantry_replica != nullptr &&
					infantry_replica->arms_dip_ticks == 80,
				"spectator reload stamps the infantry Person arms dip") &&
			expect(vehicle_replica != nullptr &&
					vehicle_replica->arms_dip_ticks == 0,
				"spectator reload does not apply Person arms dip to vehicles") &&
			expect(pipeline.drain_weapon_reloads().empty(),
				"history consumes each reload notification exactly once");
}

} // namespace

int main() {
	if (!run_person_and_vehicle_share_the_incremental_fold()) return 1;
	if (!run_reused_slot_starts_a_fresh_history_generation()) return 1;
	if (!run_spectator_reload_applies_remote_person_arms_dip()) return 1;
	std::puts("OK: shared client replica pipeline journals Person and Vehicle history");
	return 0;
}
