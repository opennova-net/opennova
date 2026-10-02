// The client half of the command map's squad and waypoint legs: the C2S
// sends, and the consequences of the S2C folds (replication
// client_replica_squad.cpp) — the pool-4 waypoint rows, the interface sounds,
// the HUD lines.
// Witness record: docs/interface/hud-re.md "The command map" (D-HUD-19).

#include <runtime/inmatch/client_runtime.h>

#include <net/npwire/ingame_message_id.h>
#include <net/npwire/squad_messages.h>
#include <runtime/audio/sound_profile.h>
#include <runtime/world/user_waypoints.h>
#include <runtime/world/world.h>

#include <algorithm>

namespace opennova::inmatch {

namespace {

// Sound_PlayInterfaceTriggerSet over a set name.
void play_interface(world::World &world, const std::string &name) {
	if (name.empty()) return;
	world::ScriptSoundEvent event;
	event.name = name;
	event.kind = world::ScriptSoundEvent::Kind::Interface;
	world.out.script_sounds.push_back(std::move(event));
}

// SoundProfile_FindByEntityAndType: "<body prefix>_<suffix>" for the entity
// behind a roster slot's pool-0 index [orig: @0x528180; the prefix
// Entity_GetBodyModelPrefix @0x5280f0 reads entity+0x374].
std::string entity_sound(const world::World &world, int16_t entity_slot, int type) {
	int anim_slot = 0;
	if (entity_slot >= 0) {
		if (const world::Entity *e = world.registry.get(world::EntityHandle::make(0, entity_slot)))
			anim_slot = e->anim_slot;
	}
	char name[64];
	audio::compose_entity_sound_set(anim_slot, type, name, sizeof(name));
	return name;
}

// The two recruit composites [orig: g_EntitySoundTypeTable @0x82f548 — 6
// RECRUIT_ACCEPT, 7 RECRUIT].
constexpr int kSoundRecruitAccept = 6;
constexpr int kSoundRecruit = 7;
// [orig: g_SndMPCommand1 @0x24E09F0 = the "MP_COMMAND1" set]
constexpr const char *kMpCommand1 = "MP_COMMAND1";

} // namespace

bool ClientRuntime::queue_squad_message(uint8_t c2s_tag, std::vector<uint8_t> body) {
	const bool joiner_path = role_ == Role::Joiner && joiner_ != nullptr && joiner_->in_session();
	const bool host_path = role_ == Role::HostClient && loopback_ != nullptr;
	if (host_path) {
		loopback_->client_send(c2s_tag, std::move(body));
		return true;
	}
	if (!joiner_path) return false;
	// QueueReliableMessage(tag, 1, 0): reliable, no finite lifetime; sent
	// outside the client net frame, so it rides the held one-shot queue.
	send_queue_.push_back(make_protocol_message(c2s_tag, std::move(body)));
	return true;
}

std::vector<hud::SquadFeedLine> ClientRuntime::drain_squad_lines() {
	std::vector<hud::SquadFeedLine> out;
	out.swap(pending_squad_lines_);
	return out;
}

std::vector<hud::FeedPost> ClientRuntime::drain_ring_posts() {
	std::vector<hud::FeedPost> out;
	out.swap(pending_ring_posts_);
	return out;
}

bool ClientRuntime::place_user_waypoint(world::World &world, int32_t x, int32_t y,
		const std::string &name) {
	const world::EntityHandle handle = world::place_user_waypoint(world, x, y, name);
	const world::Entity *wp = world.registry.get(handle);
	if (wp == nullptr) return false;
	// NetPacket_SendChatMessage(255, waypoint): the row's name and position
	// [orig: @0x54a1be — NetPacket_WriteTypeNameAndPosition @0x42b160].
	WaypointShare share;
	share.target = 0xFF;
	share.name = wp->display_name;
	share.x = x;
	share.y = y;
	share.z = world::to_fixed(wp->position.z);
	queue_squad_message(c2s::WAYPOINT_SHARE, encode_waypoint_share(share));
	return true;
}

bool ClientRuntime::delete_hovered_user_waypoint(world::World &world) {
	// The 0x4F precedes the destroy in retail; the handle is the same word
	// [orig: NetPacket_SendEntityUpdate @0x5479e6, j_Entity_Destroy @0x5479ee].
	const world::EntityHandle handle = world::delete_hovered_user_waypoint(world);
	if (!handle.valid()) return false;
	queue_squad_message(c2s::WAYPOINT_DELETE, encode_entity_handle16(handle.packed));
	return true;
}

void ClientRuntime::clear_user_waypoints(world::World &world) {
	// [orig: @0x548137 — one 0x4F per placed waypoint, in table order]
	std::vector<world::EntityHandle> removed;
	world::clear_user_waypoints(world, removed);
	for (const world::EntityHandle handle : removed)
		queue_squad_message(c2s::WAYPOINT_DELETE, encode_entity_handle16(handle.packed));
}

void ClientRuntime::send_go_code(world::World &world, uint8_t code) {
	// NetPacket_SendVoteKick (a misnomer) with entity+0x154, then
	// Server_PlayGoCodeSoundAndChat(local, code, 0) [orig: @0x5482ad, @0x5482bc].
	GoCode go;
	go.leader = static_cast<uint8_t>(local_roster_slot());
	go.code = code;
	queue_squad_message(c2s::GO_CODE, encode_go_code(go));
	replication::ClientSquadEvent event;
	event.kind = replication::ClientSquadEvent::Kind::GoCode;
	event.entity_slot = world.cached.local_player.valid()
			? static_cast<int16_t>(world.cached.local_player.slot())
			: static_cast<int16_t>(-1);
	event.value = code;
	event.mute = 0;
	event.feed_order = view_.claim_feed_order();
	apply_squad_event(world, event);
}

void ClientRuntime::send_squad_recruit(uint8_t target) {
	// NetPacket_SendTeamChange(local entity+0x154, the row's slot) (a
	// misnomer: the recruit) [orig: @0x54864e..0x54865c].
	const int slot = local_roster_slot();
	SquadRecruit recruit;
	recruit.recruiter = static_cast<uint8_t>(slot < 0 ? 0xFF : slot);
	recruit.target = target;
	queue_squad_message(c2s::SQUAD_RECRUIT, encode_squad_recruit(recruit));
}

void ClientRuntime::send_squad_join(uint8_t leader) {
	// [orig: NetPacket_SendWeaponSlotSwitch(slot) @0x548616 (a misnomer: the join)]
	queue_squad_message(c2s::SQUAD_JOIN_REQUEST, encode_squad_join_request(leader));
}

void ClientRuntime::send_fireteam_assign(uint8_t fireteam, const std::vector<uint8_t> &members) {
	// [orig: NetPacket_SendWeaponAction(fireteam, count, slots) @0x548d67]
	FireteamAssign assign;
	assign.fireteam = fireteam;
	assign.members = members;
	queue_squad_message(c2s::FIRETEAM_ASSIGN, encode_fireteam_assign(assign));
}

void ClientRuntime::send_squad_order(uint8_t kind, const std::string &text,
		const std::vector<uint8_t> &targets) {
	// [orig: NetPacket_SendCommandType44(kind, count, targets, text) @0x547839 /
	//  @0x548c25]
	SquadOrderRequest order;
	order.kind = kind;
	order.text = text;
	order.targets = targets;
	queue_squad_message(c2s::SQUAD_ORDER_REQUEST, encode_squad_order_request(order));
}

void ClientRuntime::send_punt_vote(uint8_t target) {
	// NetPacket_WriteByte(target) then CNapiNetwork_QueueReliableMessage(0x3F,
	// 1, 0) [orig: @0x54889b..0x5488b9].
	queue_squad_message(c2s::PUNT_VOTE, encode_punt_vote(target));
}

void ClientRuntime::apply_squad_event(world::World &world,
		const replication::ClientSquadEvent &event) {
	using Kind = replication::ClientSquadEvent::Kind;
	switch (event.kind) {
	case Kind::MemberJoined: {
		// [orig: NapiNPClientMsg_HandleSquadJoin @0x42565e..0x4256d1]
		if ((event.mute & 2u) == 0u)
			pending_squad_lines_.push_back(
					{hud::SquadFeedLine::Kind::Join, event.name, 0, event.feed_order});
		if ((event.mute & 1u) == 0u)
			play_interface(world, entity_sound(world, event.entity_slot, kSoundRecruitAccept));
		// Every placed waypoint goes to the new member: one C2S 0x17 per
		// non-null table entry, the member's own slot the target, the row's
		// name and its exact x / y / z dwords (a wiped row's zeros).
		// [orig: Server_BroadcastChatToAllPlayers @0x549200 (a misnomer: the
		//  push) — NetPacket_SendChatMessage(joiner+0x154, entry) per non-null
		//  entry @0x549210..0x54921f]
		for (const world::UserWaypointTable::Entry &entry : world.user_waypoints.entries) {
			if (!entry.handle.valid()) continue;
			const world::UserWaypointRow row = world::user_waypoint_row(world, entry.handle);
			WaypointShare share;
			share.target = event.slot;
			share.name = row.name;
			share.x = row.x;
			share.y = row.y;
			share.z = row.z;
			queue_squad_message(c2s::WAYPOINT_SHARE, encode_waypoint_share(share));
		}
		break;
	}
	case Kind::SquadLeft: {
		// Every pool-4 row with a def that is not one of this player's own
		// placed waypoints is destroyed.
		// [orig: Entity_DestroyUnreferencedPool4Entries @0x54b4b0]
		std::vector<world::EntityHandle> foreign;
		world.registry.for_each_in_pool(world::kUserWaypointPool, [&](const world::Entity &e) {
			if (e.item_type_index == 0) return;
			for (const world::UserWaypointTable::Entry &entry : world.user_waypoints.entries)
				if (entry.handle == e.handle) return;
			foreign.push_back(e.handle);
		});
		for (const world::EntityHandle h : foreign) world::destroy_pool_row(world, h);
		break;
	}
	case Kind::FireteamSet:
		// [orig: NapiNPClientMsg_0x073 @0x4257f4..0x42586a]
		if ((event.mute & 1u) == 0u) play_interface(world, kMpCommand1);
		pending_squad_lines_.push_back(
				{hud::SquadFeedLine::Kind::Fireteam, "", event.value, event.feed_order});
		break;
	case Kind::Recruited:
		// [orig: NapiNPClientMsg_PlayerRecruited @0x4258d9..0x425915]
		if ((event.mute & 1u) == 0u)
			play_interface(world, entity_sound(world, event.entity_slot, kSoundRecruit));
		if ((event.mute & 2u) == 0u)
			pending_squad_lines_.push_back(
					{hud::SquadFeedLine::Kind::Recruit, event.name, 0, event.feed_order});
		break;
	case Kind::GoCode: {
		// Server_PlayGoCodeSoundAndChat (a client function despite the name):
		// the leader's body prefix + "_GC_<code>", the line unless +50 & 2,
		// the set unless +50 & 1 (a code past 5 adds no suffix and no line).
		// [orig: @0x5522e0]
		std::string name;
		if (event.entity_slot >= 0) {
			if (const world::Entity *e =
							world.registry.get(world::EntityHandle::make(0, event.entity_slot)))
				name = audio::body_model_prefix(e->anim_slot);
		}
		if (event.value <= 5) {
			name += "_GC_";
			name += hud::go_code_name(event.value);
			if ((event.mute & 2u) == 0u)
				pending_squad_lines_.push_back(
						{hud::SquadFeedLine::Kind::GoCode, "", event.value, event.feed_order});
		}
		if ((event.mute & 1u) == 0u) play_interface(world, name);
		break;
	}
	case Kind::OrderSound:
		play_interface(world, kMpCommand1); // [orig: @0x425749]
		break;
	case Kind::WaypointCreate: {
		// [orig: NapiNPClientMsg_0x033 @0x425fea — Pool_GetEntryUnchecked(0,
		//  owner) -> Waypoint_CreateForPlayer]
		std::string line;
		world::waypoint_create_for_player(world, event.x, event.y, event.name.c_str(),
				world::EntityHandle::make(0, event.handle & 0xFFFu), line);
		if (!line.empty())
			pending_squad_lines_.push_back(
					{hud::SquadFeedLine::Kind::WaypointReceived, line, 0, event.feed_order});
		break;
	}
	case Kind::EntityDestroy: {
		// [orig: NapiNPClientMsg_0x07C @0x426020 — `(h & 0xF000) < 0x5000`,
		//  slot < the pool's capacity, then Entity_Destroy]
		const world::EntityHandle h{event.handle};
		if (h.pool() >= world::kEntityPoolCount) break;
		if (static_cast<size_t>(h.slot()) >= world.registry.pool_capacity(h.pool())) break;
		world::destroy_pool_row(world, h);
		break;
	}
	}
}

} // namespace opennova::inmatch
