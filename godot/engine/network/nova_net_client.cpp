#include "nova_net_client.h"

#include "object/nova_item_database.h"

#include <godot_cpp/classes/packet_peer_udp.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace godot;
using opennova::CaptureDatagram;
using opennova::EntityClass;
using opennova::InGameMessage;
using opennova::ReplayEntity;
using opennova::ReplaySample;
using opennova::ReplaySampleSource;

namespace {

constexpr uint8_t HELLO_MAGIC[4] = {'N', 'W', 'R', 'H'};
constexpr uint8_t ASSIGN_MAGIC[4] = {'N', 'W', 'R', 'A'};

PackedByteArray to_pba(const uint8_t *p, size_t n) {
	PackedByteArray out;
	out.resize(int(n));
	if (n) std::memcpy(out.ptrw(), p, n);
	return out;
}

std::vector<uint8_t> from_pba(const PackedByteArray &pba) {
	std::vector<uint8_t> out(pba.size());
	if (!out.empty()) std::memcpy(out.data(), pba.ptr(), out.size());
	return out;
}

inline double fp16(int32_t v) { return double(v) / 65536.0; }

} // namespace

NovaNetClient::NovaNetClient() = default;
NovaNetClient::~NovaNetClient() = default;

void NovaNetClient::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_replay_host", "host"), &NovaNetClient::set_replay_host);
	ClassDB::bind_method(D_METHOD("get_replay_host"), &NovaNetClient::get_replay_host);
	ClassDB::bind_method(D_METHOD("set_replay_port", "port"), &NovaNetClient::set_replay_port);
	ClassDB::bind_method(D_METHOD("get_replay_port"), &NovaNetClient::get_replay_port);
	ClassDB::bind_method(D_METHOD("set_item_database", "db"), &NovaNetClient::set_item_database);
	ClassDB::bind_method(D_METHOD("connect_to_replay"), &NovaNetClient::connect_to_replay);
	ClassDB::bind_method(D_METHOD("stop"), &NovaNetClient::stop);
	ClassDB::bind_method(D_METHOD("get_state"), &NovaNetClient::get_state);
	ClassDB::bind_method(D_METHOD("get_role_index"), &NovaNetClient::get_role_index);
	ClassDB::bind_method(D_METHOD("is_host_role"), &NovaNetClient::is_host_role);
	ClassDB::bind_method(D_METHOD("get_session_port"), &NovaNetClient::get_session_port);
	ClassDB::bind_method(D_METHOD("get_mission"), &NovaNetClient::get_mission);
	ClassDB::bind_method(D_METHOD("get_entity_count"), &NovaNetClient::get_entity_count);
	ClassDB::bind_method(D_METHOD("get_entities"), &NovaNetClient::get_entities);
	ClassDB::bind_method(D_METHOD("get_latest_frame"), &NovaNetClient::get_latest_frame);
	ClassDB::bind_method(D_METHOD("get_frame_range"), &NovaNetClient::get_frame_range);
	ClassDB::bind_method(D_METHOD("sample_at", "handle", "frame_f"), &NovaNetClient::sample_at);
	ClassDB::bind_method(D_METHOD("get_own_player_handle"), &NovaNetClient::get_own_player_handle);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "replay_host"), "set_replay_host", "get_replay_host");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "replay_port"), "set_replay_port", "get_replay_port");

	ADD_SIGNAL(MethodInfo("assigned", PropertyInfo(Variant::INT, "role_index"),
	                      PropertyInfo(Variant::BOOL, "is_host"),
	                      PropertyInfo(Variant::INT, "session_port"),
	                      PropertyInfo(Variant::STRING, "mission")));
	ADD_SIGNAL(MethodInfo("world_updated"));
	ADD_SIGNAL(MethodInfo("disconnected"));
	ADD_SIGNAL(MethodInfo("error_occurred", PropertyInfo(Variant::STRING, "message")));

	BIND_ENUM_CONSTANT(STATE_IDLE);
	BIND_ENUM_CONSTANT(STATE_CONNECTING);
	BIND_ENUM_CONSTANT(STATE_RECEIVING);
	BIND_ENUM_CONSTANT(STATE_DISCONNECTED);
	BIND_ENUM_CONSTANT(STATE_ERROR);
}

void NovaNetClient::set_replay_host(const String &host) { replay_host_ = host; }
String NovaNetClient::get_replay_host() const { return replay_host_; }
void NovaNetClient::set_replay_port(int port) { replay_port_ = port; }
int NovaNetClient::get_replay_port() const { return replay_port_; }

void NovaNetClient::set_item_database(const Ref<NovaItemDatabase> &db) {
	class_table_.clear();
	if (db.is_null()) return;
	// Snapshot wire type_id -> §5.10b class once, so the per-record class lookup in
	// the fold is a fast local map hit (no String marshaling per S2C 0x0A record).
	const PackedInt32Array ids = db->get_item_ids();
	for (int i = 0; i < ids.size(); ++i) {
		const int def_id = ids[i];
		const int wire = def_id - 100000;
		if (wire < 0 || wire >= 0x10000) continue;
		const String ai = db->get_ai_function(def_id);
		EntityClass cls = opennova::class_from_tag(ai.utf8().get_data());
		if (cls == EntityClass::Unknown) {
			const String mv = db->get_move_function(def_id);
			cls = opennova::class_from_tag(mv.utf8().get_data());
		}
		if (cls != EntityClass::Unknown) class_table_[uint16_t(wire)] = cls;
	}
}

EntityClass NovaNetClient::class_of(uint16_t wire_type) const {
	const auto it = class_table_.find(wire_type);
	return it == class_table_.end() ? EntityClass::Unknown : it->second;
}

void NovaNetClient::connect_to_replay() {
	stop();
	socket_.instantiate();
	if (socket_->bind(0, "0.0.0.0") != OK) {
		enter_state(STATE_ERROR, "UDP bind failed");
		return;
	}
	local_port_ = int(socket_->get_local_port());
	socket_->set_dest_address(replay_host_, replay_port_);
	connect_elapsed_ = 0.0;
	since_hello_ = hello_resend_s_; // send immediately on first _process
	enter_state(STATE_CONNECTING);
	send_hello();
}

void NovaNetClient::stop() {
	if (socket_.is_valid()) socket_->close();
	socket_.unref();
	role_index_ = -1;
	is_host_ = false;
	session_port_ = 0;
	mission_ = String();
	decoder_ = opennova::CaptureDecoder();
	messages_.clear();
	world_ = opennova::ReplayTimeline();
	// NB: class_table_ is configuration (set via set_item_database), not
	// per-connection state — do NOT clear it here, or a set_item_database()
	// before connect_to_replay() (which calls stop()) would be wiped, leaving
	// S2C 0x0A motion unwalkable.
	recv_counter_ = 0;
	own_player_handle_ = 0xFFFF;
	dirty_ = false;
	if (state_ != STATE_IDLE) enter_state(STATE_IDLE);
}

void NovaNetClient::send_hello() {
	if (!socket_.is_valid()) return;
	socket_->set_dest_address(replay_host_, replay_port_);
	socket_->put_packet(to_pba(HELLO_MAGIC, 4));
}

bool NovaNetClient::parse_assign(const uint8_t *p, int n) {
	if (n < 9 || std::memcmp(p, ASSIGN_MAGIC, 4) != 0) return false;
	role_index_ = int(p[4]);
	is_host_ = p[5] != 0;
	session_port_ = int(p[6]) | (int(p[7]) << 8);
	const int mlen = int(p[8]);
	mission_ = String();
	if (n >= 9 + mlen && mlen > 0) {
		PackedByteArray mb;
		mb.resize(mlen);
		std::memcpy(mb.ptrw(), p + 9, mlen);
		mission_ = mb.get_string_from_utf8();
	}
	return true;
}

void NovaNetClient::drain_socket() {
	if (!socket_.is_valid()) return;
	while (socket_->get_available_packet_count() > 0) {
		const PackedByteArray pkt = socket_->get_packet();
		if (pkt.size() <= 0) continue;
		const std::vector<uint8_t> bytes = from_pba(pkt);

		if (state_ == STATE_CONNECTING) {
			if (parse_assign(bytes.data(), int(bytes.size()))) {
				enter_state(STATE_RECEIVING);
				emit_signal("assigned", role_index_, is_host_, session_port_, mission_);
			}
			continue; // ignore non-ASSIGN before assignment
		}
		if (state_ != STATE_RECEIVING) continue;
		if (parse_assign(bytes.data(), int(bytes.size()))) continue; // stray re-assign

		// A raw captured datagram — decode it exactly as a real client would.
		CaptureDatagram d;
		d.frame_index = ++recv_counter_;
		d.src_port = int(socket_->get_packet_port()); // nw_replay's port (constant)
		d.dst_port = local_port_;
		d.payload = bytes;
		for (auto &m : decoder_.push(d)) {
			if (m.settings_update) continue;
			messages_.push_back(std::move(m));
			dirty_ = true;
		}
	}
}

void NovaNetClient::rebuild_world() {
	world_ = opennova::build_replay_timeline(
	    messages_, [this](uint16_t t) { return class_of(t); });
	// Own player = the entity with a clean C2S 0x0C uplink sample (a client's own
	// player). The host has none (spectator), so this stays 0xFFFF for it.
	own_player_handle_ = 0xFFFF;
	for (const auto &e : world_.entities) {
		for (const auto &s : e.track) {
			if (s.source == ReplaySampleSource::ClientUplink) {
				own_player_handle_ = e.handle;
				break;
			}
		}
		if (own_player_handle_ != 0xFFFF) break;
	}
}

void NovaNetClient::_process(double delta) {
	if (state_ == STATE_CONNECTING) {
		drain_socket();
		if (state_ != STATE_CONNECTING) return; // assigned this frame
		connect_elapsed_ += delta;
		since_hello_ += delta;
		if (since_hello_ >= hello_resend_s_) {
			since_hello_ = 0.0;
			send_hello();
		}
		if (connect_elapsed_ >= connect_timeout_s_)
			enter_state(STATE_ERROR, "no ASSIGN from nw_replay (is it running?)");
		return;
	}
	if (state_ != STATE_RECEIVING) return;

	drain_socket();
	since_rebuild_ += delta;
	if (dirty_ && since_rebuild_ >= rebuild_interval_s_) {
		since_rebuild_ = 0.0;
		dirty_ = false;
		rebuild_world();
		emit_signal("world_updated");
	}
}

void NovaNetClient::enter_state(State next, const String &reason) {
	state_ = next;
	if (next == STATE_ERROR) emit_signal("error_occurred", reason);
	else if (next == STATE_DISCONNECTED) emit_signal("disconnected");
}

// --- world-model query -------------------------------------------------------

int NovaNetClient::get_entity_count() const { return int(world_.entities.size()); }

Array NovaNetClient::get_entities() const {
	Array out;
	for (const auto &e : world_.entities) {
		Dictionary d;
		d["handle"] = int(e.handle);
		d["pool"] = int(e.pool);
		d["type_id"] = int(e.type_id);
		d["name"] = String(e.name.c_str());
		d["team"] = e.team;
		d["team_known"] = e.team_known;
		d["owner_session"] = e.owner_session;
		d["spawn_tag"] = int((unsigned char)e.spawn_tag);
		d["is_own_player"] = (e.handle == own_player_handle_);
		out.push_back(d);
	}
	return out;
}

int NovaNetClient::get_latest_frame() const { return world_.last_frame; }

Vector2i NovaNetClient::get_frame_range() const {
	return Vector2i(world_.first_frame, world_.last_frame);
}

int NovaNetClient::get_own_player_handle() const { return int(own_player_handle_); }

Dictionary NovaNetClient::sample_at(int handle, double frame_f) const {
	Dictionary out;
	out["found"] = false;
	const ReplayEntity *e = nullptr;
	for (const auto &ent : world_.entities)
		if (ent.handle == uint16_t(handle)) { e = &ent; break; }
	if (!e || e->track.empty()) return out;

	const std::vector<ReplaySample> &tr = e->track;
	// Locate the sample bracketing frame_f (track is ascending by frame_index).
	size_t i = 0;
	while (i + 1 < tr.size() && double(tr[i + 1].frame_index) <= frame_f) ++i;

	const ReplaySample *a = &tr[i];
	const ReplaySample *b = (i + 1 < tr.size()) ? &tr[i + 1] : nullptr;

	double x = fp16(a->x), y = fp16(a->y), z = fp16(a->z);
	double heading = a->heading_deg;
	bool has_heading = a->has_heading;

	// Interpolate toward b only when continuous (no respawn/mount/dead boundary).
	if (b && frame_f > double(a->frame_index) && !a->dead && !b->respawn &&
	    a->mounted == b->mounted && b->frame_index != a->frame_index) {
		double t = (frame_f - double(a->frame_index)) /
		           double(b->frame_index - a->frame_index);
		if (t < 0.0) t = 0.0;
		if (t > 1.0) t = 1.0;
		x = fp16(a->x) + (fp16(b->x) - fp16(a->x)) * t;
		y = fp16(a->y) + (fp16(b->y) - fp16(a->y)) * t;
		z = fp16(a->z) + (fp16(b->z) - fp16(a->z)) * t;
		if (a->has_heading && b->has_heading) {
			double d = b->heading_deg - a->heading_deg;
			while (d > 180.0) d -= 360.0;
			while (d < -180.0) d += 360.0;
			heading = a->heading_deg + d * t;
		}
	}

	out["found"] = true;
	out["pos"] = Vector3(real_t(x), real_t(y), real_t(z)); // wire/mission meters
	out["heading_deg"] = heading;
	out["has_heading"] = has_heading;
	out["alive"] = !a->dead;
	out["dead"] = a->dead;
	out["mounted"] = a->mounted;
	return out;
}
