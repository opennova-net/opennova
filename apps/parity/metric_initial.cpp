// The initial reliable stream of one direction: each direction's origin packets
// through its first gameplay packet (run_facts.h), judged as
//   initial_grouping   edit distance over the packet groups, a timer-phase identity
//                      left out; the C2S stream as one record sequence, since the
//                      client's send pump batches by load timing
//   initial_body       the material bodies by position over the common prefix
//   initial_count      how many each identity carries (the count rule)
//   initial_body_set   a phase-bound identity's distinct bodies (S2C 0x16, 0x46)
//   initial_records    the world stream decoded record by record, keyed by slot
//                      (S2C 0x0C organics, S2C 0x0D pool spawns, S2C 0x10 statics)
// Every route is the registry's (normalization.h).

#include "metric.h"

#include "normalization.h"

#include <net/npwire/ingame_decode.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace opennova::parity {

namespace {

// Each origin packet's grouping, through the first gameplay packet. A NACK replay
// repeats an earlier (SID, sequence); only the origin counts. A timer-phase
// identity (a FirstOrder exclusion) leaves the key, and a packet left with no
// record drops out. A boundary-tolerant direction yields one element per record,
// its packet's flags with its framing, so a merged or split packet costs nothing.
std::vector<std::string> initial_groups(const RunFacts &run, char dir) {
	static_assert(has_normalization("packet.sid_seq_ack"));
	static_assert(has_normalization("order.S.0x16"));
	static_assert(has_normalization("initial.C.pump_boundary"));
	std::map<std::pair<uint32_t, uint32_t>, int> earliest;
	for (const wire::ParityPacket &packet : run.trace.packets) {
		if (packet.dir != dir)
			continue;
		const auto key = std::make_pair(packet.sid, packet.seq);
		const auto found = earliest.find(key);
		if (found == earliest.end() || packet.frame < found->second)
			earliest[key] = packet.frame;
	}
	const bool flat = group_boundary_tolerant(dir);
	std::vector<std::string> groups;
	for (const wire::ParityPacket &packet : run.trace.packets) {
		if (packet.dir != dir || packet.records.empty())
			continue;
		if (earliest[std::make_pair(packet.sid, packet.seq)] != packet.frame)
			continue;
		wire::ParityPacket kept = packet;
		kept.records.clear();
		kept.tags.clear();
		for (const wire::ParityWireRecord &record : packet.records)
			if (!first_order_excluded(dir, record.full_tag)) {
				kept.records.push_back(record);
				kept.tags.push_back(record.full_tag);
			}
		if (flat) {
			char flags[8];
			std::snprintf(flags, sizeof(flags), "0x%02x", unsigned(packet.flags));
			for (const wire::ParityWireRecord &record : kept.records)
				groups.push_back(std::string(flags) + "|" + wire_key(dir, record));
		} else if (!kept.records.empty()) {
			groups.push_back(group_key(kept));
		}
		if (carries_low_tag(packet, gameplay_identity(dir)))
			break;
	}
	return groups;
}

// The frames of each direction's packets through its first gameplay packet.
std::set<int> initial_frames(const RunFacts &run) {
	std::set<int> frames;
	for (const char dir : { 'C', 'S' }) {
		for (const wire::ParityPacket &packet : run.trace.packets) {
			if (packet.dir != dir)
				continue;
			frames.insert(packet.frame);
			if (carries_identity(packet, gameplay_identity(dir)))
				break;
		}
	}
	return frames;
}

// One direction's initial material bodies per identity, in capture order.
std::map<uint16_t, std::vector<std::string>> initial_bodies(const RunFacts &run, char dir) {
	const std::set<int> frames = initial_frames(run);
	std::map<uint16_t, std::vector<std::string>> by_identity;
	for (const wire::ParityEvent &event : run.trace.events) {
		if (event.dir != dir || !event.material || !frames.count(event.frame))
			continue;
		by_identity[event_identity(event)].push_back(
				std::string(event.body.begin(), event.body.end()));
	}
	return by_identity;
}

// ---------------------------------------------------------------------------
// Decoded keys

// A field list: (name, rendered value).
using Fields = std::vector<std::pair<const char *, std::string>>;

std::string render(const Fields &fields) {
	std::string out;
	for (const auto &field : fields)
		out += std::string(out.empty() ? "" : "|") + field.first + "=" + field.second;
	return out;
}

std::string num(int64_t value) {
	return std::to_string(value);
}

std::string hex_body(const std::string &raw) {
	return hex_of(std::vector<uint8_t>(raw.begin(), raw.end()));
}

// S2C 0x46 with the NovaWorld session identity left out.
std::string player_sync_key(const std::string &raw) {
	static_assert(has_normalization("initial.S.0x46.timer_score"));
	PlayerSync sync;
	if (!decode_player_sync(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), sync))
		return "undecoded:" + hex_body(raw);
	char head[48];
	std::snprintf(head, sizeof(head), "slot=%u|mask=%04x|ack=%u", unsigned(sync.slot_id),
			unsigned(sync.field_bitmask), sync.queue_ack ? 1u : 0u);
	if (sync.removal)
		return std::string(head) + "|removal";
	const Fields fields = { { "entity", num(sync.entity_slot_id) }, { "name", sync.name },
		{ "team_string", sync.clan }, { "pcid", sync.id_label }, { "team", num(sync.team) },
		{ "downed", num(sync.downed_state) }, { "late_join", num(sync.field_1000) },
		{ "squad", num(sync.field_0040) }, { "side", num(sync.field_0080) },
		{ "quality", num(sync.quality) } };
	return std::string(head) + "|" + render(fields);
}

// The distinct bodies of a set-compared identity.
std::set<std::string> body_set(char dir, uint16_t identity, const std::vector<std::string> &raw) {
	std::set<std::string> out;
	for (const std::string &body : raw)
		out.insert(dir == 'S' && identity == 0x046 ? player_sync_key(body)
												   : stable_body_key(dir, identity, body));
	return out;
}

// One decoded world-stream record: its fields, plus the pose fields a live
// record leaves out and the physics state (contact flag, ground link) one live by
// its own physics leaves out (both flags are decided by RR's record).
struct WorldRecord {
	Fields fields;
	Fields pose;
	Fields state;
	bool live = false;
	bool self_live = false;
};

using WorldRecords = std::map<uint32_t, WorldRecord>; // by slot; the first sighting

void organic_records(const std::string &raw, WorldRecords &out) {
	static_assert(has_normalization("initial.S.0x0c.live_pose"));
	OrganicSpawnBatch batch;
	decode_organic_spawn_batch(
			reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), batch);
	const size_t complete = batch.records.size() - (batch.last_record_partial ? 1 : 0);
	for (size_t i = 0; i < complete; ++i) {
		const OrganicSpawnRecord &r = batch.records[i];
		WorldRecord record;
		record.fields = { { "def_type", num(r.def_type) } };
		if (r.has_body()) {
			record.fields.insert(record.fields.end(),
					{ { "type", num(r.item_type_id) }, { "owner", num(r.owner_connection_id) },
							{ "name", r.entity_name }, { "minimap_flags", num(r.minimap_flags) },
							{ "team", num(r.team) }, { "ai_state", num(r.ai_state) },
							{ "anim_slot", num(r.anim_slot) }, { "net_id", num(r.net_id) },
							{ "class", num(r.player_class) }, { "ai_action", num(r.ai_action) },
							{ "player_slot", num(r.player_slot_id) },
							{ "alert", num(r.alert_level) }, { "sub_type", num(r.sub_type) },
							{ "weapon", num(r.weapon_type) }, { "parent_slot", num(r.parent_slot) },
							{ "parent", num(r.parent_handle) } });
			record.pose = { { "x", num(r.pos_x) }, { "y", num(r.pos_y) }, { "z", num(r.pos_z) },
				{ "yaw", num(r.orientation) } };
			// minimapFlags bit 0 set: a hidden, unjoined body at its placement.
			record.live = (r.minimap_flags & 1u) == 0;
		}
		out.emplace(r.slot_id, std::move(record));
	}
}

void static_records(const std::string &raw, WorldRecords &out) {
	static_assert(has_normalization("world.S.0x10.zone_prefix"));
	StaticEntityBatch batch;
	decode_static_entity_batch(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), batch);
	const size_t complete = batch.records.size() - (batch.last_record_partial ? 1 : 0);
	for (size_t i = 0; i < complete; ++i) {
		const StaticEntityRecord &r = batch.records[i];
		WorldRecord record;
		record.fields = { { "type", num(r.item_type_id) } };
		if (!r.is_empty_slot) {
			char flags[16], entity_flags[16];
			std::snprintf(flags, sizeof(flags), "%04x", unsigned(r.field_flags));
			std::snprintf(entity_flags, sizeof(entity_flags), "%08x", unsigned(r.entity_flags));
			// The trailing zone byte and radius (weapon_byte / attach_ref) are the
			// serializer's uninitialized stack outs for a static before the page's
			// first zone-chain hit: left out.
			record.fields.insert(record.fields.end(),
					{ { "field_flags", flags }, { "x", num(r.pos_x) }, { "y", num(r.pos_y) },
							{ "z", num(r.pos_z) }, { "euler_z", num(r.euler_z) },
							{ "euler_x", num(r.euler_x) }, { "euler_y", num(r.euler_y) },
							{ "section", num(r.section_mask) }, { "team", num(r.team_byte) },
							{ "flags", entity_flags }, { "ammo", num(r.ammo_count) },
							{ "ref_num", num(r.bone_a) }, { "sub_type", num(r.bone_b) },
							{ "score_flag", num(r.score_flag) } });
		}
		out.emplace(uint32_t(batch.start_index) + uint32_t(i), std::move(record));
	}
}

// A pool spawn record's own liveness and its carrier (+0x28, the record's target).
struct PoolLink {
	bool live = false;
	uint16_t carrier = 0xFFFF;
};

// S2C 0x0D pool spawns. A record whose physics runs every tick is live
// (initial.S.0x0d.live_pose): a hull on water (entity+0x24 bit 0x8000), a vehicle
// the sleep gate keeps awake (bit 0x2000 or 0x40, or an occupant: the parent), and,
// folded in world_records, a record carried on a live one. One live by its own physics
// also leaves out its contact bit (0x2000) and its ground link (the target), which its
// mover re-probes every tick.
void pool_records(const std::string &raw, WorldRecords &out, std::map<uint32_t, PoolLink> &links) {
	static_assert(has_normalization("initial.S.0x0d.live_pose"));
	constexpr uint32_t kInAir = 0x2000;
	constexpr uint32_t kAwakeFlags = 0x8000 | kInAir | 0x40;
	// The euler presence bits follow the values (the serializer sets each for a
	// nonzero angle, NetPacket_SerializeEntityPoolToPacket_0 @0x503B3A..0x503B74): the
	// pose carries them; the target's bit rides the ground link.
	constexpr uint16_t kStateBits =
			kPoolSpawnHasEulerZ | kPoolSpawnHasEulerX | kPoolSpawnHasEulerY | kPoolSpawnHasTargetHandle;
	PoolSpawnBatch batch;
	decode_pool_spawn_batch(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), batch);
	const size_t complete = batch.records.size() - (batch.last_record_partial ? 1 : 0);
	for (size_t i = 0; i < complete; ++i) {
		const PoolSpawnRecord &r = batch.records[i];
		if (out.count(r.slot_id))
			continue; // the first sighting
		char flags[8], entity_flags[16];
		std::snprintf(flags, sizeof(flags), "%04x", unsigned(r.spawn_flags & ~kStateBits));
		std::snprintf(entity_flags, sizeof(entity_flags), "%08x", unsigned(r.entity_flags & ~kInAir));
		std::string mounts;
		for (const uint16_t handle : r.mount_handles)
			mounts += num(handle) + ",";
		mounts += num(r.mount_handle_8) + "," + num(r.mount_handle_9);
		WorldRecord record;
		record.fields = { { "spawn_flags", flags }, { "type", num(r.item_type_id) },
			{ "name", r.entity_name }, { "entity_flags", entity_flags },
			{ "section", num(r.section_mask) }, { "team", num(r.team_byte) },
			{ "parent", num(r.parent_handle) },
			{ "seat_mask", num(r.seat_mask) }, { "mounts", mounts }, { "bone", num(r.bone_byte) },
			{ "ai_profile_1", num(r.ai_profile_1) }, { "ai_profile_2", num(r.ai_profile_2) },
			{ "ai_name", r.ai_name }, { "ref_num", num(r.alert_byte) },
			{ "sub_type", num(r.action_byte) }, { "sound_latch", num(r.sound_latch_byte) },
			{ "zone_rank", num(r.zone_number_rank) }, { "zone_radius", num(r.zone_radius) },
			{ "difficulty", num(r.difficulty_byte) } };
		record.pose = { { "x", num(r.pos_x) }, { "y", num(r.pos_y) }, { "z", num(r.pos_z) },
			{ "euler_z", num(r.euler_z) }, { "euler_x", num(r.euler_x) },
			{ "euler_y", num(r.euler_y) } };
		record.state = { { "in_air", num((r.entity_flags & kInAir) ? 1 : 0) },
			{ "target", num((r.spawn_flags & kPoolSpawnHasTargetHandle) ? r.target_handle : -1) } };
		PoolLink link;
		link.live = ((r.spawn_flags & kPoolSpawnHasEntityFlags) && (r.entity_flags & kAwakeFlags)) ||
				(r.spawn_flags & kPoolSpawnHasParentHandle);
		record.self_live = link.live;
		if (r.spawn_flags & kPoolSpawnHasTargetHandle)
			link.carrier = r.target_handle;
		links.emplace(r.slot_id, link);
		out.emplace(r.slot_id, std::move(record));
	}
}

WorldRecords world_records(uint16_t identity, const std::vector<std::string> &raw) {
	WorldRecords out;
	std::map<uint32_t, PoolLink> links;
	for (const std::string &body : raw) {
		if (identity == 0x00C)
			organic_records(body, out);
		else if (identity == 0x00D)
			pool_records(body, out, links);
		else if (identity == 0x010)
			static_records(body, out);
	}
	// A record carried on a live one is live, its carrier often on an earlier page.
	for (bool changed = true; changed;) {
		changed = false;
		for (auto &entry : links) {
			const auto carrier = links.find(entry.second.carrier);
			if (!entry.second.live && carrier != links.end() && carrier->second.live) {
				entry.second.live = true;
				changed = true;
			}
		}
	}
	for (const auto &entry : links)
		out[entry.first].live = entry.second.live;
	return out;
}

// The first field two records differ in ("" when they match); every differing
// field's name lands in `names`.
std::string first_difference(const Fields &a, const Fields &b, std::set<std::string> &names) {
	std::string first;
	for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
		const char *name = i < a.size() ? a[i].first : b[i].first;
		const std::string x = i < a.size() ? a[i].second : "-";
		const std::string y = i < b.size() ? b[i].second : "-";
		if (x == y)
			continue;
		names.insert(name);
		if (first.empty())
			first = std::string(name) + " " + x + " vs " + y;
	}
	return first;
}

MetricValue compare_records(char dir, uint16_t identity, const std::vector<std::string> &raw_a,
		const std::vector<std::string> &raw_b) {
	const WorldRecords a = world_records(identity, raw_a), b = world_records(identity, raw_b);
	std::set<uint32_t> slots;
	for (const auto &entry : a)
		slots.insert(entry.first);
	for (const auto &entry : b)
		slots.insert(entry.first);
	size_t differing = 0;
	std::map<std::string, size_t> by_field; // slots differing in each field
	std::string examples;
	for (const uint32_t slot : slots) {
		const auto fa = a.find(slot), fb = b.find(slot);
		std::set<std::string> names;
		std::string what;
		if (fa == a.end() || fb == b.end()) {
			what = fa == a.end() ? "RR has none" : "the run has none";
			names.insert("slot");
		} else {
			what = first_difference(fa->second.fields, fb->second.fields, names);
			if (!fa->second.live) {
				const std::string pose = first_difference(fa->second.pose, fb->second.pose, names);
				what = what.empty() ? pose : what;
			}
			if (!fa->second.self_live) {
				const std::string state = first_difference(fa->second.state, fb->second.state, names);
				what = what.empty() ? state : what;
			}
		}
		if (what.empty())
			continue;
		for (const std::string &name : names)
			++by_field[name];
		if (++differing <= 3) {
			char head[24];
			std::snprintf(head, sizeof(head), "slot 0x%04x: ", unsigned(slot));
			examples += (examples.empty() ? "" : "; ") + std::string(head) + what;
		}
	}
	std::string fields;
	for (const auto &entry : by_field)
		fields += (fields.empty() ? "" : ", ") + entry.first + " " + std::to_string(entry.second);
	MetricValue value = make_metric(
			dir, "initial_records", identity_key(identity), "INITIAL_RECORD_MISMATCH");
	value.distance = double(differing);
	value.subject = std::to_string(b.size()) + " slots";
	value.reference = std::to_string(a.size()) + " slots";
	value.detail = examples.empty() ? "records differing from RR's, by slot"
									: "slots per field: " + fields + "; " + examples;
	return value;
}

} // namespace

void compare_initial(
		const RunFacts &rr, const RunFacts &other, char dir, std::vector<MetricValue> &out) {
	const std::vector<std::string> ga = initial_groups(rr, dir), gb = initial_groups(other, dir);
	const char *unit = group_boundary_tolerant(dir) ? " records" : " groups";
	MetricValue grouping = make_metric(dir, "initial_grouping", "", "INITIAL_PACKET_GROUPING");
	grouping.distance = double(edit_distance(ga, gb));
	grouping.subject = std::to_string(gb.size()) + unit;
	grouping.reference = std::to_string(ga.size()) + unit;
	for (size_t i = 0; i < std::max(ga.size(), gb.size()); ++i) {
		const std::string x = i < ga.size() ? ga[i] : "-", y = i < gb.size() ? gb[i] : "-";
		if (x == y)
			continue;
		grouping.detail = std::string("first difference at") + unit + " " + std::to_string(i + 1) +
				": RR " + x.substr(0, 120) + " vs " + y.substr(0, 120);
		break;
	}
	out.push_back(grouping);

	// The initial material bodies, but for the registered dynamic ones.
	const auto sa = initial_bodies(rr, dir), sb = initial_bodies(other, dir);
	for (const auto &entry : sa) {
		const uint16_t identity = entry.first;
		const auto found = sb.find(identity);
		if (found == sb.end() || initial_body_exempt(dir, identity))
			continue; // presence is the histogram's
		const std::vector<std::string> &x = entry.second, &y = found->second;
		const std::string key = identity_key(identity);
		if (initial_records(dir, identity)) {
			out.push_back(compare_records(dir, identity, x, y));
			continue;
		}
		if (initial_body_set(dir, identity)) {
			const std::set<std::string> ka = body_set(dir, identity, x),
										kb = body_set(dir, identity, y);
			MetricValue set = make_metric(dir, "initial_body_set", key, "INITIAL_BODY_MISMATCH");
			set.distance = jaccard_distance(ka, kb);
			set.subject = render_set(kb, 160);
			set.reference = render_set(ka, 160);
			set.detail = "the initial window's distinct bodies";
			out.push_back(set);
			continue;
		}
		std::vector<std::string> kx, ky;
		for (const std::string &body : x)
			kx.push_back(stable_body_key(dir, identity, body));
		for (const std::string &body : y)
			ky.push_back(stable_body_key(dir, identity, body));
		size_t differing = 0;
		for (size_t i = 0; i < std::min(kx.size(), ky.size()); ++i)
			differing += kx[i] != ky[i] ? 1 : 0;
		MetricValue body = make_metric(dir, "initial_body", key, "INITIAL_BODY_MISMATCH");
		body.distance = double(differing);
		body.subject = render_list(ky, 160);
		body.reference = render_list(kx, 160);
		body.detail = "bodies differing from RR's over the common prefix, position by position";
		out.push_back(body);
		MetricValue count = make_metric(dir, "initial_count", key, "INITIAL_COUNT_MISMATCH");
		count_metric(count, double(kx.size()), 0.0, double(ky.size()), 0.0);
		out.push_back(count);
	}
}

} // namespace opennova::parity
