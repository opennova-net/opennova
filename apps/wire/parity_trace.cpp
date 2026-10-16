#include "parity_trace.h"

#include <base/gameprofile/game_type.h>
#include <base/io/strutil.h>
#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/title/title_identity.h>

#include <functional>
#include <utility>

namespace opennova::wire {

namespace {

using opennova::strutil::bytes_to_hex;

// Keep the parity stream deliberately simpler than the human pretty printer: one
// packet line preserves grouping/replay evidence, one event line preserves
// semantic order, and state/entity lines expose decoded 0x0C/0x0A gameplay
// fields. Every other event is a byte-for-byte witness. The two high-volume
// gameplay bodies retain their exact length without turning an hour-long capture
// into a giant text intermediary.
constexpr bool parity_body_is_material(char dir, uint16_t full_tag) {
	// Compare the full 9-bit dispatch identity: high-table/control messages may
	// share these low bytes but are not gameplay frames.
	return !((dir == 'C' && full_tag == c2s::ENTITY_UPLINK) ||
			(dir == 'S' && full_tag == s2c::PER_FRAME_UPDATE));
}

constexpr bool parity_body_contract_holds() {
	for (uint16_t full_tag = 0; full_tag <= 0x1FF; ++full_tag) {
		if (parity_body_is_material('C', full_tag) != (full_tag != 0x00C))
			return false;
		if (parity_body_is_material('S', full_tag) != (full_tag != 0x00A))
			return false;
	}
	return true;
}

// The parity stream is the byte witness for every message other than the two
// high-rate gameplay records, whose dedicated structured lines carry the
// comparison contract. Keep representative initial-stream tags pinned here so
// adding a new semantic comparison cannot silently hide their payload bytes.
static_assert(parity_body_contract_holds());
static_assert(parity_body_is_material('S', 0x75));
static_assert(parity_body_is_material('S', 0x60));
static_assert(parity_body_is_material('S', 0x2A));
static_assert(parity_body_is_material('C', 0x47));
static_assert(parity_body_is_material('C', 0x0A));
static_assert(parity_body_is_material('S', 0x10A));
static_assert(parity_body_is_material('C', 0x10C));
static_assert(!parity_body_is_material('C', 0x0C));
static_assert(!parity_body_is_material('S', 0x0A));

ParityEntity entity_record(const InGameMessage &message, uint64_t ts_ns, const FrameUpdate &frame,
		const FrameUpdateRecord &record) {
	ParityEntity out;
	out.frame = message.frame_index;
	out.ts_ns = ts_ns;
	out.session = message.session;
	out.handle = record.handle;
	out.type_id = record.type_id;
	uint16_t carrier = 0xFFFF;
	switch (record.cls) {
		case EntityClass::Player:
			out.cls = 'P';
			carrier = record.player.carrier_handle;
			out.pos_x = record.player.pos_x_compressed;
			out.pos_y = record.player.pos_y_compressed;
			out.pos_z = record.player.pos_z_compressed;
			out.orient_a = record.player.yaw_byte;
			out.orient_b = record.player.pitch_byte;
			out.state_a = record.player.state_flags;
			out.state_b = (uint32_t(record.player.anim_state_id) << 8) |
					uint32_t(record.player.anim_def_index);
			out.vital = record.player.health_class_byte;
			break;
		case EntityClass::Vehicle:
			out.cls = 'V';
			carrier = record.vehicle.parent_slot_handle;
			out.pos_x = record.vehicle.pos_x_compressed;
			out.pos_y = record.vehicle.pos_y_compressed;
			out.pos_z = record.vehicle.pos_z_compressed;
			out.orient_a = record.vehicle.euler_z;
			out.orient_b = record.vehicle.is_dead_pose ? record.vehicle.euler_y
													   : record.vehicle.weapon_heading_bam;
			out.state_a = record.vehicle.flags_byte;
			out.state_b = record.vehicle.is_dead_pose ? 1u : 0u;
			out.vital = record.vehicle.health_word;
			break;
		case EntityClass::Infantry:
			out.cls = 'I';
			carrier = record.infantry.vehicle_slot_handle;
			out.pos_x = record.infantry.pos_x_compressed;
			out.pos_y = record.infantry.pos_y_compressed;
			out.pos_z = record.infantry.pos_z_compressed;
			out.orient_a = record.infantry.yaw_byte;
			out.orient_b = record.infantry.pitch_byte;
			out.state_a = record.infantry.flags_byte;
			out.state_b = record.infantry.anim_byte;
			break;
		case EntityClass::NoNetworkCallback:
			out.cls = 'N';
			return out;
		default:
			return out;
	}
	// A compact position is sign/exponent/mantissa relative to the frame anchor, or
	// carrier-local when the record names a carrier (ingame_decode.h).
	out.offset_x = network_decompress_fixedpoint(uint16_t(out.pos_x));
	out.offset_y = network_decompress_fixedpoint(uint16_t(out.pos_y));
	out.offset_z = network_decompress_fixedpoint(uint16_t(out.pos_z));
	out.carrier_local = carrier != 0xFFFF;
	out.world_x = out.carrier_local ? out.offset_x : frame.anchor_x + out.offset_x;
	out.world_y = out.carrier_local ? out.offset_y : frame.anchor_y + out.offset_y;
	out.world_z = out.carrier_local ? out.offset_z : frame.anchor_z + out.offset_z;
	return out;
}

} // namespace

const char *parity_datagram_class_name(CaptureDatagramClass datagram_class) {
	switch (datagram_class) {
		case CaptureDatagramClass::Invalid:
			return "invalid";
		case CaptureDatagramClass::Unknown:
			return "unknown";
		case CaptureDatagramClass::ClientHello:
			return "client_hello";
		case CaptureDatagramClass::ClientAuth:
			return "client_auth";
		case CaptureDatagramClass::ServerHello:
			return "server_hello";
		case CaptureDatagramClass::ServerAuth:
			return "server_auth";
		case CaptureDatagramClass::ClientProtocol:
			return "client_protocol";
		case CaptureDatagramClass::ServerProtocol:
			return "server_protocol";
	}
	return "invalid";
}

// ---------------------------------------------------------------------------
// Builder

ParityTraceBuilder::ParityTraceBuilder(ParityTraceSink &sink, const ItemClassMap *item_classes) :
		sink_(sink), item_classes_(item_classes) {}

void ParityTraceBuilder::push(const net::PcapDatagram &pk) {
	frame_ts_[pk.frame_index] = pk.ts_nanos;
	emit_handshake(pk.frame_index, pk.ts_nanos, pk.srcport, pk.dstport, pk.payload);
	CaptureDatagram cd;
	cd.frame_index = pk.frame_index;
	cd.src_port = pk.srcport;
	cd.dst_port = pk.dstport;
	cd.payload = pk.payload;
	emit_decoded(decoder_.push_detailed(cd), frame_ts_);
}

void ParityTraceBuilder::push_decoded(
		const CaptureDecodeResult &decoded, const std::map<int, uint64_t> &frame_timestamps) {
	emit_decoded(decoded, frame_timestamps);
}

void ParityTraceBuilder::emit_decoded(
		const CaptureDecodeResult &decoded, const std::map<int, uint64_t> &frame_timestamps) {
	auto ts_of = [&frame_timestamps](int frame) -> uint64_t {
		const auto found = frame_timestamps.find(frame);
		return found == frame_timestamps.end() ? 0 : found->second;
	};
	for (const CapturedDatagramResult &datagram : decoded.datagrams) {
		ParityDatagram out;
		out.frame = datagram.frame_index;
		out.ts_ns = ts_of(datagram.frame_index);
		out.src_port = datagram.src_port;
		out.dst_port = datagram.dst_port;
		out.length = datagram.payload_length;
		out.outer_decoded = datagram.outer_decoded;
		out.opcode = datagram.opcode;
		out.datagram_class = datagram.datagram_class;
		out.decoded = datagram.decoded;
		sink_.on_datagram(out);
	}
	for (const CapturedSessionPacket &packet : decoded.session_packets) {
		ParityPacket out;
		out.frame = packet.frame_index;
		out.ts_ns = ts_of(packet.frame_index);
		out.dir = packet.dir;
		out.session = packet.session;
		out.sid = packet.header.session_id;
		out.seq = packet.header.seq_num;
		out.ack = packet.header.ack_count;
		out.flags = packet.header.connection_flags;
		out.tags = packet.tags;
		out.records.reserve(packet.records.size());
		for (const CapturedProtocolRecord &record : packet.records)
			out.records.push_back({ record.full_tag, record.raw_flags, record.encoded_length,
					record.skip_bytes });
		sink_.on_packet(out);
	}
	for (const InGameMessage &message : decoded.messages)
		emit_message(message, ts_of(message.frame_index));
}

const TitleIdentity *handshake_title(const std::vector<uint8_t> &raw) {
	std::vector<uint8_t> stripped(raw.size());
	size_t stripped_len = 0;
	if (napi_envelope_decode(
				raw.data(), raw.size(), stripped.data(), stripped.size(), &stripped_len) != 0 ||
			stripped_len == 0)
		return nullptr;
	stripped.resize(stripped_len);
	const uint8_t opcode = stripped[0];
	std::vector<uint8_t> body(stripped.begin() + 1, stripped.end());
	if (!body.empty()) nwu_encrypt(body.data(), body.size(), SESSION_NWU_KEY);
	std::string pn;
	if (opcode == SESSION_OPCODE_CLIENT_HELLO) {
		ClientHello value;
		if (parse_client_hello(body.data(), body.size(), value)) pn = value.pn;
	} else if (opcode == SESSION_OPCODE_CLIENT_AUTH) {
		ClientAuth value;
		if (parse_client_auth(body.data(), body.size(), value)) pn = value.pn;
	} else if (opcode == SESSION_OPCODE_SERVER_HELLO) {
		ServerHello value;
		if (parse_server_hello(body.data(), body.size(), value)) pn = value.pn;
	}
	return pn.empty() ? nullptr : title_identity_by_protocol_name(pn);
}

void ParityTraceBuilder::emit_handshake(
		int frame, uint64_t ts_ns, int src_port, int dst_port, const std::vector<uint8_t> &raw) {
	if (const TitleIdentity *title = handshake_title(raw)) title_ = title;
	std::vector<uint8_t> stripped(raw.size());
	size_t stripped_len = 0;
	if (napi_envelope_decode(
				raw.data(), raw.size(), stripped.data(), stripped.size(), &stripped_len) != 0)
		return;
	stripped.resize(stripped_len);
	if (stripped.empty())
		return;
	const uint8_t opcode = stripped[0];
	const bool client =
			opcode == SESSION_OPCODE_CLIENT_HELLO || opcode == SESSION_OPCODE_CLIENT_AUTH;
	const bool server =
			opcode == SESSION_OPCODE_SERVER_HELLO || opcode == SESSION_OPCODE_SERVER_AUTH;
	if (!client && !server)
		return;
	ParityHandshake out;
	out.frame = frame;
	out.ts_ns = ts_ns;
	out.dir = client ? 'C' : 'S';
	out.session = client ? src_port : dst_port;
	out.opcode = opcode;
	out.body.assign(stripped.begin() + 1, stripped.end());
	if (!out.body.empty())
		nwu_encrypt(out.body.data(), out.body.size(), SESSION_NWU_KEY);
	switch (opcode) {
		case SESSION_OPCODE_CLIENT_HELLO: {
			ClientHello value;
			out.decoded = parse_client_hello(out.body.data(), out.body.size(), value);
			break;
		}
		case SESSION_OPCODE_CLIENT_AUTH: {
			ClientAuth value;
			out.decoded = parse_client_auth(out.body.data(), out.body.size(), value);
			break;
		}
		case SESSION_OPCODE_SERVER_HELLO: {
			ServerHello value;
			out.decoded = parse_server_hello(out.body.data(), out.body.size(), value);
			break;
		}
		case SESSION_OPCODE_SERVER_AUTH: {
			// Every 0x82 is decoded, as the capture decoder marks it: the client
			// reads every one, and one whose CUs overrun the walk's bounds, which
			// parse_server_auth refuses, is dropped at that walk, not unread.
			out.decoded = true;
			break;
		}
	}
	sink_.on_handshake(out);
}

void ParityTraceBuilder::emit_message(const InGameMessage &message, uint64_t ts_ns) {
	ParityEvent event;
	event.frame = message.frame_index;
	event.ts_ns = ts_ns;
	event.dir = message.dir;
	event.session = message.session;
	event.participant = message.participant;
	event.full_tag = message.tag;
	event.settings_update = message.settings_update;
	event.length = message.payload.size();
	event.material = parity_body_is_material(message.dir, message.tag);
	if (event.material)
		event.body = message.payload;
	sink_.on_event(event);

	// Settings-update/high-table records share the low dispatch byte but are not
	// ordinary gameplay 0x0A/0x0C messages and must never satisfy parity state.
	if (message.settings_update)
		return;
	const uint8_t tag = static_cast<uint8_t>(message.tag & 0xFFu);
	if (message.dir == 'S' && tag == s2c::FULL_PLAYER_INFO) {
		FullPlayerInfo info;
		if (decode_full_player_info(message.payload.data(), message.payload.size(), info))
			game_type_ = info.extra;
		return;
	}

	if (message.dir == 'C' && tag == c2s::ENTITY_UPLINK) {
		EntityPacketSubHeader header;
		PlayerExtendedUplink uplink;
		size_t header_bytes = 0, body_bytes = 0;
		const bool have_header = decode_entity_packet_sub_header(
				message.payload.data(), message.payload.size(), header, header_bytes);
		bool clean = false;
		if (have_header && header.sub_op == ENTITY_SUB_OP_EXTENDED) {
			const uint8_t *body = message.payload.data() + header_bytes;
			const size_t body_len = message.payload.size() - header_bytes;
			clean = decode_player_extended_uplink(body, body_len, uplink, body_bytes) &&
					body_bytes == body_len;
		}
		ParityUplinkState out;
		out.frame = message.frame_index;
		out.ts_ns = ts_ns;
		out.session = message.session;
		out.decoded = clean;
		out.length = message.payload.size();
		out.handle = header.handle;
		out.type_id = header.item_type_id;
		out.sub_op = header.sub_op;
		out.carrier = uplink.carrier_handle;
		out.pos_x = uplink.pos_x;
		out.pos_y = uplink.pos_y;
		out.pos_z = uplink.pos_z;
		out.heading = uplink.heading;
		out.pitch = uplink.pitch;
		out.move = uplink.move_input_byte;
		out.state_flags = uplink.state_flags_byte;
		out.analog_x = uplink.analog_x;
		out.analog_y = uplink.analog_y;
		out.analog_z = uplink.analog_z;
		out.adm = uplink.equipped_adm_index;
		const uint16_t handles[4] = { uplink.priority_handle_0, uplink.priority_handle_1,
			uplink.priority_handle_2, uplink.priority_handle_3 };
		const uint16_t scores[4] = { uplink.priority_score_0, uplink.priority_score_1,
			uplink.priority_score_2, uplink.priority_score_3 };
		for (int slot = 0; slot < 4; ++slot) {
			out.priority_handle[slot] = handles[slot];
			out.priority_score[slot] = scores[slot];
			if (handles[slot] || scores[slot])
				++out.priority_nonzero;
		}
		sink_.on_uplink(out);
		return;
	}

	if (message.dir != 'S' || tag != s2c::PER_FRAME_UPDATE)
		return;
	const ItemClassMap *classes = item_classes_;
	auto class_of = [classes](uint16_t type) -> EntityClass {
		if (classes) {
			const auto found = classes->find(type);
			if (found != classes->end())
				return found->second;
		}
		if (type == kPlayerPersonTypeId)
			return EntityClass::Player;
		return EntityClass::Unknown;
	};
	FrameUpdate frame;
	const bool clean = decode_frame_update(message.payload.data(), message.payload.size(), class_of,
			frame, frame_objective_body(game_type::is_objective(game_type_),
					title_in_match(title_).objective_checkpoint));
	ParityFrameState out;
	out.frame = message.frame_index;
	out.ts_ns = ts_ns;
	out.session = message.session;
	out.decoded = clean;
	out.length = message.payload.size();
	out.sub_block = frame.sub_block;
	out.flags1 = frame.flags1;
	out.flags2 = frame.flags2;
	out.anchor_x = frame.anchor_x;
	out.anchor_y = frame.anchor_y;
	out.anchor_z = frame.anchor_z;
	out.local_tail = frame.local_tail_present;
	out.state_flag_byte = frame.state_flag_byte;
	out.health = frame.health;
	out.armor = frame.armor;
	out.records = frame.records.size();
	for (const FrameUpdateRecord &record : frame.records) {
		switch (record.cls) {
			case EntityClass::Player:
				++out.players;
				break;
			case EntityClass::Vehicle:
				++out.vehicles;
				break;
			case EntityClass::Infantry:
				++out.infantry;
				break;
			case EntityClass::NoNetworkCallback:
				++out.none;
				break;
			default:
				break;
		}
	}
	out.rounds = frame.round_events.size();
	out.carried = frame.carried_handle;
	out.weapon_present = frame.weapon.present;
	out.weapon[0] = frame.weapon.preround_timer;
	out.weapon[1] = frame.weapon.slot_state360;
	out.weapon[2] = frame.weapon.slot_state368;
	out.weapon[3] = frame.weapon.slot_state364;
	out.weapon[4] = frame.weapon.slot_state356;
	out.weapon[5] = frame.weapon.slot_state460;
	out.weapon[6] = frame.weapon.reload_seconds;
	out.weapon_uniform_team_mask = frame.weapon.uniform_team_mask;
	out.timer_present = frame.timer.present;
	out.timer_state[0] = frame.timer.state0;
	out.timer_state[1] = frame.timer.state1;
	out.timer_state[2] = frame.timer.state2;
	out.timer_state[3] = frame.timer.state3;
	out.timer_seconds = frame.timer.timer_seconds;
	out.env_present = frame.env.present;
	out.env[0] = frame.env.fog_dist;
	out.env[1] = frame.env.fog_accel;
	out.env[2] = frame.env.tod_fixed;
	out.env[3] = frame.env.quake_ticks;
	out.env[4] = frame.env.cloud_scroll;
	out.env[5] = frame.env.rain_pct;
	out.env[6] = frame.env.overcast;
	out.env[7] = frame.env.env_param;
	out.objective_present = frame.objective.present;
	for (int i = 0; i < 4; ++i)
		out.objective[i] = frame.objective.state[i];
	out.mount_present = frame.passenger.present;
	out.mount_handle = frame.passenger.mount_handle;
	out.mount_bound = frame.passenger.has_mount;
	out.mount_clip = frame.passenger.clip;
	out.mount_reserve = frame.passenger.reserve;
	sink_.on_frame(out);
	for (const FrameUpdateRecord &record : frame.records)
		sink_.on_entity(entity_record(message, ts_ns, frame, record));
}

// ---------------------------------------------------------------------------
// Printer: the opennova-wire --parity-events lines

void ParityTracePrinter::on_handshake(const ParityHandshake &record) {
	const std::string body_hex = bytes_to_hex(record.body);
	std::fprintf(out_,
			"PARITY_HANDSHAKE frame=%d ts_ns=%llu dir=%c session=%d "
			"opcode=0x%02x decode=%u len=%zu body=%s\n",
			record.frame, static_cast<unsigned long long>(record.ts_ns), record.dir, record.session,
			unsigned(record.opcode), record.decoded ? 1u : 0u, record.body.size(),
			body_hex.c_str());
}

void ParityTracePrinter::on_datagram(const ParityDatagram &record) {
	std::fprintf(out_,
			"PARITY_DATAGRAM frame=%d ts_ns=%llu src=%d dst=%d len=%zu outer=%u "
			"opcode=",
			record.frame, static_cast<unsigned long long>(record.ts_ns), record.src_port,
			record.dst_port, record.length, record.outer_decoded ? 1u : 0u);
	if (record.outer_decoded)
		std::fprintf(out_, "0x%02x", static_cast<unsigned>(record.opcode));
	else
		std::fprintf(out_, "--");
	std::fprintf(out_, " class=%s decode=%u\n", parity_datagram_class_name(record.datagram_class),
			record.decoded ? 1u : 0u);
}

void ParityTracePrinter::on_packet(const ParityPacket &record) {
	std::fprintf(out_,
			"PARITY_PACKET frame=%d ts_ns=%llu dir=%c session=%d sid=0x%08x "
			"seq=%u ack=%u flags=0x%02x records=%zu tags=",
			record.frame, static_cast<unsigned long long>(record.ts_ns), record.dir, record.session,
			static_cast<unsigned>(record.sid), static_cast<unsigned>(record.seq),
			static_cast<unsigned>(record.ack), static_cast<unsigned>(record.flags),
			record.records.size());
	if (record.tags.empty()) {
		std::fprintf(out_, "-");
	} else {
		for (size_t i = 0; i < record.tags.size(); ++i) {
			if (i != 0)
				std::fprintf(out_, ",");
			std::fprintf(out_, "0x%03x", static_cast<unsigned>(record.tags[i]));
		}
	}
	std::fprintf(out_, " wire=");
	if (record.records.empty()) {
		std::fprintf(out_, "-");
	} else {
		for (size_t i = 0; i < record.records.size(); ++i) {
			if (i != 0)
				std::fprintf(out_, ",");
			const ParityWireRecord &wire = record.records[i];
			const std::string skip =
					wire.skip_bytes.empty() ? std::string("-") : bytes_to_hex(wire.skip_bytes);
			std::fprintf(out_, "0x%03x:0x%02x:%u:%s", static_cast<unsigned>(wire.full_tag),
					static_cast<unsigned>(wire.raw_flags),
					static_cast<unsigned>(wire.encoded_length), skip.c_str());
		}
	}
	std::fprintf(out_, "\n");
}

void ParityTracePrinter::on_event(const ParityEvent &record) {
	const uint8_t tag = static_cast<uint8_t>(record.full_tag & 0xFFu);
	const std::string body = record.material ? bytes_to_hex(record.body) : std::string("-");
	std::fprintf(out_,
			"PARITY_EVENT frame=%d ts_ns=%llu dir=%c session=%d participant=%d tag=0x%02x "
			"settings=%u len=%zu body=%s\n",
			record.frame, static_cast<unsigned long long>(record.ts_ns), record.dir, record.session,
			record.participant, static_cast<unsigned>(tag), record.settings_update ? 1u : 0u,
			record.length, body.c_str());
}

void ParityTracePrinter::on_uplink(const ParityUplinkState &record) {
	std::fprintf(out_,
			"PARITY_STATE frame=%d ts_ns=%llu dir=C session=%d tag=0x0c "
			"kind=uplink decode=%u len=%zu handle=%u type=%u sub=%u carrier=%u "
			"pos=%d,%d,%d orient=%d,%d move=%u state=%u analog=%u,%u,%u "
			"adm=%u priority_nonzero=%u priority=%u:%u,%u:%u,%u:%u,%u:%u\n",
			record.frame, static_cast<unsigned long long>(record.ts_ns), record.session,
			record.decoded ? 1u : 0u, record.length, unsigned(record.handle),
			unsigned(record.type_id), unsigned(record.sub_op), unsigned(record.carrier),
			int(record.pos_x), int(record.pos_y), int(record.pos_z), int(record.heading),
			int(record.pitch), unsigned(record.move), unsigned(record.state_flags),
			unsigned(record.analog_x), unsigned(record.analog_y), unsigned(record.analog_z),
			unsigned(record.adm), unsigned(record.priority_nonzero),
			unsigned(record.priority_handle[0]), unsigned(record.priority_score[0]),
			unsigned(record.priority_handle[1]), unsigned(record.priority_score[1]),
			unsigned(record.priority_handle[2]), unsigned(record.priority_score[2]),
			unsigned(record.priority_handle[3]), unsigned(record.priority_score[3]));
}

void ParityTracePrinter::on_frame(const ParityFrameState &record) {
	std::fprintf(out_,
			"PARITY_STATE frame=%d ts_ns=%llu dir=S session=%d tag=0x0a "
			"kind=frame decode=%u len=%zu sub=%u flags=%u,%u anchor=%d,%d,%d "
			"local=%u local_state=%u,%d,%d records=%zu players=%u vehicles=%u "
			"infantry=%u none=%u rounds=%zu carried=%u "
			"weapon=%u,%u,%u,%u,%u,%u,%u,%u,%d "
			"timer=%u,%u,%u,%u,%u,%d "
			"env=%u,%u,%u,%u,%u,%u,%u,%u,%u "
			"objective=%u,%d,%d,%d,%d mount=%u,%u,%u,%u,%u\n",
			record.frame, static_cast<unsigned long long>(record.ts_ns), record.session,
			record.decoded ? 1u : 0u, record.length, unsigned(record.sub_block),
			unsigned(record.flags1), unsigned(record.flags2), int(record.anchor_x),
			int(record.anchor_y), int(record.anchor_z), record.local_tail ? 1u : 0u,
			unsigned(record.state_flag_byte), int(record.health), int(record.armor),
			record.records, unsigned(record.players), unsigned(record.vehicles),
			unsigned(record.infantry), unsigned(record.none), record.rounds,
			unsigned(record.carried), record.weapon_present ? 1u : 0u, unsigned(record.weapon[0]),
			unsigned(record.weapon[1]), unsigned(record.weapon[2]), unsigned(record.weapon[3]),
			unsigned(record.weapon[4]), unsigned(record.weapon[5]), unsigned(record.weapon[6]),
			int(record.weapon_uniform_team_mask), record.timer_present ? 1u : 0u,
			unsigned(record.timer_state[0]), unsigned(record.timer_state[1]),
			unsigned(record.timer_state[2]), unsigned(record.timer_state[3]),
			int(record.timer_seconds), record.env_present ? 1u : 0u, unsigned(record.env[0]),
			unsigned(record.env[1]), unsigned(record.env[2]), unsigned(record.env[3]),
			unsigned(record.env[4]), unsigned(record.env[5]), unsigned(record.env[6]),
			unsigned(record.env[7]), record.objective_present ? 1u : 0u, int(record.objective[0]),
			int(record.objective[1]), int(record.objective[2]), int(record.objective[3]),
			record.mount_present ? 1u : 0u, unsigned(record.mount_handle),
			record.mount_bound ? 1u : 0u, unsigned(record.mount_clip),
			unsigned(record.mount_reserve));
}

void ParityTracePrinter::on_entity(const ParityEntity &record) {
	std::fprintf(out_,
			"PARITY_ENTITY frame=%d ts_ns=%llu session=%d class=%c handle=%u "
			"type=%u pos=%u,%u,%u orient=%d,%d state=%u,%u vital=%u\n",
			record.frame, static_cast<unsigned long long>(record.ts_ns), record.session, record.cls,
			unsigned(record.handle), unsigned(record.type_id), unsigned(record.pos_x),
			unsigned(record.pos_y), unsigned(record.pos_z), int(record.orient_a),
			int(record.orient_b), unsigned(record.state_a), unsigned(record.state_b),
			unsigned(record.vital));
}

// ---------------------------------------------------------------------------
// Collector

void ParityTraceCollector::on_handshake(const ParityHandshake &record) {
	trace_.handshakes.push_back(record);
}
void ParityTraceCollector::on_datagram(const ParityDatagram &record) {
	trace_.datagrams.push_back(record);
}
void ParityTraceCollector::on_packet(const ParityPacket &record) {
	trace_.packets.push_back(record);
}
void ParityTraceCollector::on_event(const ParityEvent &record) {
	trace_.events.push_back(record);
}
void ParityTraceCollector::on_uplink(const ParityUplinkState &record) {
	trace_.uplinks.push_back(record);
}
void ParityTraceCollector::on_frame(const ParityFrameState &record) {
	trace_.frames.push_back(record);
}
void ParityTraceCollector::on_entity(const ParityEntity &record) {
	trace_.entities.push_back(record);
}

bool read_parity_trace(
		const std::string &path, const ItemClassMap *item_classes, ParityTrace &out) {
	ParityTraceCollector collector(out);
	ParityTraceBuilder builder(collector, item_classes);
	return net::stream_pcap_udp_file(path, [&builder](const net::PcapDatagram &datagram) {
		builder.push(datagram);
		return true;
	});
}

} // namespace opennova::wire
