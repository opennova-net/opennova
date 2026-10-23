// opennova-parity's metrics and verdict (apps/parity) over synthetic canonical
// traces: a scripted session (handshake, the initial reliable stream, a steady
// 0x0C/0x0A gameplay loop with header-only ACKs, alternating integrity challenges)
// whose per-run dynamic fields (keys, ticks, the connection index, a position
// jitter) follow a seed, so RR, RR-aa and the subject are independent runs of one
// case. Each metric is pinned identical / within its floor / beyond it, plus the
// A/A widening rule, the evidence checks, the normalization registry and the
// verdict document. No captures, no retail data.

#include "metric.h"
#include "normalization.h"
#include "run_facts.h"
#include "verdict.h"

#include <base/io/json.h>
#include <net/npwire/flat_tlv.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace opennova;
using namespace opennova::parity;

namespace {

int g_failures = 0;

void check(bool cond, const std::string &what) {
	if (cond)
		return;
	std::fprintf(stderr, "FAIL: %s\n", what.c_str());
	++g_failures;
}

constexpr uint64_t kMs = 1000000ull;
constexpr uint64_t kSec = 1000000000ull;
constexpr int kClientPort = 30000;
constexpr int kServerPort = 32786;
constexpr uint64_t kTick = 32 * kMs;

struct Msg {
	uint16_t identity;
	std::vector<uint8_t> body;
};

std::vector<uint8_t> bytes(std::initializer_list<int> values) {
	std::vector<uint8_t> out;
	for (const int v : values)
		out.push_back(uint8_t(v));
	return out;
}

std::vector<uint8_t> text_bytes(const std::string &text) {
	std::vector<uint8_t> out(text.begin(), text.end());
	out.push_back(0);
	return out;
}

void put_u32(std::vector<uint8_t> &out, uint32_t v) {
	for (int i = 0; i < 4; ++i)
		out.push_back(uint8_t(v >> (8 * i)));
}

void tlv_u32(std::vector<uint8_t> &out, const char *name, uint32_t v) {
	std::vector<uint8_t> value;
	put_u32(value, v);
	append_flat_tlv(out, name, value.data(), uint16_t(value.size()));
}

void tlv_text(std::vector<uint8_t> &out, const char *name, const std::string &text) {
	const std::vector<uint8_t> value = text_bytes(text);
	append_flat_tlv(out, name, value.data(), uint16_t(value.size()));
}

// A session crypt key as the manager LCG draws it: from draw index `start`, each
// next index 17x + 1 mod 32, index 31 (the charset's NUL) appending nothing.
std::string lcg_key(uint32_t start, size_t chars) {
	static const char kCharset[] = "0123456789BCDFGHJKLMNPQRSTVWXYZ";
	std::string out;
	uint32_t x = start & 31u;
	while (out.size() < chars) {
		if (x != 31u)
			out.push_back(kCharset[x]);
		x = (17u * x + 1u) & 31u;
	}
	return out;
}

// The trace builder's world fields (parity_trace.cpp): the decompressed code plus
// the frame anchor.
void place(wire::ParityEntity &e, const wire::ParityFrameState &frame) {
	e.offset_x = network_decompress_fixedpoint(uint16_t(e.pos_x));
	e.offset_y = network_decompress_fixedpoint(uint16_t(e.pos_y));
	e.offset_z = network_decompress_fixedpoint(uint16_t(e.pos_z));
	e.world_x = frame.anchor_x + e.offset_x;
	e.world_y = frame.anchor_y + e.offset_y;
	e.world_z = frame.anchor_z + e.offset_z;
}

void tlv_cu(
		std::vector<uint8_t> &out, uint8_t type, const std::string &name, const std::string &text) {
	std::vector<uint8_t> value{ type };
	value.insert(value.end(), name.begin(), name.end());
	value.push_back(0);
	const std::vector<uint8_t> v = text_bytes(text);
	value.push_back(uint8_t(v.size()));
	value.push_back(uint8_t(v.size() >> 8));
	value.insert(value.end(), v.begin(), v.end());
	append_flat_tlv(out, "CU", value.data(), uint16_t(value.size()));
}

// One run of the scripted case.
class Session {
public:
	explicit Session(uint32_t seed) : seed_(seed) {}

	wire::ParityTrace trace;

	void handshake(uint64_t at, uint8_t opcode, std::vector<uint8_t> body) {
		const int frame = ++frame_;
		const bool client = opcode < 0x80;
		wire::ParityDatagram d;
		d.frame = frame;
		d.ts_ns = at;
		d.src_port = client ? kClientPort : kServerPort;
		d.dst_port = client ? kServerPort : kClientPort;
		d.length = body.size() + 5;
		d.outer_decoded = true;
		d.opcode = opcode;
		d.datagram_class = opcode == 0x41 ? CaptureDatagramClass::ClientHello
				: opcode == 0x42		  ? CaptureDatagramClass::ClientAuth
				: opcode == 0x81		  ? CaptureDatagramClass::ServerHello
										  : CaptureDatagramClass::ServerAuth;
		d.decoded = true;
		trace.datagrams.push_back(d);
		wire::ParityHandshake h;
		h.frame = frame;
		h.ts_ns = at;
		h.dir = client ? 'C' : 'S';
		h.session = kClientPort;
		h.opcode = opcode;
		h.decoded = true;
		h.body = std::move(body);
		trace.handshakes.push_back(h);
	}

	// One protocol packet; a gameplay message adds its decoded state.
	void send(char dir, uint64_t at, const std::vector<Msg> &messages, uint8_t flags = 0x01,
			int tick = 0) {
		const int frame = ++frame_;
		wire::ParityPacket p;
		p.frame = frame;
		p.ts_ns = at;
		p.dir = dir;
		p.session = kClientPort;
		p.sid = dir == 'C' ? 0x1000 + seed_ : 0x2000 + seed_;
		p.seq = dir == 'C' ? ++seq_c_ : ++seq_s_;
		p.ack = dir == 'C' ? seq_s_ : seq_c_;
		p.flags = flags;
		size_t length = 12;
		for (const Msg &m : messages) {
			const size_t body_len =
					gameplay(dir, m.identity) ? gameplay_length(dir) : m.body.size();
			length += body_len + 2;
			p.tags.push_back(m.identity);
			wire::ParityWireRecord r;
			r.full_tag = m.identity;
			r.raw_flags =
					uint8_t((body_len <= 0xFF ? 0x20 : 0x40) | (m.identity >= 0x100 ? 0x80 : 0));
			r.encoded_length = uint32_t(body_len);
			p.records.push_back(r);
			wire::ParityEvent e;
			e.frame = frame;
			e.ts_ns = at;
			e.dir = dir;
			e.session = kClientPort;
			e.participant = 1;
			e.full_tag = m.identity;
			e.settings_update = m.identity >= 0x100;
			e.length = body_len;
			e.material = !gameplay(dir, m.identity);
			if (e.material)
				e.body = m.body;
			trace.events.push_back(e);
			if (dir == 'C' && m.identity == 0x00C)
				uplink(frame, at, tick);
			if (dir == 'S' && m.identity == 0x00A)
				frame_update(frame, at, tick);
		}
		trace.packets.push_back(p);
		datagram(dir, frame, at, length, true);
	}

	void ack(char dir, uint64_t at, uint8_t flags = 0x02) {
		const int frame = ++frame_;
		wire::ParityPacket p;
		p.frame = frame;
		p.ts_ns = at;
		p.dir = dir;
		p.session = kClientPort;
		p.sid = dir == 'C' ? 0x1000 + seed_ : 0x2000 + seed_;
		p.seq = dir == 'C' ? seq_c_ : seq_s_;
		p.ack = dir == 'C' ? seq_s_ : seq_c_;
		p.flags = flags;
		trace.packets.push_back(p);
		datagram(dir, frame, at, 12, true);
	}

	uint32_t seed() const { return seed_; }

private:
	static bool gameplay(char dir, uint16_t identity) {
		return (dir == 'C' && identity == 0x00C) || (dir == 'S' && identity == 0x00A);
	}
	static size_t gameplay_length(char dir) { return dir == 'C' ? 48 : 84; }

	void datagram(char dir, int frame, uint64_t at, size_t length, bool decoded) {
		wire::ParityDatagram d;
		d.frame = frame;
		d.ts_ns = at;
		d.src_port = dir == 'C' ? kClientPort : kServerPort;
		d.dst_port = dir == 'C' ? kServerPort : kClientPort;
		d.length = length;
		d.outer_decoded = true;
		d.opcode = dir == 'C' ? 0x43 : 0x83;
		d.datagram_class = dir == 'C' ? CaptureDatagramClass::ClientProtocol
									  : CaptureDatagramClass::ServerProtocol;
		d.decoded = decoded;
		trace.datagrams.push_back(d);
	}

	void uplink(int frame, uint64_t at, int tick) {
		wire::ParityUplinkState u;
		u.frame = frame;
		u.ts_ns = at;
		u.session = kClientPort;
		u.decoded = true;
		u.length = 48;
		u.handle = 0x1000;
		u.type_id = 5305;
		u.sub_op = 10;
		u.carrier = 0xFFFF;
		u.pos_x = int32_t(1000 * 65536 + tick * 16384 + int32_t(seed_) * 65536);
		u.pos_y = int32_t(2000 * 65536 + (tick % 50) * 8192);
		u.pos_z = 50 * 65536;
		u.heading = int32_t((tick * 300) % 65536) - 32768;
		u.pitch = 256;
		u.move = tick % 3 == 0 ? 1 : 0;
		u.state_flags = 0;
		u.analog_x = uint32_t((tick * 7) % 256);
		u.analog_y = 128;
		u.adm = 3;
		u.priority_handle[0] = 0x2003;
		u.priority_score[0] = 100;
		u.priority_nonzero = 1;
		trace.uplinks.push_back(u);
	}

	void frame_update(int frame, uint64_t at, int tick) {
		wire::ParityFrameState f;
		f.frame = frame;
		f.ts_ns = at;
		f.session = kClientPort;
		f.decoded = true;
		f.length = 84;
		f.sub_block = uint32_t(tick % 4);
		f.flags2 = uint32_t(tick) & 0xFFu; // the per-slot send counter; its low bits route
		f.anchor_x = int32_t(1000 * 65536 + tick * 16384 + int32_t(seed_) * 65536);
		f.anchor_y = 2000 * 65536;
		f.anchor_z = 50 * 65536;
		f.local_tail = true;
		f.health = 100;
		f.records = 3;
		f.players = 2;
		f.vehicles = 1;
		f.carried = 0xFFFF;
		f.weapon_present = f.sub_block == 0;
		if (f.weapon_present)
			f.weapon[6] = 0;
		f.timer_present = f.sub_block == 1;
		if (f.timer_present) {
			f.timer_state[0] = 20;
			f.timer_state[1] = 13;
			f.timer_state[2] = 62;
			f.timer_seconds = 600 - tick / 31 - int32_t(seed_);
		}
		f.env_present = f.sub_block == 2;
		if (f.env_present) {
			f.env[0] = 0x4000;
			f.env[2] = uint32_t(tick * 3 + seed_ * 1000) & 0xFFFF;
			f.env[6] = 0x20;
		}
		f.objective_present = f.sub_block == 3;
		if (f.objective_present)
			f.objective[0] = 1;
		trace.frames.push_back(f);
		// The recipient's own record (its uplink handle) against the eye anchor:
		// -CameraOffset, z code 0x6801 = -0xD001.
		wire::ParityEntity self;
		self.frame = frame;
		self.ts_ns = at;
		self.session = kClientPort;
		self.cls = 'P';
		self.handle = 0x1000;
		self.type_id = 5305;
		self.pos_z = 0x6801;
		self.orient_a = (tick * 5) % 256;
		self.orient_b = 2;
		self.state_a = 4;
		self.vital = 100;
		place(self, f);
		trace.entities.push_back(self);
		wire::ParityEntity p;
		p.frame = frame;
		p.ts_ns = at;
		p.session = kClientPort;
		p.cls = 'P';
		p.handle = 0x1001;
		p.type_id = 5305;
		p.pos_x = uint32_t(tick * 64 % 60000);
		p.pos_y = 3000 + seed_;
		p.pos_z = 100;
		p.orient_a = (tick * 5) % 256;
		p.orient_b = 2;
		p.state_a = 4;
		p.vital = 100;
		place(p, f);
		trace.entities.push_back(p);
		wire::ParityEntity v = p;
		v.cls = 'V';
		v.handle = 0x2003;
		v.type_id = 1291;
		v.pos_x = 5000;
		v.pos_y = 6000 + uint32_t(tick % 40) * 10;
		v.orient_a = (tick * 40) % 65536;
		v.state_a = 0;
		v.vital = 3000;
		place(v, f);
		trace.entities.push_back(v);
	}

	uint32_t seed_;
	int frame_ = 0;
	uint32_t seq_c_ = 0, seq_s_ = 0;
};

constexpr uint64_t kSteadyStart = 5 * kSec;
constexpr double kSteadySeconds = 15.0;

// A S2C 0x46 slot record with the name, quality and NovaWorld squad id fields
// (mask 0x0C01); the squad id is per account (initial.S.0x46.timer_score).
std::vector<uint8_t> player_sync(int slot, int entity, int quality, uint32_t squad) {
	std::vector<uint8_t> body = bytes({ slot, 0x01, 0x0C, entity });
	const std::vector<uint8_t> name = text_bytes("Host");
	body.insert(body.end(), name.begin(), name.end());
	body.push_back(uint8_t(quality));
	put_u32(body, squad);
	return body;
}

std::vector<uint8_t> world_state_load(uint32_t seed) {
	std::vector<uint8_t> body;
	put_u32(body, 0x1000 + seed * 77); // the live session tick
	for (int i = 0; i < 24; ++i)
		body.push_back(uint8_t(i * 3));
	return body;
}

// The scripted case, run `seed`.
wire::ParityTrace scripted(uint32_t seed) {
	Session s(seed);
	uint64_t t = 1 * kSec;
	{
		std::vector<uint8_t> b;
		tlv_text(b, "NVS", "NAPI NP Version 0.0.1");
		tlv_text(b, "PN", "JointOperations");
		tlv_u32(b, "CI", seed * 11);
		tlv_u32(b, "EIP", 0xFFFFFFFFu); // the LAN browse's broadcast target
		tlv_u32(b, "EPN", kServerPort); // the enumerated host port
		s.handshake(t, 0x41, b);
	}
	{
		std::vector<uint8_t> b;
		tlv_u32(b, "CI", seed * 11);
		tlv_text(b, "AP", "JOINTOPS.EXE");
		tlv_u32(b, "HK", 0x0FE0E112 ^ seed);
		tlv_text(b, "SN", "Untitled ");
		tlv_text(b, "SUS1", "GSID-01-" + std::to_string(100000 + seed * 99991));
		s.handshake(t + 10 * kMs, 0x81, b);
	}
	{
		std::vector<uint8_t> b;
		tlv_u32(b, "CI", seed * 11);
		tlv_u32(b, "HK", 0x0FE0E112 ^ seed);
		tlv_u32(b, "CK", 0xC0FFEE00 + seed);
		// 61 or 62 characters by where the draw falls (handshake.0x42.SCRK.length).
		tlv_text(b, "SCRK", lcg_key(seed * 7, 61 + seed % 2));
		tlv_cu(b, 1, "UdpCode1", std::to_string(1000 + seed * 7));
		tlv_cu(b, 2, "Callsign", "ParityJoin");
		// The u16 character id printed from a stack dword: the high word is residue.
		tlv_cu(b, 3, "CI0", std::to_string(0x0200u | (seed << 16)));
		s.handshake(t + 20 * kMs, 0x42, b);
	}
	{
		std::vector<uint8_t> b;
		tlv_u32(b, "CI", seed * 11);
		tlv_u32(b, "MI", seed);
		tlv_u32(b, "SK", 0xABC00000 + seed);
		tlv_text(b, "SCRK", lcg_key(seed * 5 + 3, 61 + (seed + 1) % 2));
		tlv_cu(b, 3, "NWUID", std::to_string(1000 + seed));
		tlv_cu(b, 3, "NovaworldName", "NWServer");
		s.handshake(t + 30 * kMs, 0x82, b);
	}

	// The initial reliable stream.
	t = 1100 * kMs;
	auto step = [&t]() { return t += 20 * kMs; };
	s.send('S', step(), { { 0x075, bytes({ 1, 2, 3 }) }, { 0x060, bytes({ 4, 5, 6, 7 }) } });
	s.send('C', step(), { { 0x047, bytes({ 9, 9 }) } });
	s.send('S', step(),
			{ { 0x075, bytes({ 1, 2, 4 }) }, { 0x064, bytes({ 0 }) },
					{ 0x016, bytes({ 7, 7, 7 }) } });
	s.send('C', step(), { { 0x022, bytes({ 1 }) }, { 0x009, bytes({ 2, 2 }) } });
	s.send('S', step(),
			{ { 0x046, player_sync(0, 5, 1, seed) }, { 0x02C, bytes({ 5, 5, 5, 5 }) } });
	s.send('S', step(), { { 0x008, bytes({ 0x20, 0x00, 0x01, 0x00, 0x05 }) } });
	for (int i = 0; i < 3; ++i)
		s.send('S', step(), { { 0x02A, bytes({ i, 1, 2, 3 }) } });
	s.send('S', step(),
			{ { 0x01C, {} }, { 0x00B, bytes({ 0x42, 0x4D, 0x53, 0x0B }) }, { 0x066, bytes({ 1 }) },
					{ 0x076, bytes({ 2 }) }, { 0x011, bytes({ 3 }) } });
	s.send('S', step(), { { 0x07B, text_bytes("Untitled ") } });
	s.send('C', step(), { { 0x00A, bytes({ 1, 0, 0, 0 }) } });
	s.send('S', step(), { { 0x019, bytes({ int(seed), int(seed >> 8), 0x33, 0x44 }) } });
	s.send('S', step(), { { 0x00F, world_state_load(seed) } });
	s.send('C', step(), { { 0x00B, bytes({ 1 }) } });

	// The steady gameplay loop, through the capture's end at 22 s.
	bool ammo = true;
	uint64_t next_challenge = 6 * kSec;
	int tick = 0;
	for (t = 2 * kSec; t < 22 * kSec; t += kTick, ++tick) {
		s.send('C', t, { { 0x00C, {} } }, 0x01, tick);
		if (tick % 4 == 0)
			s.ack('C', t + 4 * kMs);
		if (tick % 8 == 0)
			s.send('S', t + 8 * kMs, { { 0x00A, {} }, { 0x02C, bytes({ 5, 5, 5, 5 }) } }, 0x01,
					tick);
		else
			s.send('S', t + 8 * kMs, { { 0x00A, {} } }, 0x01, tick);
		if (t >= next_challenge) {
			const uint32_t key = (seed * 2654435761u + uint32_t(tick)) & 0xFFFF;
			const uint8_t row = ammo ? 0x18 : 0x05;
			const uint32_t crc = ammo ? 0xDEADBEEFu : 0x12345678u;
			s.send('S', t + 12 * kMs,
					{ { 0x016, bytes({ 7, 7, 7 }) },
							{ uint16_t(ammo ? 0x031 : 0x030),
									bytes({ row, int(key & 0xFF), int(key >> 8) }) } });
			std::vector<uint8_t> reply{ row };
			put_u32(reply, crc ^ key);
			if (ammo)
				put_u32(reply, key);
			s.send('C', t + 20 * kMs, { { uint16_t(ammo ? 0x021 : 0x020), reply } });
			ammo = !ammo;
			next_challenge += 5 * kSec;
		}
	}
	return std::move(s.trace);
}

RunInfo info_for(const char *label, const char *topology) {
	RunInfo info;
	info.label = label;
	info.run_dir = std::string("synthetic/") + label;
	info.topology = topology;
	info.run_id = label;
	info.mission = "TEST.BMS";
	info.game_type = "65568";
	info.integrity_profile = "retail-test-profile";
	info.port = kServerPort;
	info.capture = std::string("synthetic/") + label + ".pcap";
	info.perspective = "retail-client";
	info.steady_seconds = kSteadySeconds;
	info.steady_start_ns = kSteadyStart;
	info.steady_end_ns = kSteadyStart + uint64_t(kSteadySeconds * 1e9);
	info.steady_completed_ns = info.steady_end_ns + 500 * kMs;
	return info;
}

struct Case {
	RunFacts rr, aa, subject;
	Verdict verdict;
};

Case judge_case(wire::ParityTrace rr, wire::ParityTrace aa, wire::ParityTrace subject,
		const char *topology = "RO", bool rotation = false, bool ends_session = false) {
	Case c;
	auto info = [rotation, ends_session](const char *label, const char *top) {
		RunInfo i = info_for(label, top);
		i.rotation = rotation;
		i.ends_session = ends_session;
		return i;
	};
	c.rr = make_run_facts(info("rr", "RR"), std::move(rr));
	c.aa = make_run_facts(info("rr_aa", "RR"), std::move(aa));
	c.subject = make_run_facts(info("subject", topology), std::move(subject));
	c.verdict = judge(c.rr, c.aa, { { topology, &c.subject } });
	return c;
}

const VerdictRow *find_row(const std::vector<VerdictRow> &rows, char dir, const std::string &name,
		const std::string &key = "") {
	for (const VerdictRow &row : rows)
		if (row.value.direction == dir && row.value.metric == name && row.value.key == key)
			return &row;
	return nullptr;
}

void expect_row(const Case &c, char dir, const std::string &name, const std::string &key,
		const char *status, const char *code = nullptr) {
	const VerdictRow *row = find_row(c.verdict.rows, dir, name, key);
	const std::string label = std::string(1, dir) + " " + name + " [" + key + "]";
	check(row != nullptr, label + " row exists");
	if (!row)
		return;
	check(row->status == status,
			label + " is " + status + " (got " + row->status + ", distance " +
					std::to_string(row->value.distance) + ", threshold " +
					std::to_string(row->threshold) + ", " + row->value.detail + ")");
	if (code)
		check(row->value.code == code,
				label + " reports " + code + " (got " + row->value.code + ")");
}

size_t failing_rows(const Case &c) {
	size_t n = 0;
	for (const VerdictRow &row : c.verdict.rows) {
		if (row.status != "fail")
			continue;
		if (++n <= 8)
			std::fprintf(stderr, "  failing: %c %s [%s] %s d=%g t=%g %s\n", row.value.direction,
					row.value.metric.c_str(), row.value.key.c_str(), row.value.code.c_str(),
					row.value.distance, row.threshold, row.value.detail.c_str());
	}
	return n;
}

template <typename F> void for_events(wire::ParityTrace &trace, char dir, uint16_t identity, F f) {
	for (wire::ParityEvent &event : trace.events)
		if (event.dir == dir && event.full_tag == identity)
			f(event);
}

void erase_events(
		wire::ParityTrace &trace, char dir, uint16_t identity,
		const std::function<bool(size_t)> &which = [](size_t) { return true; }) {
	std::vector<wire::ParityEvent> kept;
	size_t index = 0;
	for (wire::ParityEvent &event : trace.events) {
		if (event.dir == dir && event.full_tag == identity && which(index++))
			continue;
		kept.push_back(std::move(event));
	}
	trace.events = std::move(kept);
}

// ---------------------------------------------------------------------------

void test_registry() {
	std::set<std::string> ids;
	for (const Normalization &entry : kNormalizations) {
		check(entry.reason && std::strlen(entry.reason) > 10,
				std::string("normalization ") + entry.id + " cites its nondeterminism source");
		check(ids.insert(entry.id).second,
				std::string("normalization id ") + entry.id + " is unique");
	}
	check(kNormalizationCount >= 40, "the registry carries the retired verifier's masks");
	check(handshake_field_masked(0x41, "CI") && !handshake_field_masked(0x41, "PN"),
			"the handshake masks are the registry's");
	check(initial_body_exempt('C', 0x021) && !initial_body_exempt('S', 0x07B),
			"the initial-body exemptions are the registry's");
}

// The registered body masks: S2C 0x1A's tick is out of byte equality; S2C 0x64's
// session block zeroes only its random seed, session id and token.
void test_body_masks() {
	check(initial_body_exempt('S', 0x01A), "the 0x1A game-start tick is exempt");

	auto chunk = [](uint32_t offset, uint8_t fill, uint8_t name) {
		std::string body(12, '\0');
		body[0] = 1;                    // transfer id
		body[4] = char(180);            // total size
		body[8] = char(offset & 0xFF);  // chunk offset
		// Random bytes (0..35, 148..179) get `fill`, the rules word and its neighbours
		// (36..51) a constant, the three names (52..147) `name`.
		for (uint32_t at = offset; at < 180; ++at)
			body.push_back(char(at < 36 || at >= 148 ? fill : at < 52 ? 0x04 : name));
		return body;
	};
	check(stable_body_key('S', 0x064, chunk(0, 0x11, 'A')) ==
					stable_body_key('S', 0x064, chunk(0, 0x77, 'A')),
			"0x64: the seed, session id and token are masked");
	check(stable_body_key('S', 0x064, chunk(0, 0x11, 'A')) !=
					stable_body_key('S', 0x064, chunk(0, 0x11, 'B')),
			"0x64: the names compare");
	check(stable_body_key('S', 0x064, chunk(140, 0x11, 'A')) ==
					stable_body_key('S', 0x064, chunk(140, 0x77, 'A')),
			"0x64: a chunk past the names masks the token by its block offset");
}

void test_timestamps() {
	uint64_t ns = 0;
	check(parse_utc_ns("1970-01-01T00:00:10.0000000Z", ns) && ns == 10 * kSec, "parse 7 digits");
	check(parse_utc_ns("2026-10-11T12:34:56Z", ns) && ns == 1791722096ull * kSec,
			"parse no fraction");
	check(parse_utc_ns("2024-02-29T00:00:00.5Z", ns) && ns == 1709164800ull * kSec + 500 * kMs,
			"parse a leap day");
	check(parse_utc_ns("1970-01-01T00:00:00.123456789Z", ns) && ns == 123456789ull, "parse ns");
	for (const char *bad : { "1970-01-01T00:00:10+00:00", "1970-01-01 00:00:10Z",
				 "2023-02-29T00:00:00Z", "1970-01-01T00:00:10.Z", "1970-01-01T00:00:10.0000000000Z",
				 "1970-13-01T00:00:00Z", "1969-12-31T23:59:59Z", "" })
		check(!parse_utc_ns(bad, ns), std::string("reject '") + bad + "'");
}

void test_identical() {
	const Case c = judge_case(scripted(1), scripted(2), scripted(3));
	for (const EvidenceRow &row : c.verdict.evidence)
		std::fprintf(stderr, "  evidence: %s %s %s\n", row.run.c_str(), row.code.c_str(),
				row.detail.c_str());
	check(c.verdict.evidence.empty(), "independent runs of the scripted case carry clean evidence");
	check(failing_rows(c) == 0, "independent runs of one case pass every metric");
	check(c.verdict.pass(), "the verdict passes");
	size_t widened = 0;
	for (const VerdictRow &row : c.verdict.aa)
		widened += row.status == "widened" ? 1 : 0;
	check(widened == 0, "the A/A run sits inside every fixed floor");
	check(c.verdict.rows.size() > 150, "the comparison covers the metric catalog");
	check(c.subject.session == kClientPort, "the run binds its gameplay session");
	// The dynamic fields really differ between runs; the registry masks them.
	expect_row(c, 'C', "handshake", "0x41", "pass");
	expect_row(c, 'S', "handshake", "0x82", "pass");
	expect_row(c, 'S', "stable_body_set", "0x0f:0", "pass");
	expect_row(c, 'S', "initial_body", "0x0f:0", "pass");
	check(find_row(c.verdict.rows, 'S', "initial_body", "0x19:0") == nullptr,
			"S2C 0x19 (a live timestamp) is exempt from initial byte equality");
	expect_row(c, 'B', "integrity_crc", "ammo", "pass");
	expect_row(c, 'C', "uplink_position_range_p90", "", "pass");
}

void test_tag_histogram() {
	{
		wire::ParityTrace subject = scripted(3);
		erase_events(subject, 'S', 0x07B);
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "tag_presence", "0x7b:0", "fail", "MISSING_TAG");
		check(!c.verdict.pass(), "a missing tag fails the verdict");
	}
	{
		wire::ParityTrace subject = scripted(3);
		wire::ParityEvent extra = subject.events.back();
		extra.dir = 'S';
		extra.full_tag = 0x055;
		extra.ts_ns = 10 * kSec;
		subject.events.push_back(extra);
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "tag_presence", "0x55:0", "fail", "UNEXPECTED_TAG");
	}
	{
		wire::ParityTrace within = scripted(3), beyond = scripted(3);
		erase_events(within, 'S', 0x02C, [](size_t i) { return i % 10 == 3; });
		erase_events(beyond, 'S', 0x02C, [](size_t i) { return i % 2 == 1; });
		expect_row(
				judge_case(scripted(1), scripted(2), within), 'S', "tag_count", "0x2c:0", "pass");
		expect_row(judge_case(scripted(1), scripted(2), beyond), 'S', "tag_count", "0x2c:0", "fail",
				"COUNT_MISMATCH");
	}
	{
		wire::ParityTrace subject = scripted(3);
		for_events(subject, 'S', 0x02A, [](wire::ParityEvent &e) {
			e.body.push_back(0);
			++e.length;
		});
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "length_set", "0x2a:0", "fail", "LENGTH_SET_MISMATCH");
	}
	{
		// RR's three distinct S2C 0x2A bodies collapse to one constant over ten.
		wire::ParityTrace subject = scripted(3);
		wire::ParityEvent first_2a;
		for_events(subject, 'S', 0x02A, [&first_2a](wire::ParityEvent &e) {
			if (first_2a.full_tag == 0)
				first_2a = e;
		});
		for (int i = 0; i < 7; ++i)
			subject.events.push_back(first_2a);
		for_events(
				subject, 'S', 0x02A, [](wire::ParityEvent &e) { e.body = bytes({ 0, 1, 2, 3 }); });
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "body_constant", "0x2a:0", "fail", "BODY_CONSTANT");
	}
	{
		wire::ParityTrace subject = scripted(3);
		for_events(
				subject, 'S', 0x060, [](wire::ParityEvent &e) { e.body.assign(e.body.size(), 0); });
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "body_all_zero", "0x60:0", "fail", "BODY_ALL_ZERO");
	}
	{
		wire::ParityTrace subject = scripted(3);
		for_events(
				subject, 'S', 0x07B, [](wire::ParityEvent &e) { e.body = text_bytes("Untitled"); });
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "stable_body_set", "0x7b:0", "fail", "STABLE_BODY_MISMATCH");
	}
	{
		// Past S2C 0x0F's live tick the body is the case's own.
		wire::ParityTrace subject = scripted(3);
		for_events(subject, 'S', 0x00F, [](wire::ParityEvent &e) { e.body[10] ^= 0xFF; });
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "stable_body_set", "0x0f:0", "fail", "STABLE_BODY_MISMATCH");
		expect_row(c, 'S', "initial_body", "0x0f:0", "fail", "INITIAL_BODY_MISMATCH");
	}
	{
		wire::ParityTrace subject = scripted(3);
		size_t first_08 = 0, first_7b = 0;
		for (size_t i = 0; i < subject.events.size(); ++i) {
			const wire::ParityEvent &e = subject.events[i];
			if (e.dir == 'S' && e.full_tag == 0x008 && !first_08)
				first_08 = i;
			if (e.dir == 'S' && e.full_tag == 0x07B && !first_7b)
				first_7b = i;
		}
		std::swap(subject.events[first_08].full_tag, subject.events[first_7b].full_tag);
		std::swap(subject.events[first_08].body, subject.events[first_7b].body);
		std::swap(subject.events[first_08].length, subject.events[first_7b].length);
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "first_order", "", "fail", "FIRST_ORDER_MISMATCH");
	}
}

// A periodic identity's small count is judged by RR's cadence over the compared
// run's span from its first occurrence, not raw: a stream that starts at a session
// event (ASH's integrity challenges after the joiner's C2S 0x0B) runs longer when the
// readiness before it differs.
void test_cadence_counts() {
	check(periodic_cadence_ms({ 10 * kSec, 20 * kSec, 30 * kSec, 40 * kSec }, 45 * kSec) == 10000.0,
			"a regular stream's cadence is its median gap");
	check(periodic_cadence_ms({ 10 * kSec, 21 * kSec, 30 * kSec, 40 * kSec, 49 * kSec }, 50 * kSec) ==
					9500.0,
			"the median of an even gap count is the middle pair's mean");
	check(periodic_cadence_ms({ 10 * kSec, 20 * kSec }, 25 * kSec) == 0.0,
			"two occurrences are no cadence");
	check(periodic_cadence_ms({ 10 * kSec, 12 * kSec, 30 * kSec }, 35 * kSec) == 0.0,
			"irregular gaps are no cadence");
	check(periodic_cadence_ms({ 10000 * kMs, 10200 * kMs, 10400 * kMs }, 40 * kSec) == 0.0,
			"a burst that stopped is no cadence");
	{
		// R4's shape: four in RR from 25 s, six in the subject from 24 s over 11 s more.
		MetricValue raw, cadence;
		count_metric(raw, 4, 63000, 6, 80000);
		check(raw.distance > raw.floor, "the raw count rule fails 6 against 4");
		cadence_count_metric(cadence, 4, 10000, 6, 56000);
		check(cadence.distance <= cadence.floor, "RR's cadence over the subject's span predicts 6");
		MetricValue doubled;
		cadence_count_metric(doubled, 4, 10000, 12, 56000);
		check(doubled.distance > doubled.floor, "twice the cadence fails");
	}

	auto add = [](wire::ParityTrace &trace, const std::vector<uint64_t> &times) {
		const wire::ParityEvent model = trace.events.back();
		for (const uint64_t ts : times) {
			wire::ParityEvent e = model;
			e.dir = 'S';
			e.full_tag = 0x079;
			e.ts_ns = ts;
			e.body = bytes({ 1, 0, 0, 0 });
			e.length = e.body.size();
			trace.events.push_back(e);
		}
	};
	auto ms = [](std::initializer_list<uint64_t> list) {
		std::vector<uint64_t> out;
		for (const uint64_t v : list)
			out.push_back(v * kMs);
		return out;
	};
	wire::ParityTrace rr = scripted(1), aa = scripted(2);
	add(rr, ms({ 10500, 13500, 16500, 19500 }));
	add(aa, ms({ 10600, 13600, 16600, 19600 }));
	{
		wire::ParityTrace subject = scripted(3);
		add(subject, ms({ 4500, 7500, 10500, 13500, 16500, 19500 }));
		expect_row(judge_case(rr, aa, subject), 'S', "tag_count", "0x79:0", "pass");
	}
	{
		wire::ParityTrace subject = scripted(3);
		std::vector<uint64_t> fast;
		for (uint64_t t = 4500; t <= 19500; t += 1500)
			fast.push_back(t * kMs);
		add(subject, fast);
		expect_row(judge_case(rr, aa, subject), 'S', "tag_count", "0x79:0", "fail", "COUNT_MISMATCH");
	}
	{
		wire::ParityTrace subject = scripted(3);
		add(subject, ms({ 4500, 7500, 10500 }));
		expect_row(judge_case(rr, aa, subject), 'S', "tag_count", "0x79:0", "fail", "COUNT_MISMATCH");
	}
}

// The tag histogram's window: each run's first packet plus the pair's shorter
// comparison interval, so a run whose readiness took longer is not counted over
// more of the mission clock; and an identity landing just past a run's window end
// is not missing (10 s presence grace), while one far past it is.
void test_tag_window() {
	auto add = [](wire::ParityTrace &trace, uint16_t identity, const std::vector<uint64_t> &times) {
		const wire::ParityEvent model = trace.events.back();
		for (const uint64_t ts : times) {
			wire::ParityEvent e = model;
			e.dir = 'S';
			e.full_tag = identity;
			e.ts_ns = ts;
			e.body = bytes({ 3, 0 });
			e.length = e.body.size();
			trace.events.push_back(e);
		}
	};
	auto burst = [](uint64_t from_ms) {
		std::vector<uint64_t> out;
		for (uint64_t i = 0; i < 10; ++i)
			out.push_back((from_ms + i * 90) * kMs);
		return out;
	};
	auto longer = [](wire::ParityTrace &trace) {
		// The session's first packet a second earlier: a longer comparison interval.
		wire::ParityPacket early = trace.packets.front();
		early.ts_ns -= 1 * kSec;
		trace.packets.insert(trace.packets.begin(), early);
	};
	{
		wire::ParityTrace rr = scripted(1), aa = scripted(2), subject = scripted(3);
		add(rr, 0x055, burst(10000));
		add(aa, 0x055, burst(10000));
		add(subject, 0x055, burst(10000));
		add(subject, 0x055, burst(19050)); // past the common window, inside its own interval
		longer(subject);
		expect_row(judge_case(rr, aa, subject), 'S', "tag_count", "0x55:0", "pass");
	}
	{
		wire::ParityTrace rr = scripted(1), aa = scripted(2), near = scripted(3), far = scripted(3);
		add(rr, 0x056, { 18 * kSec });
		add(aa, 0x056, { 18 * kSec });
		add(near, 0x056, { 20500 * kMs });
		add(far, 0x056, { 35 * kSec });
		expect_row(judge_case(rr, aa, near), 'S', "tag_presence", "0x56:0", "pass");
		expect_row(judge_case(rr, aa, far), 'S', "tag_presence", "0x56:0", "fail", "MISSING_TAG");
	}
}

// A stream that opens at the joiner's own join (the per-slot overlay visit's S2C 0x40
// runs only for a state-6 slot, which the join's C2S 0x0B sets) counts from each run's
// join over the pair's shorter span from it (window.tag_stream_anchor): x1's kick RR
// joined 34.6 s after its hello and RO 24.2 s, and the same stream counted over the
// common window read 59 against 88. A stream that opens late after its join counts
// short. A drawn burst's last 0x40 (after a full 97-byte one, any visit but the join's)
// one entry (6 bytes) from a drawn tail of the other run is that draw
// (length.S.0x40.burst_tail); the join burst's tail and a lone visit's 0x40 stay exact.
void test_stream_anchor_and_burst_tail() {
	// One visit per step: its 0x40s share a datagram. The first visit is the join's;
	// the rest cycle.
	auto stream = [](wire::ParityTrace &trace, uint64_t from, uint64_t step,
						  std::vector<std::vector<size_t>> visits = { { 7 } }) {
		const wire::ParityEvent model = trace.events.back();
		size_t i = 0;
		for (uint64_t ts = from; ts <= 22 * kSec; ts += step, ++i) {
			const std::vector<size_t> &visit = i == 0 || visits.size() == 1
					? visits[0]
					: visits[1 + (i - 1) % (visits.size() - 1)];
			for (const size_t length : visit) {
				wire::ParityEvent e = model;
				e.dir = 'S';
				e.full_tag = 0x040;
				e.ts_ns = ts;
				e.frame = model.frame + 1 + int(i);
				e.body.assign(length, uint8_t(i));
				e.length = e.body.size();
				trace.events.push_back(e);
			}
		}
	};
	auto join_later = [](wire::ParityTrace &trace, uint64_t by) {
		for_events(trace, 'C', 0x00B, [by](wire::ParityEvent &e) { e.ts_ns += by; });
	};
	{
		wire::ParityTrace rr = scripted(1), aa = scripted(2), early = scripted(3), late = scripted(3),
						  fast = scripted(3);
		// The reference joins 4 s later and its stream opens 0.6 s after its join.
		join_later(rr, 4 * kSec);
		join_later(aa, 4 * kSec);
		stream(rr, 6 * kSec, 300 * kMs);
		stream(aa, 6 * kSec, 300 * kMs);
		stream(early, 2 * kSec, 300 * kMs); // the same stream after an earlier join
		stream(late, 12 * kSec, 300 * kMs); // a stream that opens 10.6 s after its join
		stream(fast, 2 * kSec, 150 * kMs);
		expect_row(judge_case(rr, aa, early), 'S', "tag_count", "0x40:0", "pass");
		expect_row(judge_case(rr, aa, late), 'S', "tag_count", "0x40:0", "fail", "COUNT_MISMATCH");
		expect_row(judge_case(rr, aa, fast), 'S', "tag_count", "0x40:0", "fail", "COUNT_MISMATCH");
	}
	{
		// A timer-phase stream (S2C 0x6F, one the first-occurrence order leaves out)
		// counts from the join too: retail draws only its first send's phase, so one
		// opening at another phase passes and one opening 10.6 s after its join fails,
		// where counting from its own first passed it.
		auto timer = [](wire::ParityTrace &trace, uint64_t from) {
			const wire::ParityEvent model = trace.events.back();
			for (uint64_t ts = from; ts <= 22 * kSec; ts += 500 * kMs) {
				wire::ParityEvent e = model;
				e.dir = 'S';
				e.full_tag = 0x06F;
				e.ts_ns = ts;
				e.body = bytes({ 1, 0 });
				e.length = e.body.size();
				trace.events.push_back(e);
			}
		};
		static_assert(first_order_excluded('S', 0x06F));
		wire::ParityTrace rr = scripted(1), aa = scripted(2), phase = scripted(3), late = scripted(3);
		timer(rr, 2 * kSec);
		timer(aa, 2 * kSec);
		timer(phase, 2300 * kMs);
		timer(late, 12 * kSec);
		expect_row(judge_case(rr, aa, phase), 'S', "tag_count", "0x6f:0", "pass");
		expect_row(judge_case(rr, aa, late), 'S', "tag_count", "0x6f:0", "fail", "COUNT_MISMATCH");
	}
	{
		const std::vector<std::vector<size_t>> reference = { { 97, 79 }, { 7 }, { 7 }, { 97, 79 }, { 7 } };
		wire::ParityTrace rr = scripted(1), aa = scripted(2), tail = scripted(3), off = scripted(3),
						  extra = scripted(3), join_off = scripted(3);
		stream(rr, 6 * kSec, 300 * kMs, reference);
		stream(aa, 6 * kSec, 300 * kMs, reference);
		stream(tail, 6 * kSec, 300 * kMs, { { 97, 79 }, { 7 }, { 7 }, { 97, 73 }, { 7 } });
		stream(off, 6 * kSec, 300 * kMs, { { 97, 79 }, { 7 }, { 7 }, { 97, 67 }, { 7 } });
		stream(extra, 6 * kSec, 300 * kMs, { { 97, 79 }, { 7 }, { 97, 79 }, { 7 }, { 97, 73 } });
		// The join's burst is no draw: its tail one entry off is ours.
		stream(join_off, 6 * kSec, 300 * kMs, { { 97, 73 }, { 7 }, { 7 }, { 97, 79 }, { 7 } });
		expect_row(judge_case(rr, aa, tail), 'S', "length_set", "0x40:0", "pass");
		expect_row(judge_case(rr, aa, extra), 'S', "length_set", "0x40:0", "pass");
		expect_row(judge_case(rr, aa, off), 'S', "length_set", "0x40:0", "fail",
				"LENGTH_SET_MISMATCH");
		expect_row(judge_case(rr, aa, join_off), 'S', "length_set", "0x40:0", "fail",
				"LENGTH_SET_MISMATCH");
	}
	{
		// A lone two-entry 0x40 against one-entry visits is no burst tail.
		wire::ParityTrace rr = scripted(1), aa = scripted(2), lone = scripted(3);
		stream(rr, 6 * kSec, 300 * kMs);
		stream(aa, 6 * kSec, 300 * kMs);
		stream(lone, 6 * kSec, 300 * kMs, { { 7 }, { 7 }, { 13 }, { 7 } });
		expect_row(judge_case(rr, aa, lone), 'S', "length_set", "0x40:0", "fail",
				"LENGTH_SET_MISMATCH");
	}
}

// A message spliced into a scripted trace at `at`, in the datagram of the event before it
// (the trace keeps capture order).
void splice(wire::ParityTrace &trace, char dir, uint16_t identity, uint64_t at,
		std::vector<uint8_t> body) {
	auto at_or_after = std::find_if(trace.events.begin(), trace.events.end(),
			[at](const wire::ParityEvent &event) { return event.ts_ns > at; });
	wire::ParityEvent event;
	event.frame = at_or_after == trace.events.begin() ? 1 : std::prev(at_or_after)->frame;
	event.ts_ns = at;
	event.dir = dir;
	event.session = kClientPort;
	event.participant = 1;
	event.full_tag = identity;
	event.material = true;
	event.body = std::move(body);
	event.length = event.body.size();
	trace.events.insert(at_or_after, std::move(event));
}

// E2's round end and rotation in retail's order (x2's tdh-i3a-tdm-round RR joiner
// capture: the zero 0x61 seed and the 7-byte 0x1D board in one datagram, the 0x56 the
// board pull answers, the 0x25 after the linger and before the host's load, the joiner's
// reload C2S 0x48 at once, then the next mission's 0x7B at the host's load end, its 0x0B
// and first 0x0A; a retail joiner's first join carries two 0x7B). Gameplay pauses in
// both directions from the round end to the next mission: the round end moves the slot
// to state 7 and the host's 0x0A needs state 6 (Server_ProcessRoundEnd @0x51685E,
// Server_SendEntityStateToPlayer @0x517BD2); the joiner's 0x1D raises the round-over
// gate that holds its 0x0C (@0x430858, @0x42C467). The steady gap holding the round
// end's board is that pause; a gap that does not hold it, or the same gap without a
// declared rotation, is STEADY_GAMEPLAY_GAP.
constexpr uint64_t kRoundEnd = 10200 * kMs;
constexpr uint64_t kNextGameplay = 13 * kSec;

struct Rotation {
	bool zero_seed = true;
	uint64_t pause_from = kRoundEnd;     // the gameplay pause's first moment
	uint64_t reset_at = 11 * kSec;       // the 0x25 (retail: before the host's load)
	uint64_t reload_at = 11150 * kMs;    // the joiner's reload 0x48, answering the 0x25
	uint64_t announce_at = 11300 * kMs;  // the host's load-end 0x7B
	uint64_t resume_at = kNextGameplay;  // the next mission's first gameplay
};

void test_rotation_load_gap() {
	auto rotate = [](wire::ParityTrace trace, Rotation r = {}) {
		std::vector<wire::ParityFrameState> frames;
		for (const wire::ParityFrameState &frame : trace.frames)
			if (frame.ts_ns < r.pause_from || frame.ts_ns >= r.resume_at)
				frames.push_back(frame);
		trace.frames = std::move(frames);
		std::vector<wire::ParityUplinkState> uplinks;
		for (const wire::ParityUplinkState &uplink : trace.uplinks)
			if (uplink.ts_ns < r.pause_from || uplink.ts_ns >= r.resume_at)
				uplinks.push_back(uplink);
		trace.uplinks = std::move(uplinks);
		splice(trace, 'S', 0x07B, 1290 * kMs, text_bytes("Untitled "));
		splice(trace, 'S', 0x061, kRoundEnd,
				r.zero_seed ? bytes({ 0, 0, 0, 0 }) : bytes({ 0, 0, 0xE5, 0 }));
		splice(trace, 'S', 0x01D, kRoundEnd, bytes({ 0, 0, 0, 0, 0, 1, 1 }));
		splice(trace, 'S', 0x056, 10400 * kMs, bytes({ 0x9E, 0, 0, 0, 0x15, 1, 2 }));
		splice(trace, 'S', 0x025, r.reset_at, {});
		splice(trace, 'C', 0x048, r.reload_at, bytes({ 3, 0, 0, 0 }));
		splice(trace, 'S', 0x07B, r.announce_at, text_bytes("Untitled "));
		splice(trace, 'S', 0x00B, r.announce_at + 200 * kMs, bytes({ 0x42, 0x4D, 0x53, 0x0C }));
		return trace;
	};
	auto gap = [](const Case &c) {
		for (const EvidenceRow &row : c.verdict.evidence)
			if (row.code == "STEADY_GAMEPLAY_GAP")
				return true;
		return false;
	};
	auto milestones = [](const Case &c) {
		const VerdictRow *row = find_row(c.verdict.rows, 'B', "milestones", "");
		return row ? row->value.subject : std::string();
	};
	auto lingers_short = [](const RunFacts &run) {
		for (const EvidenceFinding &finding : run.evidence)
			if (finding.code == "ROUND_LINGER_SHORT")
				return true;
		return false;
	};

	const Case declared = judge_case(rotate(scripted(1)), rotate(scripted(2)), rotate(scripted(3)),
			"RO", true);
	check(!gap(declared), "the round-end pause of a declared rotation is no STEADY_GAMEPLAY_GAP");
	expect_row(declared, 'B', "milestones", "", "pass");
	expect_row(declared, 'B', "milestone_order", "", "pass");
	const std::string order = milestones(declared);
	size_t at = 0;
	for (const char *name : { "S:0x61 round", "S:0x1d round", "S:0x56 round", "S:0x25 reset",
				 "C:0x48 next", "S:0x7b next", "S:0x0b next", "S:0x0a next" }) {
		const size_t found = order.find(name, at);
		check(found != std::string::npos, std::string(name) + " follows in retail's order: " + order);
		if (found != std::string::npos)
			at = found;
	}
	check(order.find("0x5d") == std::string::npos, "no 0x5D belongs to the round end");

	const Case seeded = judge_case(rotate(scripted(1), { false }), rotate(scripted(2), { false }),
			rotate(scripted(3), { false }), "RO", true);
	check(milestones(seeded).find("S:0x61 round") == std::string::npos,
			"a nonzero seed ahead of the board is no round-end 0x61");

	// A 0x25 held until the host's load end rides the 0x7B's datagram, and the joiner's
	// 0x48 follows both: the inversion milestone_order keeps, not normalizes.
	const Rotation ride{ true, kRoundEnd, 11300 * kMs, 11400 * kMs, 11300 * kMs };
	const Case late_reset = judge_case(rotate(scripted(1)), rotate(scripted(2)),
			rotate(scripted(3), ride), "OR", true);
	expect_row(late_reset, 'B', "milestones", "", "pass");
	expect_row(late_reset, 'B', "milestone_order", "", "fail", "MILESTONE_ORDER");
	// An A/A presence difference (RR-aa's window cut before its next mission's first
	// 0x0A) widens the presence row only: the same inversion still fails.
	wire::ParityTrace cut_aa = rotate(scripted(2));
	{
		std::vector<wire::ParityEvent> events;
		for (wire::ParityEvent &event : cut_aa.events)
			if (!(event.dir == 'S' && event.full_tag == 0x00A && event.ts_ns > kRoundEnd))
				events.push_back(std::move(event));
		cut_aa.events = std::move(events);
	}
	const Case presence_aa = judge_case(rotate(scripted(1)), cut_aa, rotate(scripted(3), ride),
			"OR", true);
	const VerdictRow *aa_presence = find_row(presence_aa.verdict.aa, 'B', "milestones", "");
	check(aa_presence && aa_presence->value.distance >= 1.0,
			"RR-aa lacks the next mission's first 0x0A (an A/A presence difference)");
	expect_row(presence_aa, 'B', "milestones", "", "pass");
	expect_row(presence_aa, 'B', "milestone_order", "", "fail", "MILESTONE_ORDER");

	// A stall of ours that ends in the round end is no pause: its first sample lies
	// 2.2 s before the board.
	const Case stall = judge_case(rotate(scripted(1)), rotate(scripted(2)),
			rotate(scripted(3), { true, 8 * kSec }), "RO", true);
	check(gap(stall), "a stall ending in the round end's board is a STEADY_GAMEPLAY_GAP");

	// A second round end in a long window is excused the same way.
	auto again = [](wire::ParityTrace trace, bool board) {
		constexpr uint64_t kSecondEnd = 16 * kSec, kSecondNext = 18500 * kMs;
		std::vector<wire::ParityFrameState> frames;
		for (const wire::ParityFrameState &frame : trace.frames)
			if (frame.ts_ns < kSecondEnd || frame.ts_ns >= kSecondNext)
				frames.push_back(frame);
		trace.frames = std::move(frames);
		std::vector<wire::ParityUplinkState> uplinks;
		for (const wire::ParityUplinkState &uplink : trace.uplinks)
			if (uplink.ts_ns < kSecondEnd || uplink.ts_ns >= kSecondNext)
				uplinks.push_back(uplink);
		trace.uplinks = std::move(uplinks);
		if (board)
			splice(trace, 'S', 0x01D, kSecondEnd, bytes({ 0, 0, 0, 0, 0, 1, 1 }));
		return trace;
	};
	const Case twice = judge_case(again(rotate(scripted(1)), true), again(rotate(scripted(2)), true),
			again(rotate(scripted(3)), true), "RO", true);
	check(!gap(twice), "a second round end's pause is no STEADY_GAMEPLAY_GAP");
	const Case hole = judge_case(again(rotate(scripted(1)), true), again(rotate(scripted(2)), true),
			again(rotate(scripted(3)), false), "RO", true);
	check(gap(hole), "the same hole without its board is a STEADY_GAMEPLAY_GAP");

	// The 0x25 follows its board by the 2790-tick linger (44.64 s) at least; these
	// scripted rotations compress it, so each run reports it short.
	check(lingers_short(declared.subject) && lingers_short(declared.rr),
			"a 0x25 0.8 s after its board is ROUND_LINGER_SHORT");
	auto lingered = [](uint64_t linger) {
		RunInfo info = info_for("linger", "RO");
		info.rotation = true;
		info.steady_end_ns = 60 * kSec;
		info.steady_completed_ns = info.steady_end_ns + 500 * kMs;
		info.steady_seconds = double(info.steady_end_ns - info.steady_start_ns) / 1e9;
		wire::ParityTrace trace = scripted(3);
		splice(trace, 'S', 0x01D, kRoundEnd, bytes({ 0, 0, 0, 0, 0, 1, 1 }));
		splice(trace, 'S', 0x025, kRoundEnd + linger, {});
		return make_run_facts(info, std::move(trace));
	};
	check(!lingers_short(lingered(44700 * kMs)), "a 0x25 44.7 s after its board is no ROUND_LINGER_SHORT");
	check(lingers_short(lingered(30 * kSec)), "a 0x25 30 s after its board is ROUND_LINGER_SHORT");

	const Case resumed = judge_case(rotate(scripted(1)), rotate(scripted(2)),
			rotate(scripted(3), { true, 11 * kSec }), "RO", true);
	check(gap(resumed), "gameplay after the round end leaves a later gap a STEADY_GAMEPLAY_GAP");

	const Case undeclared = judge_case(rotate(scripted(1)), rotate(scripted(2)), rotate(scripted(3)));
	check(gap(undeclared), "the same gap without a declared rotation is STEADY_GAMEPLAY_GAP");

	// A window that ends inside the pause (the host still loading at the steady end):
	// its tail is the pause, and the legs past the steady end are the capture's tail.
	const Rotation tail{ true, kRoundEnd, 11 * kSec, 11150 * kMs, 20500 * kMs, 30 * kSec };
	const Case cut = judge_case(rotate(scripted(1), tail), rotate(scripted(2), tail),
			rotate(scripted(3), tail), "RO", true);
	check(!gap(cut), "a window ending inside the round-end pause has no STEADY_GAMEPLAY_GAP");
	for (const EvidenceRow &row : cut.verdict.evidence)
		check(row.code != "STEADY_BOUNDARY_GAMEPLAY_MISSING",
				"the pause holds the window end: " + row.run + " " + row.detail);
	const std::string cut_order = milestones(cut);
	check(cut_order.find("C:0x48 next") != std::string::npos &&
					cut_order.find("S:0x7b next") == std::string::npos,
			"milestones count through the steady end only: " + cut_order);
}

// A dead joiner sends no C2S 0x0C (window.dead_joiner_uplink): a gap or a window
// edge that a frame reporting the joiner's own health 0 falls in is its death; the
// same hole while the frames report it alive is STEADY_GAMEPLAY_GAP.
void test_dead_joiner_uplink() {
	auto die = [](wire::ParityTrace trace, uint64_t from, uint64_t until, bool report_dead) {
		std::vector<wire::ParityUplinkState> kept;
		for (const wire::ParityUplinkState &u : trace.uplinks)
			if (u.ts_ns < from || u.ts_ns >= until)
				kept.push_back(u);
		trace.uplinks = std::move(kept);
		if (report_dead)
			for (wire::ParityFrameState &frame : trace.frames)
				if (frame.ts_ns >= from + 50 * kMs && frame.ts_ns < until)
					frame.health = 0;
		return trace;
	};
	auto coverage = [](const Case &c) {
		for (const EvidenceRow &row : c.verdict.evidence)
			if (row.code == "STEADY_GAMEPLAY_GAP" || row.code == "STEADY_BOUNDARY_GAMEPLAY_MISSING")
				return true;
		return false;
	};
	const uint64_t end = kSteadyStart + uint64_t(kSteadySeconds * 1e9) + kSec;
	// Dead for the window's rest (a self nade), and dead then respawned.
	for (const auto &[from, until] : { std::pair{ 12 * kSec, end }, std::pair{ 9 * kSec, 13 * kSec } }) {
		const Case dead = judge_case(die(scripted(1), from, until, true),
				die(scripted(2), from, until, true), die(scripted(3), from, until, true));
		check(!coverage(dead), "a dead joiner's missing uplink is no coverage failure");
		const Case alive = judge_case(die(scripted(1), from, until, true),
				die(scripted(2), from, until, true), die(scripted(3), from, until, false));
		check(coverage(alive), "the same hole while the joiner lives is a coverage failure");
	}
}

// A scenario whose own play ends the joiner's session (window.session_end, E4's
// kick): the host's punt reaches the joiner as the S2C 0x103 description record and
// the connection tears down at once, so no gameplay follows. A run that declares it
// ends its coverage at the record; the same silence without the declaration, or a
// declared run with no record, is a coverage failure.
void test_session_end() {
	static constexpr uint64_t kPunt = 8 * kSec;
	auto punt = [](wire::ParityTrace trace, bool record) {
		auto drop = [](auto &records, auto gone) {
			records.erase(std::remove_if(records.begin(), records.end(), gone), records.end());
		};
		drop(trace.uplinks, [](const wire::ParityUplinkState &u) { return u.ts_ns > kPunt; });
		drop(trace.frames, [](const wire::ParityFrameState &f) { return f.ts_ns > kPunt; });
		drop(trace.events, [](const wire::ParityEvent &e) {
			return e.ts_ns > kPunt && (e.full_tag == 0x00C || e.full_tag == 0x00A);
		});
		if (record) {
			splice(trace, 'S', 0x103, kPunt + 90 * kMs, bytes({ 0x44, 0x53, 0, 4, 0, 1, 0, 0, 0 }));
			for_events(trace, 'S', 0x103, [](wire::ParityEvent &e) { e.settings_update = true; });
		}
		return trace;
	};
	auto coverage = [](const Case &c) {
		for (const EvidenceRow &row : c.verdict.evidence)
			if (row.code == "STEADY_GAMEPLAY_GAP" || row.code == "STEADY_BOUNDARY_GAMEPLAY_MISSING")
				return true;
		return false;
	};
	const Case declared = judge_case(punt(scripted(1), true), punt(scripted(2), true),
			punt(scripted(3), true), "RO", false, true);
	check(!coverage(declared), "a declared punt's silence after its record is no coverage failure");
	const Case undeclared = judge_case(punt(scripted(1), true), punt(scripted(2), true),
			punt(scripted(3), true));
	check(coverage(undeclared), "the same silence without the declaration is a coverage failure");
	const Case unrecorded = judge_case(punt(scripted(1), false), punt(scripted(2), false),
			punt(scripted(3), false), "RO", false, true);
	check(coverage(unrecorded), "a declared run whose capture holds no record keeps its window");

	// Its rates count the covered play over that play's length: a punt landing 1.5 s
	// sooner (x5's kick covered 1.08 to 1.29 s) reads the same gameplay cadence.
	auto punt_at = [](wire::ParityTrace trace, uint64_t at) {
		auto drop = [](auto &records, auto gone) {
			records.erase(std::remove_if(records.begin(), records.end(), gone), records.end());
		};
		drop(trace.uplinks, [at](const wire::ParityUplinkState &u) { return u.ts_ns > at; });
		drop(trace.frames, [at](const wire::ParityFrameState &f) { return f.ts_ns > at; });
		drop(trace.events, [at](const wire::ParityEvent &e) {
			return e.ts_ns > at && (e.full_tag == 0x00C || e.full_tag == 0x00A);
		});
		splice(trace, 'S', 0x103, at + 90 * kMs, bytes({ 0x44, 0x53, 0, 4, 0, 1, 0, 0, 0 }));
		for_events(trace, 'S', 0x103, [](wire::ParityEvent &e) { e.settings_update = true; });
		return trace;
	};
	const Case sooner = judge_case(punt_at(scripted(1), kPunt), punt_at(scripted(2), kPunt),
			punt_at(scripted(3), kPunt - 1500 * kMs), "RO", false, true);
	expect_row(sooner, 'S', "gameplay_cadence", "", "pass");
	expect_row(sooner, 'C', "gameplay_cadence", "", "pass");
	const Case undeclared_sooner = judge_case(punt_at(scripted(1), kPunt),
			punt_at(scripted(2), kPunt), punt_at(scripted(3), kPunt - 1500 * kMs));
	expect_row(undeclared_sooner, 'S', "gameplay_cadence", "", "fail", "GAMEPLAY_CADENCE");

	// A stream's count runs from each run's join to its own punt over that span: a punt
	// a readiness wait later (x6's kick: 15.26 s from the join on RR, 14.36 s on RR-aa,
	// 16.82 s on OO) carries more of the same stream, not a faster one.
	const Case later = judge_case(punt_at(scripted(1), kPunt), punt_at(scripted(2), kPunt),
			punt_at(scripted(3), kPunt + 1500 * kMs), "RO", false, true);
	expect_row(later, 'C', "tag_count", "0x0c:0", "pass");
	const Case undeclared_later = judge_case(punt_at(scripted(1), kPunt),
			punt_at(scripted(2), kPunt), punt_at(scripted(3), kPunt + 1500 * kMs));
	expect_row(undeclared_later, 'C', "tag_count", "0x0c:0", "fail", "COUNT_MISMATCH");
	// The same span sending the stream twice as often still fails.
	wire::ParityTrace doubled = punt_at(scripted(3), kPunt);
	std::vector<wire::ParityEvent> events;
	for (const wire::ParityEvent &event : doubled.events) {
		events.push_back(event);
		if (event.dir == 'C' && event.full_tag == 0x00C)
			events.push_back(event);
	}
	doubled.events = std::move(events);
	const Case faster = judge_case(punt_at(scripted(1), kPunt), punt_at(scripted(2), kPunt),
			std::move(doubled), "RO", false, true);
	expect_row(faster, 'C', "tag_count", "0x0c:0", "fail", "COUNT_MISMATCH");
}

void test_aa_widening() {
	// RR-aa itself carries half of RR's S2C 0x2C: retail's noise. A subject as far
	// off passes, against the widened floor; the aa row reports the widening.
	wire::ParityTrace aa = scripted(2), subject = scripted(3);
	erase_events(aa, 'S', 0x02C, [](size_t i) { return i % 2 == 1; });
	erase_events(subject, 'S', 0x02C, [](size_t i) { return i % 2 == 1; });
	const Case c = judge_case(scripted(1), aa, subject);
	expect_row(c, 'S', "tag_count", "0x2c:0", "pass");
	const VerdictRow *row = find_row(c.verdict.rows, 'S', "tag_count", "0x2c:0");
	check(row && row->threshold > row->value.floor, "the floor widened to the A/A distance");
	const VerdictRow *aa_row = find_row(c.verdict.aa, 'S', "tag_count", "0x2c:0");
	check(aa_row && aa_row->status == "widened", "the aa sanity row reports the widening");

	// A presence RR-aa lacks too is noise; one RR-aa keeps is not.
	wire::ParityTrace aa2 = scripted(2), missing = scripted(3);
	erase_events(aa2, 'S', 0x066);
	erase_events(missing, 'S', 0x066);
	erase_events(missing, 'S', 0x076);
	const Case p = judge_case(scripted(1), aa2, missing);
	expect_row(p, 'S', "tag_presence", "0x66:0", "pass");
	expect_row(p, 'S', "tag_presence", "0x76:0", "fail", "MISSING_TAG");
}

void test_initial_stream() {
	{
		wire::ParityTrace subject = scripted(3);
		for (wire::ParityPacket &p : subject.packets)
			if (p.dir == 'S' && !p.tags.empty() && p.tags[0] == 0x008)
				p.flags = 0x03;
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "initial_grouping", "", "fail", "INITIAL_PACKET_GROUPING");
	}
	{
		wire::ParityTrace subject = scripted(3);
		for_events(subject, 'S', 0x011, [](wire::ParityEvent &e) { e.body[0] = 9; });
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "initial_body", "0x11:0", "fail", "INITIAL_BODY_MISMATCH");
	}
}

void test_steady_and_envelope() {
	{
		// Every steady S2C 0x0A packet regrouped with an extra record.
		wire::ParityTrace subject = scripted(3);
		for (wire::ParityPacket &p : subject.packets)
			if (p.dir == 'S' && p.tags.size() == 1 && p.tags[0] == 0x00A) {
				p.tags.push_back(0x040);
				p.records.push_back(p.records.front());
				p.records.back().full_tag = 0x040;
			}
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "steady_group_distribution", "", "fail", "GAMEPLAY_GROUP_DISTRIBUTION");
		expect_row(c, 'S', "steady_order_distribution", "", "fail", "GAMEPLAY_ORDER_DISTRIBUTION");
	}
	{
		// A handful regrouped stays under the 2% share.
		wire::ParityTrace subject = scripted(3);
		int changed = 0;
		for (wire::ParityPacket &p : subject.packets)
			if (p.dir == 'S' && p.tags.size() == 1 && p.tags[0] == 0x00A &&
					p.ts_ns > kSteadyStart && changed < 3) {
				p.flags = 0x05;
				++changed;
			}
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "steady_group_distribution", "", "pass");
	}
	{
		// No header-only ACK at all: a structural absence and a cadence miss.
		wire::ParityTrace subject = scripted(3);
		std::set<int> dropped;
		std::vector<wire::ParityPacket> kept;
		for (wire::ParityPacket &p : subject.packets) {
			if (p.dir == 'C' && p.records.empty())
				dropped.insert(p.frame);
			else
				kept.push_back(std::move(p));
		}
		subject.packets = std::move(kept);
		std::vector<wire::ParityDatagram> datagrams;
		for (const wire::ParityDatagram &d : subject.datagrams)
			if (!dropped.count(d.frame))
				datagrams.push_back(d);
		subject.datagrams = std::move(datagrams);
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(
				c, 'C', "packet_class_presence", "header_only", "fail", "SESSION_PACKET_CADENCE");
		expect_row(c, 'C', "packet_cadence", "header_only", "fail", "SESSION_PACKET_CADENCE");
		expect_row(c, 'C', "header_only_flags", "", "fail", "SESSION_PACKET_HEADER_FLAGS");
		expect_row(c, 'C', "packet_shape_distribution", "", "fail",
				"SESSION_PACKET_SHAPE_DISTRIBUTION");
	}
}

void test_handshake() {
	{
		wire::ParityTrace subject = scripted(3);
		std::vector<uint8_t> b;
		tlv_text(b, "NVS", "NAPI NP Version 0.0.1");
		tlv_text(b, "PN", "NOVAWORLDUDP");
		tlv_u32(b, "CI", 33);
		tlv_u32(b, "EIP", 0x0100007F);
		tlv_u32(b, "EPN", 30003);
		subject.handshakes[0].body = b;
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'C', "handshake", "0x41", "fail", "HANDSHAKE_MISMATCH");
	}
	{
		wire::ParityTrace subject = scripted(3);
		subject.handshakes.erase(subject.handshakes.begin() + 3); // no 0x82
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "handshake", "0x82", "fail", "HANDSHAKE_MISSING");
	}
}

void test_integrity() {
	{
		wire::ParityTrace subject = scripted(3);
		for_events(subject, 'C', 0x021, [](wire::ParityEvent &e) { e.body[2] ^= 0x10; });
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'B', "integrity_crc", "ammo", "fail", "INTEGRITY_CRC_MISMATCH");
		expect_row(c, 'B', "integrity_crc", "entity", "pass");
	}
	{
		wire::ParityTrace subject = scripted(3);
		erase_events(subject, 'C', 0x020, [](size_t i) { return i == 0; });
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'B', "integrity_pairs", "entity", "fail", "ENTITY_INTEGRITY_PAIR_INVALID");
	}
	{
		wire::ParityTrace subject = scripted(3);
		wire::ParityEvent punt = subject.events.back();
		punt.dir = 'S';
		punt.full_tag = 0x103;
		punt.settings_update = true;
		punt.material = true;
		punt.body = text_bytes("PUNT ACRC ParityJoin");
		punt.length = punt.body.size();
		subject.events.push_back(punt);
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'B', "integrity_punt", "", "fail", "INTEGRITY_PUNT");
	}
	{
		// The host challenges another ammo row. A retail host that punts nothing is
		// the oracle for it; an OpenNova host's row is unwitnessed and off RR's set.
		wire::ParityTrace subject = scripted(3);
		for_events(subject, 'S', 0x031, [](wire::ParityEvent &e) { e.body[0] = 0x19; });
		for_events(subject, 'C', 0x021, [](wire::ParityEvent &e) { e.body[0] = 0x19; });
		const Case ro = judge_case(scripted(1), scripted(2), subject, "RO");
		expect_row(ro, 'B', "integrity_row_unwitnessed", "ammo", "pass");
		check(find_row(ro.verdict.rows, 'B', "integrity_rows", "ammo") == nullptr,
				"a retail host's rows are not the subject's");
		const Case oo = judge_case(scripted(1), scripted(2), subject, "OO");
		expect_row(
				oo, 'B', "integrity_row_unwitnessed", "ammo", "fail", "INTEGRITY_ROW_UNWITNESSED");
		expect_row(oo, 'B', "integrity_rows", "ammo", "fail", "HOST_AMMO_ROW_MISMATCH");

		// A retail host that punted is no oracle.
		wire::ParityEvent punt = subject.events.back();
		punt.dir = 'S';
		punt.full_tag = 0x103;
		punt.settings_update = true;
		punt.material = true;
		punt.body = text_bytes("PUNT ACRC ParityJoin");
		punt.length = punt.body.size();
		subject.events.push_back(punt);
		const Case punted = judge_case(scripted(1), scripted(2), subject, "RO");
		expect_row(punted, 'B', "integrity_row_unwitnessed", "ammo", "fail",
				"INTEGRITY_ROW_UNWITNESSED");
	}
	{
		// RR-aa's rows of the same integrity profile join the oracle: a row RR never
		// challenged is witnessed by RR-aa's reply, and a CRC off it still fails.
		wire::ParityTrace aa = scripted(2), subject = scripted(3), wrong = scripted(3);
		for (wire::ParityTrace *trace : { &aa, &subject, &wrong }) {
			for_events(*trace, 'S', 0x031, [](wire::ParityEvent &e) { e.body[0] = 0x19; });
			for_events(*trace, 'C', 0x021, [](wire::ParityEvent &e) { e.body[0] = 0x19; });
		}
		for_events(wrong, 'C', 0x021, [](wire::ParityEvent &e) { e.body[2] ^= 0x10; });
		const Case pooled = judge_case(scripted(1), aa, subject, "OO");
		expect_row(pooled, 'B', "integrity_row_unwitnessed", "ammo", "pass");
		expect_row(pooled, 'B', "integrity_crc", "ammo", "pass");
		const Case off = judge_case(scripted(1), aa, wrong, "OO");
		expect_row(off, 'B', "integrity_crc", "ammo", "fail", "INTEGRITY_CRC_MISMATCH");

		Case other_profile;
		other_profile.rr = make_run_facts(info_for("rr", "RR"), scripted(1));
		RunInfo aa_info = info_for("rr_aa", "RR");
		aa_info.integrity_profile = "another-corpus";
		other_profile.aa = make_run_facts(aa_info, aa);
		other_profile.subject = make_run_facts(info_for("subject", "OO"), subject);
		other_profile.verdict =
				judge(other_profile.rr, other_profile.aa, { { "OO", &other_profile.subject } });
		expect_row(other_profile, 'B', "integrity_row_unwitnessed", "ammo", "fail",
				"INTEGRITY_ROW_UNWITNESSED");
	}
	{
		wire::ParityTrace subject = scripted(3);
		for (wire::ParityPacket &p : subject.packets)
			if (p.tags.size() == 2 && p.tags[0] == 0x016)
				std::swap(p.tags[0], p.tags[1]);
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'B', "integrity_scoreboard", "", "fail", "INTEGRITY_SCOREBOARD_ORDER");
	}
	{
		wire::ParityTrace subject = scripted(3);
		for_events(subject, 'S', 0x030, [](wire::ParityEvent &e) { e.full_tag = 0x031; });
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'B', "integrity_alternation", "", "fail", "INTEGRITY_NOT_ALTERNATING");
	}
	{
		// The family is one toggle flipped at every boundary, a slot challenged only
		// while it plays (integrity.family_toggle): a boundary the joiner sat out
		// (dead) flips it unseen, so two challenges two boundaries apart share a family;
		// a host that flipped only on its sends would not.
		wire::ParityTrace skipped = scripted(3);
		erase_events(skipped, 'S', 0x030);
		expect_row(judge_case(scripted(1), scripted(2), skipped), 'B', "integrity_alternation",
				"", "pass");
		wire::ParityTrace per_send = scripted(3);
		erase_events(per_send, 'S', 0x030);
		for_events(per_send, 'S', 0x031, [](wire::ParityEvent &e) {
			if (e.ts_ns > 15 * kSec && e.ts_ns < 17 * kSec)
				e.full_tag = 0x030;
		});
		expect_row(judge_case(scripted(1), scripted(2), per_send), 'B', "integrity_alternation",
				"", "fail", "INTEGRITY_NOT_ALTERNATING");
		// A mission change (its S2C 0x25) ends the count: the next mission's first
		// family is free (x6's round RO retail host repeated across it).
		auto across = [](bool reset) {
			wire::ParityTrace trace = scripted(3);
			for_events(trace, 'S', 0x030, [](wire::ParityEvent &e) {
				if (e.ts_ns < 12 * kSec)
					e.full_tag = 0x031;
			});
			for_events(trace, 'S', 0x031, [](wire::ParityEvent &e) {
				if (e.ts_ns > 15 * kSec && e.ts_ns < 17 * kSec)
					e.full_tag = 0x030;
			});
			if (reset)
				splice(trace, 'S', 0x025, 8 * kSec, {});
			return trace;
		};
		expect_row(judge_case(scripted(1), scripted(2), across(true)), 'B',
				"integrity_alternation", "", "pass");
		expect_row(judge_case(scripted(1), scripted(2), across(false)), 'B',
				"integrity_alternation", "", "fail", "INTEGRITY_NOT_ALTERNATING");
	}
	{
		// An ammo challenge between the join and the first uplink carries the class-init
		// ADM's row (integrity.join_adm): the host's extra row there is the join's phase
		// (x6's ash-deploy RO retail host: row 14 at join + 0.53 s); after the first
		// uplink the same row is a host divergence.
		auto extra_row = [](uint64_t at) {
			wire::ParityTrace trace = scripted(3);
			splice(trace, 'S', 0x031, at, bytes({ 0x0E, 0x34, 0x12 }));
			std::vector<uint8_t> reply{ 0x0E };
			put_u32(reply, 0xCAFEF00Du ^ 0x1234u);
			put_u32(reply, 0x1234u);
			splice(trace, 'C', 0x021, at + 20 * kMs, reply);
			return trace;
		};
		const Case before = judge_case(scripted(1), scripted(2), extra_row(1600 * kMs), "OO");
		expect_row(before, 'B', "integrity_rows", "ammo", "pass");
		expect_row(before, 'B', "integrity_row_unwitnessed", "ammo", "pass");
		const Case after = judge_case(scripted(1), scripted(2), extra_row(3 * kSec), "OO");
		expect_row(after, 'B', "integrity_rows", "ammo", "fail", "HOST_AMMO_ROW_MISMATCH");
	}
	{
		// An ammo challenge carries the row of the ADM the uplinks name (integrity.held_adm):
		// x6's selfnade holds the grenade (ADM 52, row 54) 6.5 s, under the two boundaries
		// an ammo challenge needs, so RR's row 54 there against a run whose ammo boundary
		// fell before the select is the stay's phase. A stay of two boundaries or more
		// compares.
		auto grenade = [](uint32_t seed, uint64_t from, uint64_t until, bool challenged) {
			wire::ParityTrace trace = scripted(seed);
			for (wire::ParityUplinkState &u : trace.uplinks)
				if (u.ts_ns >= from && u.ts_ns < until)
					u.adm = 52;
			if (challenged) {
				for_events(trace, 'S', 0x031, [from, until](wire::ParityEvent &e) {
					if (e.ts_ns >= from && e.ts_ns < until)
						e.body[0] = 0x36;
				});
				for_events(trace, 'C', 0x021, [from, until](wire::ParityEvent &e) {
					if (e.ts_ns >= from && e.ts_ns < until + 20 * kMs)
						e.body[0] = 0x36;
				});
			}
			return trace;
		};
		// The 16 s ammo challenge: inside RR's 14..17 s stay, before the run's 16.5..19.5 s.
		const Case short_stay = judge_case(grenade(1, 14 * kSec, 17 * kSec, true),
				grenade(2, 14 * kSec, 17 * kSec, true), grenade(3, 16500 * kMs, 19500 * kMs, false),
				"OR");
		expect_row(short_stay, 'B', "integrity_rows", "ammo", "pass");
		expect_row(short_stay, 'B', "integrity_row_unwitnessed", "ammo", "pass");
		// Held 8..20 s (over two boundaries) on both sides: the row must be there.
		const Case long_stay = judge_case(grenade(1, 8 * kSec, 21 * kSec, true),
				grenade(2, 8 * kSec, 21 * kSec, true), grenade(3, 8 * kSec, 21 * kSec, false),
				"OR");
		expect_row(long_stay, 'B', "integrity_rows", "ammo", "fail", "HOST_AMMO_ROW_MISMATCH");
	}
}

// A rotation's joiner joins each mission, and a stream that starts in a later mission
// counts from that mission's join (window.tag_stream_anchor): round's next-mission S2C
// 0x6E opens at the joiner's second C2S 0x0B (x8 RR 0.9 s, OO 0.2 s after it), and OO's
// second join came 10.8 s after RR's (our host's map-change load), so counted from the
// first join OO read 30 against 37 where the stream itself runs at RR's rate.
void test_stream_anchor_rotation() {
	auto rotation = [](uint32_t seed, uint64_t second_join) {
		wire::ParityTrace trace = scripted(seed);
		splice(trace, 'C', 0x00B, second_join, bytes({ 1 }));
		for (uint64_t ts = second_join + 500 * kMs; ts <= 20 * kSec; ts += 500 * kMs)
			splice(trace, 'S', 0x06E, ts, bytes({ 0 }));
		return trace;
	};
	const Case c = judge_case(rotation(1, 6 * kSec), rotation(2, 6 * kSec), rotation(3, 10 * kSec));
	expect_row(c, 'S', "tag_count", "0x6e:0", "pass");
	// The same stream opening 4 s after its own mission's join still counts short.
	wire::ParityTrace slow = scripted(3);
	splice(slow, 'C', 0x00B, 6 * kSec, bytes({ 1 }));
	for (uint64_t ts = 10 * kSec; ts <= 20 * kSec; ts += 500 * kMs)
		splice(slow, 'S', 0x06E, ts, bytes({ 0 }));
	const Case late = judge_case(rotation(1, 6 * kSec), rotation(2, 6 * kSec), slow);
	expect_row(late, 'S', "tag_count", "0x6e:0", "fail", "COUNT_MISMATCH");
}

// The auto-reload loop re-sends until the first echo lands (count.reload_echo): RR's 2
// and RR-aa's 3 against a subject's 5 are the echo's latency against the loop. A lone
// request passes only when its echo landed inside the longest entry spacing the
// reference's own requests allow: x7's selfnade RR sent 2 then 3 requests in two
// consecutive datagrams 187 ms apart (spacing at most 93 ms) and x7 RO's joiner sent one,
// its echo on the wire 55 ms later; x1's OR retail joiner sent one, echoed in 23 ms.
void test_reload_echo() {
	auto reloads = [](uint32_t seed, int n) {
		wire::ParityTrace trace = scripted(seed);
		for (int i = 0; i < n; ++i) {
			const uint64_t at = 9 * kSec + uint64_t(i) * 190 * kMs;
			splice(trace, 'C', 0x025, at, bytes({ 0x5D, 0x00, 0x45, 0x01 }));
			splice(trace, 'S', 0x049, at + 80 * kMs, bytes({ 0x5D, 0x00, 0x45, 0x01 }));
		}
		return trace;
	};
	const Case more = judge_case(reloads(1, 2), reloads(2, 3), reloads(3, 5));
	expect_row(more, 'C', "tag_count", "0x25:0", "pass");
	expect_row(more, 'S', "tag_count", "0x49:0", "pass");
	// One request per datagram with C datagrams between them bounds no spacing: a lone
	// request still fails.
	const Case lone = judge_case(reloads(1, 2), reloads(2, 3), reloads(3, 1));
	expect_row(lone, 'C', "tag_count", "0x25:0", "fail", "COUNT_MISMATCH");
	expect_row(lone, 'S', "tag_count", "0x49:0", "fail", "COUNT_MISMATCH");

	// The scripted joiner sends a C datagram every tick (32 ms): a loop that put 2 requests
	// in one and 3 in the next entered them at most 16 ms apart.
	static constexpr uint64_t kLoopAt = 2 * kSec + 219 * kTick;
	auto looped = [](uint32_t seed) {
		wire::ParityTrace trace = scripted(seed);
		for (int i = 0; i < 2; ++i)
			splice(trace, 'C', 0x025, kLoopAt, bytes({ 0x5D, 0x00, 0x45, 0x01 }));
		for (int i = 0; i < 3; ++i)
			splice(trace, 'C', 0x025, kLoopAt + kTick, bytes({ 0x5D, 0x00, 0x45, 0x01 }));
		for (int i = 0; i < 5; ++i)
			splice(trace, 'S', 0x049, kLoopAt + 2 * kTick, bytes({ 0x5D, 0x00, 0x45, 0x01 }));
		return trace;
	};
	auto single = [](uint32_t seed, uint64_t echo_after) {
		wire::ParityTrace trace = scripted(seed);
		splice(trace, 'C', 0x025, kLoopAt, bytes({ 0x5D, 0x00, 0x45, 0x01 }));
		splice(trace, 'S', 0x049, kLoopAt + echo_after, bytes({ 0x5D, 0x00, 0x45, 0x01 }));
		return trace;
	};
	const Case fast = judge_case(looped(1), looped(2), single(3, 10 * kMs));
	expect_row(fast, 'C', "tag_count", "0x25:0", "pass");
	expect_row(fast, 'S', "tag_count", "0x49:0", "pass");
	const Case slow = judge_case(looped(1), looped(2), single(3, 25 * kMs));
	expect_row(slow, 'C', "tag_count", "0x25:0", "fail", "COUNT_MISMATCH");
	expect_row(slow, 'S', "tag_count", "0x49:0", "fail", "COUNT_MISMATCH");
}

void test_milestones_and_rates() {
	{
		wire::ParityTrace subject = scripted(3);
		erase_events(subject, 'C', 0x00B);
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'B', "milestones", "", "fail", "MILESTONE_MISMATCH");
	}
	{
		wire::ParityTrace within = scripted(3), beyond = scripted(3);
		for (wire::ParityDatagram &d : within.datagrams)
			if (d.src_port == kServerPort)
				d.length += d.length / 10;
		for (wire::ParityDatagram &d : beyond.datagrams)
			if (d.src_port == kServerPort)
				d.length *= 2;
		expect_row(judge_case(scripted(1), scripted(2), within), 'S', "bandwidth", "", "pass");
		expect_row(judge_case(scripted(1), scripted(2), beyond), 'S', "bandwidth", "", "fail",
				"BANDWIDTH_MISMATCH");
	}
	{
		wire::ParityTrace subject = scripted(3);
		std::vector<wire::ParityFrameState> kept;
		for (size_t i = 0; i < subject.frames.size(); ++i)
			if (i % 2 == 0)
				kept.push_back(subject.frames[i]);
		subject.frames = std::move(kept);
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "gameplay_cadence", "", "fail", "GAMEPLAY_CADENCE");
	}
	{
		// The joiner sends while alive (window.alive_span): killed 4 s into the window on
		// the reference and 7 s on the subject, its uplinks run at one rate.
		auto killed = [](uint32_t seed, uint64_t at) {
			wire::ParityTrace trace = scripted(seed);
			std::vector<wire::ParityUplinkState> alive;
			for (const wire::ParityUplinkState &u : trace.uplinks)
				if (u.ts_ns < at)
					alive.push_back(u);
			trace.uplinks = std::move(alive);
			splice(trace, 'S', 0x013, at, bytes({ 0x00, 0x10, 0x00, 0x00 }));
			return trace;
		};
		const Case c = judge_case(killed(1, 9 * kSec), killed(2, 9 * kSec), killed(3, 12 * kSec));
		expect_row(c, 'C', "gameplay_cadence", "", "pass");
		// A joiner that stops sending while alive still reads short.
		wire::ParityTrace quiet = scripted(3);
		std::vector<wire::ParityUplinkState> early;
		for (const wire::ParityUplinkState &u : quiet.uplinks)
			if (u.ts_ns < 12 * kSec)
				early.push_back(u);
		quiet.uplinks = std::move(early);
		const Case q = judge_case(killed(1, 9 * kSec), killed(2, 9 * kSec), quiet);
		expect_row(q, 'C', "gameplay_cadence", "", "fail", "GAMEPLAY_CADENCE");
	}
}

void test_state() {
	{
		wire::ParityTrace subject = scripted(3);
		for (wire::ParityUplinkState &u : subject.uplinks)
			u.adm = 4;
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'C', "uplink_adm", "", "fail", "UPLINK_LOADOUT_MISMATCH");
	}
	{
		wire::ParityTrace subject = scripted(3);
		for (wire::ParityUplinkState &u : subject.uplinks)
			u.carrier = 0x3001;
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'C', "uplink_carrier", "", "fail", "UPLINK_CARRIER_MISMATCH");
	}
	{
		// The carrier word is the position's frame of reference: one change (a respawn onto a
		// building) in every run passes; a link that flips every few uplinks, as x7's
		// fragrespawn RO / OO joiner did (92 / 101 changes against RR's 1), does not.
		auto respawn = [](uint32_t seed, size_t flip_every) {
			wire::ParityTrace trace = scripted(seed);
			const size_t half = trace.uplinks.size() / 2;
			for (size_t i = half; i < trace.uplinks.size(); ++i) {
				const bool off = flip_every != 0 && (i - half) % flip_every == flip_every - 1;
				trace.uplinks[i].carrier = off ? 0xFFFF : 0x2011;
			}
			return trace;
		};
		const Case once = judge_case(respawn(1, 0), respawn(2, 0), respawn(3, 0));
		expect_row(once, 'C', "uplink_carrier_changes", "", "pass");
		const Case flips = judge_case(respawn(1, 0), respawn(2, 0), respawn(3, 5));
		expect_row(flips, 'C', "uplink_carrier_changes", "", "fail", "UPLINK_CARRIER_SEQUENCE");
	}
	{
		// A step across a carrier change is no motion: each carrier's uplinks are one
		// sequence. x8's fragrespawn RR-aa and OR carried the respawn's jump from world into
		// the hut's frame as their p90 step (3885822 / 3895546) where RR's ranked a motion
		// step (102143), so OR failed on a frame change.
		auto settle = [](uint32_t seed, int hut_x, bool jump_ranks) {
			wire::ParityTrace trace = scripted(seed);
			const size_t half = trace.uplinks.size() / 2;
			for (size_t i = 0; i < trace.uplinks.size(); ++i) {
				wire::ParityUplinkState &u = trace.uplinks[i];
				const bool hut = i >= half;
				const int moved = hut ? std::min(int(i - half), 8) : 0;
				u.carrier = hut ? 0x2011 : 0xFFFF;
				u.pos_x = hut ? int32_t(hut_x * 65536 + moved * 32768) : int32_t(1000 * 65536);
				u.pos_y = hut ? int32_t(-3 * 65536) : int32_t(2000 * 65536);
				u.pos_z = hut ? 65536 : 50 * 65536;
			}
			// One more motion step ahead of the respawn moves the p90's rank off the jump.
			if (!jump_ranks)
				trace.uplinks[half / 2].pos_x += 65536;
			return trace;
		};
		// The subject's jump is one unit longer than RR-aa's, as OR's was: across frames it
		// ranked past the A/A widening.
		const Case c = judge_case(settle(1, 2, false), settle(2, 2, true), settle(3, 1, true));
		expect_row(c, 'C', "uplink_position_step_p90", "", "pass");
		const VerdictRow *aa = find_row(c.verdict.aa, 'C', "uplink_position_step_p90", "");
		check(aa != nullptr && aa->status == "pass", "the frame change widens no A/A");
	}
	{
		// A frame change is no motion either: a joiner standing still across its respawn into
		// the hut's frame keys no uplink motion, as one that stood still in the world.
		auto still = [](uint32_t seed, bool hut_half) {
			wire::ParityTrace trace = scripted(seed);
			const size_t half = trace.uplinks.size() / 2;
			for (size_t i = 0; i < trace.uplinks.size(); ++i) {
				wire::ParityUplinkState &u = trace.uplinks[i];
				const bool hut = hut_half && i >= half;
				u.carrier = hut ? 0x2011 : 0xFFFF;
				u.pos_x = hut ? int32_t(2 * 65536) : int32_t(1000 * 65536);
				u.pos_y = hut ? int32_t(-3 * 65536) : int32_t(2000 * 65536);
				u.pos_z = hut ? 65536 : 50 * 65536;
			}
			return trace;
		};
		expect_row(judge_case(still(1, true), still(2, true), still(3, false)), 'C',
				"uplink_motion", "", "pass");
	}
	{
		// Session-local handles: another carrier/priority handle number is the same state.
		wire::ParityTrace subject = scripted(3);
		for (wire::ParityUplinkState &u : subject.uplinks)
			u.priority_handle[0] = 0x2007;
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'C', "uplink_priority", "", "pass");
	}
	{
		wire::ParityTrace within = scripted(3), beyond = scripted(3);
		for (wire::ParityUplinkState &u : within.uplinks)
			u.pos_x = 1000 * 65536 + (u.pos_x - 1000 * 65536) * 2;
		for (wire::ParityUplinkState &u : beyond.uplinks)
			u.pos_x = int32_t(1000 * 65536 + int64_t(u.pos_x - 1000 * 65536) * 20);
		expect_row(judge_case(scripted(1), scripted(2), within), 'C', "uplink_position_range_p90",
				"", "pass");
		expect_row(judge_case(scripted(1), scripted(2), beyond), 'C', "uplink_position_range_p90",
				"", "fail", "DYNAMIC_RANGE_MISMATCH");
	}
	{
		wire::ParityTrace subject = scripted(3);
		for (wire::ParityUplinkState &u : subject.uplinks)
			u.pos_y += 200 * 65536;
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'C', "uplink_position_baseline", "", "fail", "DYNAMIC_BASELINE_MISMATCH");
	}
	{
		wire::ParityTrace subject = scripted(3);
		for (wire::ParityFrameState &f : subject.frames)
			if (f.sub_block == 3) {
				f.sub_block = 0;
				f.objective_present = false;
				f.objective[0] = 0;
				f.weapon_present = true;
			}
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "frame_subblock", "", "fail", "FRAME_SUBBLOCK_MISMATCH");
		expect_row(c, 'S', "frame_objective", "", "fail", "FRAME_OBJECTIVE_PHASE_MISMATCH");
	}
	{
		// The time of day and the timer seconds advance with the clock: masked.
		wire::ParityTrace subject = scripted(3);
		for (wire::ParityFrameState &f : subject.frames) {
			f.env[2] += 999;
			f.timer_seconds -= 50;
		}
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "frame_env", "", "pass");
		expect_row(c, 'S', "frame_timer", "", "pass");
	}
	{
		wire::ParityTrace subject = scripted(3);
		std::vector<wire::ParityEntity> kept;
		for (const wire::ParityEntity &e : subject.entities)
			if (e.cls != 'V')
				kept.push_back(e);
		subject.entities = std::move(kept);
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "entity_types", "", "fail", "FRAME_ENTITY_TYPES_MISMATCH");
		expect_row(c, 'S', "entity_field_population", "", "fail", "ENTITY_FIELD_EMPTY");
	}
	{
		// A player that never moves in world units, whatever its codes do.
		wire::ParityTrace subject = scripted(3);
		for (wire::ParityEntity &e : subject.entities)
			if (e.cls == 'P') {
				e.world_x = 777 * 65536;
				e.world_y = 888 * 65536;
				e.world_z = 50 * 65536;
			}
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "entity_motion", "P", "fail", "FRAME_ENTITY_CONSTANT");
	}
}

// The camp AI's records (x7 selfnade's 0x0035): `lead` records at pitch `lead_pitch` from
// the steady start, then its pitch at rest, stepping 210..212 every 20 frames.
void add_camp_ai(wire::ParityTrace &trace, size_t lead, int32_t lead_pitch) {
	std::vector<wire::ParityEntity> added;
	size_t k = 0;
	for (const wire::ParityFrameState &f : trace.frames) {
		if (f.ts_ns < kSteadyStart)
			continue;
		wire::ParityEntity e;
		e.frame = f.frame;
		e.ts_ns = f.ts_ns;
		e.session = kClientPort;
		e.cls = 'I';
		e.handle = 0x0035;
		e.type_id = 1799;
		e.pos_x = 4000;
		e.pos_y = 4000;
		e.pos_z = 100;
		e.orient_a = 210;
		e.orient_b = k < lead ? lead_pitch : 210 + int32_t(k / 20 % 3);
		place(e, f);
		added.push_back(e);
		++k;
	}
	trace.entities.insert(trace.entities.end(), added.begin(), added.end());
}

// A dynamic sequence's lone transient lead sets no baseline (state.transient_lead): x7's
// selfnade RR, RR-aa and RO carried 0x0035's one aim-change record (210,3) in the window
// before its (210,210) rest, OR's fell before the window; a lead the next record repeats
// is a rest and keeps its baseline.
void test_dynamic_rules() {
	auto runs = [](size_t lead) {
		wire::ParityTrace rr = scripted(1), aa = scripted(2), subject = scripted(3);
		add_camp_ai(rr, lead, 3);
		add_camp_ai(aa, lead, 3);
		add_camp_ai(subject, 0, 3);
		return judge_case(std::move(rr), std::move(aa), std::move(subject));
	};
	{
		const Case c = runs(1);
		expect_row(c, 'S', "entity_I_orientation_displacement_p90", "", "pass");
		expect_row(c, 'S', "entity_I_orientation_range_p90", "", "pass");
	}
	{
		const Case c = runs(10);
		expect_row(c, 'S', "entity_I_orientation_displacement_p90", "", "fail",
				"DYNAMIC_RANGE_MISMATCH");
	}
	{
		// A lone tail (state.transient_tail): x8's swap RR and RR-aa joiners stood still and
		// sent the self-grenade's knockback on their last uplink before the death, OR's did
		// not. The change its end cut sets no motion; two changes at the end still do.
		auto still = [](uint32_t seed, int moved_tail) {
			wire::ParityTrace trace = scripted(seed);
			size_t last = 0;
			for (size_t i = 0; i < trace.uplinks.size(); ++i) {
				wire::ParityUplinkState &u = trace.uplinks[i];
				u.pos_x = 1000 * 65536;
				u.pos_y = 2000 * 65536;
				u.pos_z = 50 * 65536;
				if (u.ts_ns <= kSteadyStart + uint64_t(kSteadySeconds * 1e9))
					last = i;
			}
			for (int k = 0; k < moved_tail; ++k)
				trace.uplinks[last - size_t(k)].pos_x += (k + 1) * 32768;
			trace.uplinks.resize(last + 1); // the uplinks stop at the window's last
			return trace;
		};
		const Case tail = judge_case(still(1, 1), still(2, 1), still(3, 0));
		expect_row(tail, 'C', "uplink_motion", "", "pass");
		const Case moving = judge_case(still(1, 2), still(2, 2), still(3, 0));
		expect_row(moving, 'C', "uplink_motion", "", "fail", "UPLINK_CONSTANT");
	}
}

void test_evidence() {
	{
		wire::ParityTrace subject = scripted(3);
		std::vector<wire::ParityFrameState> kept;
		for (const wire::ParityFrameState &f : subject.frames)
			if (f.ts_ns < 10 * kSec || f.ts_ns > 12 * kSec)
				kept.push_back(f);
		subject.frames = std::move(kept);
		const Case c = judge_case(scripted(1), scripted(2), subject);
		bool gap = false;
		for (const EvidenceRow &row : c.verdict.evidence)
			gap = gap || (row.run == "subject" && row.code == "STEADY_GAMEPLAY_GAP");
		check(gap, "a 2 s hole in the steady 0x0A stream is STEADY_GAMEPLAY_GAP");
		check(!c.verdict.pass(), "an evidence finding fails the verdict");
	}
	{
		wire::ParityTrace rr = scripted(1);
		rr.datagrams[40].decoded = false;
		const Case c = judge_case(rr, scripted(2), scripted(3));
		bool bad = false;
		for (const EvidenceRow &row : c.verdict.evidence)
			bad = bad || (row.run == "rr" && row.code == "RAW_DATAGRAM_DECODE_FAILED");
		check(bad, "an undecoded datagram in RR's comparison interval is evidence");
	}
	{
		Case c;
		c.rr = make_run_facts(info_for("rr", "RR"), scripted(1));
		RunInfo other = info_for("subject", "RO");
		other.mission = "OTHER.BMS";
		c.subject = make_run_facts(other, scripted(3));
		c.aa = make_run_facts(info_for("rr_aa", "RR"), scripted(2));
		const Verdict v = judge(c.rr, c.aa, { { "RO", &c.subject } });
		bool mismatch = false;
		for (const EvidenceRow &row : v.evidence)
			mismatch = mismatch || row.code == "CASE_MISMATCH";
		check(mismatch, "a subject from another mission is CASE_MISMATCH");
	}
}

void test_document() {
	wire::ParityTrace subject = scripted(3);
	erase_events(subject, 'S', 0x07B);
	const Case c = judge_case(scripted(1), scripted(2), subject);
	const std::string text =
			verdict_json(c.verdict, "items.def", c.rr, c.aa, { { "RO", &c.subject } });
	io::JsonValue json;
	std::string error;
	check(io::json_parse(text, json, error), "the verdict is JSON: " + error);
	check(json.get_string("schema", "") == "opennova.parity-verdict.v1", "the verdict schema");
	check(json.get_string("status", "") == "FAIL", "the verdict status");
	check(json.get_int("failures", -1) == int(c.verdict.failures), "the failure count");
	const io::JsonValue *registry = json.get("normalizations");
	check(registry && registry->array.size() == kNormalizationCount,
			"the verdict carries the normalization registry");
	const io::JsonValue *rows = json.get("rows");
	check(rows && rows->array.size() == c.verdict.rows.size(), "every row is written");
	bool found = false;
	for (const io::JsonValue &row : rows ? rows->array : std::vector<io::JsonValue>{}) {
		if (row.get_string("metric", "") != "tag_presence" || row.get_string("key", "") != "0x7b:0")
			continue;
		found = row.get_string("status", "") == "fail" &&
				row.get_string("code", "") == "MISSING_TAG" &&
				row.get_string("topology", "") == "RO" && row.get_string("direction", "") == "S" &&
				row.get("aa_distance") && row.get("floor") && row.get_string("rr", "") == "present";
	}
	check(found, "a row carries topology, direction, metric, code, values, A/A distance and floor");
	check(json.get("aa") && json.get("aa")->is_array(), "the aa sanity section");
}

// ---------------------------------------------------------------------------
// The retail-LAN triage's rules: each mask holds against retail-shaped noise, and
// each real divergence it could hide stays visible through a row.

wire::ParityHandshake &handshake_of(wire::ParityTrace &trace, uint8_t opcode) {
	for (wire::ParityHandshake &h : trace.handshakes)
		if (h.opcode == opcode)
			return h;
	return trace.handshakes.front();
}

// Replace one flat-TLV field's value (by name), the others kept in order.
void set_tlv(wire::ParityHandshake &h, std::string_view name, const std::vector<uint8_t> &value) {
	std::vector<uint8_t> body;
	size_t pos = 0;
	while (pos < h.body.size()) {
		FlatTlvField field;
		const size_t next = read_flat_tlv(h.body.data(), h.body.size(), pos, field);
		if (next == kFlatTlvEnd)
			break;
		if (field.name == name)
			append_flat_tlv(body, field.name, value.data(), uint16_t(value.size()));
		else
			append_flat_tlv(body, field.name, field.value, field.size);
		pos = next;
	}
	h.body = std::move(body);
}

void set_cu(wire::ParityHandshake &h, const std::string &cu_name, const std::string &text) {
	std::vector<uint8_t> body;
	size_t pos = 0;
	while (pos < h.body.size()) {
		FlatTlvField field;
		const size_t next = read_flat_tlv(h.body.data(), h.body.size(), pos, field);
		if (next == kFlatTlvEnd)
			break;
		const std::string inner(reinterpret_cast<const char *>(field.value), field.size);
		if (field.name == "CU" && inner.compare(1, cu_name.size() + 1, cu_name + '\0') == 0)
			tlv_cu(body, uint8_t(field.value[0]), cu_name, text);
		else
			append_flat_tlv(body, field.name, field.value, field.size);
		pos = next;
	}
	h.body = std::move(body);
}

void test_handshake_rules() {
	{
		// EIP/EPN are the announce's destination: deterministic, so compared.
		wire::ParityTrace subject = scripted(3);
		std::vector<uint8_t> loopback;
		put_u32(loopback, 0x0100007F);
		set_tlv(handshake_of(subject, 0x41), "EIP", loopback);
		expect_row(judge_case(scripted(1), scripted(2), subject), 'C', "handshake", "0x41", "fail",
				"HANDSHAKE_MISMATCH");
	}
	{
		// AP names the running program (D-NET-NEW-L9-2): its value and size leave the key;
		// another field's size still compares.
		wire::ParityTrace runtime = scripted(3), wide = scripted(3);
		set_tlv(handshake_of(runtime, 0x41), "AP", text_bytes("Godot_v4.6.1-stable_win64.exe"));
		std::vector<uint8_t> wide_eip;
		put_u32(wide_eip, 0xFFFFFFFFu);
		wide_eip.push_back(0);
		set_tlv(handshake_of(wide, 0x41), "EIP", wide_eip);
		expect_row(judge_case(scripted(1), scripted(2), runtime), 'C', "handshake", "0x41", "pass");
		expect_row(judge_case(scripted(1), scripted(2), wide), 'C', "handshake", "0x41", "fail",
				"HANDSHAKE_MISMATCH");
	}
	{
		// CU CI0 keeps its low 16 bits: the residue high word never compares, the id does.
		wire::ParityTrace bare = scripted(3), other_id = scripted(3);
		set_cu(handshake_of(bare, 0x42), "CI0", "512");
		set_cu(handshake_of(other_id, 0x42), "CI0", "513");
		expect_row(judge_case(scripted(1), scripted(2), bare), 'C', "handshake", "0x42", "pass");
		expect_row(judge_case(scripted(1), scripted(2), other_id), 'C', "handshake", "0x42", "fail",
				"HANDSHAKE_MISMATCH");
	}
	{
		// SCRK: 62 or 63 bytes is one class; an LCG walk is the retail generator.
		wire::ParityTrace lcg = scripted(3), drawn = scripted(3), short_key = scripted(3);
		set_tlv(handshake_of(lcg, 0x82), "SCRK", text_bytes(lcg_key(9, 62)));
		set_tlv(handshake_of(drawn, 0x82), "SCRK", text_bytes(std::string(61, 'B')));
		set_tlv(handshake_of(short_key, 0x82), "SCRK", text_bytes(lcg_key(9, 59)));
		const Case a = judge_case(scripted(1), scripted(2), lcg, "OR");
		expect_row(a, 'S', "handshake", "0x82", "pass");
		expect_row(a, 'S', "handshake_key_lcg", "0x82", "pass");
		const Case b = judge_case(scripted(1), scripted(2), drawn, "OR");
		expect_row(b, 'S', "handshake", "0x82", "pass");
		expect_row(b, 'S', "handshake_key_lcg", "0x82", "fail", "HANDSHAKE_KEY_GENERATOR");
		expect_row(judge_case(scripted(1), scripted(2), short_key, "OR"), 'S', "handshake", "0x82",
				"fail", "HANDSHAKE_MISMATCH");
	}
	{
		// One announce per session: a second hello is a second leg.
		wire::ParityTrace subject = scripted(3);
		wire::ParityHandshake again = handshake_of(subject, 0x41);
		again.frame = 0;
		subject.handshakes.insert(subject.handshakes.begin(), again);
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'C', "handshake_count", "0x41", "fail", "HANDSHAKE_COUNT_MISMATCH");
		expect_row(c, 'C', "handshake_count", "0x42", "pass");
	}
}

// One more message in the initial stream, riding the packet that opens with
// `host` (every run of a case gets the same, so the grouping stays equal).
void add_initial(wire::ParityTrace &trace, char dir, uint16_t host, uint16_t identity,
		const std::vector<uint8_t> &body) {
	for (wire::ParityPacket &p : trace.packets) {
		if (p.dir != dir || p.tags.empty() || p.tags[0] != host)
			continue;
		p.tags.push_back(identity);
		wire::ParityWireRecord record;
		record.full_tag = identity;
		record.raw_flags = 0x20;
		record.encoded_length = uint32_t(body.size());
		p.records.push_back(record);
		wire::ParityEvent e;
		e.frame = p.frame;
		e.ts_ns = p.ts_ns;
		e.dir = dir;
		e.session = kClientPort;
		e.participant = 1;
		e.full_tag = identity;
		e.length = body.size();
		e.material = true;
		e.body = body;
		trace.events.push_back(e);
		return;
	}
}

// The host's admission reply and the spawn pump's add (S2C 0x03, the add's settings
// records, 0x05, 0x04, 0x7B), a periodic-second phase draw apart: RR's two packets and
// a run that carries both in one group alike (initial.S.spawn_pump_boundary); a
// merged pair of other groups still fails.
void test_spawn_pump_boundary() {
	auto record = [](uint16_t tag) {
		wire::ParityWireRecord r;
		r.full_tag = tag;
		r.raw_flags = 0x20;
		r.encoded_length = 4;
		return r;
	};
	const std::vector<uint16_t> reply = { 0x100, 0x100, 0x001, 0x07A, 0x07B, 0x003 };
	const std::vector<uint16_t> add = { 0x003, 0x100, 0x100, 0x005, 0x004, 0x07B };
	auto admit = [&](wire::ParityTrace &trace, bool merged, bool add_only_tail) {
		// Ahead of the scripted initial stream (its first S2C packet's frame), at fresh
		// sequence numbers.
		size_t at = 0;
		while (at < trace.packets.size() && trace.packets[at].dir != 'S')
			++at;
		wire::ParityPacket model = trace.packets[at];
		model.flags = 0x00;
		auto packet = [&](const std::vector<uint16_t> &tags, uint32_t seq) {
			wire::ParityPacket p = model;
			p.seq = seq;
			p.tags = tags;
			p.records.clear();
			for (const uint16_t tag : tags)
				p.records.push_back(record(tag));
			return p;
		};
		std::vector<uint16_t> second = add;
		if (add_only_tail)
			second.erase(second.begin()); // no 0x03: not the pump's add
		std::vector<wire::ParityPacket> inserted;
		if (merged) {
			std::vector<uint16_t> both = reply;
			both.insert(both.end(), second.begin(), second.end());
			inserted.push_back(packet(both, 7001));
		} else {
			inserted.push_back(packet(reply, 7001));
			inserted.push_back(packet(second, 7002));
		}
		trace.packets.insert(trace.packets.begin() + long(at), inserted.begin(), inserted.end());
	};
	wire::ParityTrace rr = scripted(1), aa = scripted(2), merged = scripted(3), other = scripted(3);
	admit(rr, false, false);
	admit(aa, false, false);
	admit(merged, true, false);
	expect_row(judge_case(rr, aa, merged), 'S', "initial_grouping", "", "pass");
	wire::ParityTrace rr2 = scripted(1), aa2 = scripted(2);
	admit(rr2, false, true);
	admit(aa2, false, true);
	admit(other, true, true);
	expect_row(judge_case(rr2, aa2, other), 'S', "initial_grouping", "", "fail",
			"INITIAL_PACKET_GROUPING");
}

void test_initial_rules() {
	{
		// S2C 0x16 rides the scoreboard clock: leaving the 0x75/0x64 packet (its first
		// list now the first steady one) moves no group and no first-occurrence order.
		wire::ParityTrace subject = scripted(3);
		for (wire::ParityPacket &packet : subject.packets)
			if (packet.dir == 'S' && packet.tags.size() == 3 && packet.tags[2] == 0x016) {
				packet.tags.pop_back();
				packet.records.pop_back();
			}
		erase_events(subject, 'S', 0x016, [](size_t i) { return i == 0; });
		const Case c = judge_case(scripted(1), scripted(2), subject);
		expect_row(c, 'S', "initial_grouping", "", "pass");
		expect_row(c, 'S', "first_order", "", "pass");
	}
	{
		// The client's send pump batches by load timing: a split packet is no
		// difference, an extra record is.
		wire::ParityTrace split = scripted(3), extra = scripted(3);
		for (wire::ParityPacket &p : split.packets)
			if (p.dir == 'C' && p.tags.size() == 2 && p.tags[1] == 0x009) {
				wire::ParityPacket second = p;
				second.seq = 9999;
				second.tags = { 0x009 };
				second.records = { p.records[1] };
				p.tags.pop_back();
				p.records.pop_back();
				split.packets.push_back(second);
				wire::ParityDatagram d = split.datagrams.front();
				d.frame = p.frame;
				d.datagram_class = CaptureDatagramClass::ClientProtocol;
				split.datagrams.push_back(d);
				break;
			}
		std::stable_sort(split.packets.begin(), split.packets.end(),
				[](const auto &x, const auto &y) { return x.frame < y.frame; });
		add_initial(extra, 'C', 0x022, 0x009, bytes({ 2, 2 }));
		expect_row(judge_case(scripted(1), scripted(2), split), 'C', "initial_grouping", "",
				"pass");
		expect_row(judge_case(scripted(1), scripted(2), extra), 'C', "initial_grouping", "",
				"fail", "INITIAL_PACKET_GROUPING");
	}
	{
		// The joiner's C2S 0x22 re-requests an unbound S2C 0x16 row, so it rides the
		// 0x16's phase (order.C.0x22.phase): with the 0x16 apart from the 0x11 (the
		// scripted case) a 0x22 sent after the 0x0A moves no order and no group; with
		// the 0x16 in the 0x11's packet on every run the joiner answers both in one
		// pass, the 0x22 first, and the same move fails.
		auto late_request = [](wire::ParityTrace trace) {
			for (wire::ParityPacket &p : trace.packets)
				if (p.dir == 'C' && p.tags.size() == 2 && p.tags[0] == 0x022) {
					p.tags.erase(p.tags.begin());
					p.records.erase(p.records.begin());
					break;
				}
			erase_events(trace, 'C', 0x022);
			add_initial(trace, 'C', 0x00A, 0x022, bytes({ 1 }));
			return trace;
		};
		auto riding = [](wire::ParityTrace trace) {
			add_initial(trace, 'S', 0x01C, 0x016, bytes({ 7, 7, 7 }));
			return trace;
		};
		const Case apart = judge_case(scripted(1), scripted(2), late_request(scripted(3)));
		expect_row(apart, 'C', "first_order", "", "pass");
		expect_row(apart, 'C', "initial_grouping", "", "pass");
		const Case rode = judge_case(riding(scripted(1)), riding(scripted(2)),
				riding(late_request(scripted(3))));
		expect_row(rode, 'C', "first_order", "", "fail", "FIRST_ORDER_MISMATCH");
		expect_row(rode, 'C', "initial_grouping", "", "fail", "INITIAL_PACKET_GROUPING");
	}
	{
		// A gameplay or world-page record's length and a fragment's length leave the
		// grouping key; any other record's length stays in it.
		wire::ParityTrace masked = scripted(3), framed = scripted(3);
		for (wire::ParityPacket &p : masked.packets)
			for (wire::ParityWireRecord &r : p.records) {
				if (p.dir == 'S' && r.full_tag == 0x00A)
					r.encoded_length += 7;
				if (p.dir == 'S' && r.full_tag == 0x00F) {
					r.raw_flags |= 0x04; // a first fragment: the packet's remaining room
					r.encoded_length += 9;
				}
			}
		for (wire::ParityTrace *trace : { &framed })
			for (wire::ParityPacket &p : trace->packets)
				for (wire::ParityWireRecord &r : p.records)
					if (p.dir == 'S' && r.full_tag == 0x011)
						r.encoded_length += 1;
		wire::ParityTrace rr = scripted(1), aa = scripted(2);
		for (wire::ParityTrace *trace : { &rr, &aa })
			for (wire::ParityPacket &p : trace->packets)
				for (wire::ParityWireRecord &r : p.records)
					if (p.dir == 'S' && r.full_tag == 0x00F)
						r.raw_flags |= 0x04;
		expect_row(judge_case(rr, aa, masked), 'S', "initial_grouping", "", "pass");
		expect_row(judge_case(scripted(1), scripted(2), framed), 'S', "initial_grouping", "",
				"fail", "INITIAL_PACKET_GROUPING");
	}
	{
		// Bodies compare over the common prefix; a count difference is the count rule's.
		wire::ParityTrace one = scripted(3), three = scripted(3);
		add_initial(one, 'S', 0x07B, 0x011, bytes({ 3 }));
		for (int i = 0; i < 3; ++i)
			add_initial(three, 'S', 0x07B, 0x011, bytes({ 3 }));
		const Case c1 = judge_case(scripted(1), scripted(2), one);
		expect_row(c1, 'S', "initial_body", "0x11:0", "pass");
		expect_row(c1, 'S', "initial_count", "0x11:0", "pass");
		const Case c3 = judge_case(scripted(1), scripted(2), three);
		expect_row(c3, 'S', "initial_body", "0x11:0", "pass");
		expect_row(c3, 'S', "initial_count", "0x11:0", "fail", "INITIAL_COUNT_MISMATCH");
	}
	{
		// S2C 0x16's initial bodies compare as a set: a repeat is the boundary phase,
		// a new body (the in-game count) is not.
		wire::ParityTrace repeat = scripted(3), other = scripted(3);
		add_initial(repeat, 'S', 0x07B, 0x016, bytes({ 7, 7, 7 }));
		add_initial(other, 'S', 0x07B, 0x016, bytes({ 7, 7, 8 }));
		expect_row(judge_case(scripted(1), scripted(2), repeat), 'S', "initial_body_set", "0x16:0",
				"pass");
		expect_row(judge_case(scripted(1), scripted(2), other), 'S', "initial_body_set", "0x16:0",
				"fail", "INITIAL_BODY_MISMATCH");
		check(find_row(judge_case(scripted(1), scripted(2), repeat).verdict.rows, 'S',
					  "initial_body", "0x16:0") == nullptr,
				"S2C 0x16 leaves the position-by-position comparison");
	}
	{
		// S2C 0x46: the squad id is per account (it already differs per seed); the
		// quality byte is the case's.
		wire::ParityTrace subject = scripted(3);
		for_events(subject, 'S', 0x046,
				[](wire::ParityEvent &e) { e.body = player_sync(0, 5, 0, 3); });
		expect_row(judge_case(scripted(1), scripted(2), subject), 'S', "initial_body_set", "0x46:0",
				"fail", "INITIAL_BODY_MISMATCH");
	}
	{
		// The host answers each C2S 0x2F with its S2C 0x5A on the next boundary
		// (initial.S.0x5a.reply): two replies in one packet and two in their own
		// packets group alike; a missing reply does not.
		auto replies = [](uint32_t seed, const std::vector<std::vector<uint16_t>> &packets) {
			wire::ParityTrace trace = scripted(seed);
			for (size_t i = 0; i < trace.packets.size(); ++i) {
				if (trace.packets[i].dir != 'S' || trace.packets[i].tags.empty() ||
						trace.packets[i].tags[0] != 0x07B)
					continue;
				const wire::ParityPacket host = trace.packets[i];
				uint32_t seq = 8100;
				std::vector<wire::ParityPacket> added;
				for (const std::vector<uint16_t> &tags : packets) {
					wire::ParityPacket p = host;
					p.seq = ++seq;
					p.tags = tags;
					p.records.clear();
					for (const uint16_t tag : tags) {
						wire::ParityWireRecord r;
						r.full_tag = tag;
						r.raw_flags = 0x20;
						r.encoded_length = 4;
						p.records.push_back(r);
					}
					added.push_back(p);
				}
				trace.packets.insert(trace.packets.begin() + long(i), added.begin(), added.end());
				break;
			}
			return trace;
		};
		const std::vector<std::vector<uint16_t>> together = { { 0x05A, 0x05A } };
		const std::vector<std::vector<uint16_t>> apart = { { 0x05A }, { 0x05A } };
		const std::vector<std::vector<uint16_t>> one = { { 0x05A } };
		expect_row(judge_case(replies(1, together), replies(2, together), replies(3, apart)), 'S',
				"initial_grouping", "", "pass");
		expect_row(judge_case(replies(1, together), replies(2, together), replies(3, one)), 'S',
				"initial_grouping", "", "fail", "INITIAL_PACKET_GROUPING");
	}
}

std::vector<uint8_t> organics(int32_t live_x, int32_t hidden_x, uint16_t ai_net_id) {
	OrganicSpawnBatch batch;
	OrganicSpawnRecord live; // a deployed body: its owner's input moves it
	live.slot_id = 0;
	live.def_type = 3;
	live.item_type_id = 5305;
	live.entity_name = "ParityHost";
	live.minimap_flags = 0x0100;
	live.pos_x = live_x;
	live.orientation = live_x * 3;
	live.net_id = ai_net_id;
	OrganicSpawnRecord hidden = live; // an unjoined body at its placement
	hidden.slot_id = 1;
	hidden.entity_name = "ParityJoin";
	hidden.minimap_flags = 0x0101;
	hidden.pos_x = hidden_x;
	hidden.orientation = 0x40000000;
	hidden.net_id = 0;
	batch.records = { live, hidden };
	batch.entity_count = 2;
	return encode_organic_spawn_batch(batch);
}

std::vector<uint8_t> statics(uint8_t zone_byte, uint16_t radius, uint32_t flags) {
	StaticEntityBatch batch;
	StaticEntityRecord record;
	record.item_type_id = 1435;
	record.field_flags = kStaticEntityHasEntityFlags;
	record.pos_x = 57610843;
	record.entity_flags = flags;
	record.weapon_byte = zone_byte;
	record.attach_ref = radius;
	StaticEntityRecord empty;
	empty.item_type_id = 0;
	empty.is_empty_slot = true;
	batch.start_index = 4;
	batch.records = { record, empty };
	batch.entity_count = 2;
	return encode_static_entity_batch(batch);
}

void test_world_records() {
	auto with = [](uint32_t seed, const std::vector<uint8_t> &organic,
						const std::vector<uint8_t> &statics_body) {
		wire::ParityTrace trace = scripted(seed);
		add_initial(trace, 'S', 0x07B, 0x00C, organic);
		add_initial(trace, 'S', 0x07B, 0x010, statics_body);
		return trace;
	};
	const std::vector<uint8_t> base_statics = statics(0, 0, 0x08020000);
	{
		// A live record's pose is its owner's; a hidden record's pose and every other
		// field are the case's.
		const Case live = judge_case(with(1, organics(100, 5, 0), base_statics),
				with(2, organics(200, 5, 0), base_statics),
				with(3, organics(300, 5, 0), base_statics));
		expect_row(live, 'S', "initial_records", "0x0c:0", "pass");
		check(find_row(live.verdict.rows, 'S', "initial_body", "0x0c:0") == nullptr,
				"S2C 0x0C compares as records, not bytes");
		const Case hidden = judge_case(with(1, organics(100, 5, 0), base_statics),
				with(2, organics(200, 5, 0), base_statics),
				with(3, organics(300, 6, 0), base_statics));
		expect_row(hidden, 'S', "initial_records", "0x0c:0", "fail", "INITIAL_RECORD_MISMATCH");
		const Case net_id = judge_case(with(1, organics(100, 5, 0), base_statics),
				with(2, organics(200, 5, 0), base_statics),
				with(3, organics(300, 5, 6), base_statics));
		expect_row(net_id, 'S', "initial_records", "0x0c:0", "fail", "INITIAL_RECORD_MISMATCH");
	}
	{
		// The zone byte and radius are stack residue; the static's flags are not.
		const std::vector<uint8_t> organic = organics(100, 5, 0);
		const Case residue = judge_case(with(1, organic, statics(0x80, 10, 0x08020000)),
				with(2, organic, base_statics), with(3, organic, statics(0x40, 7, 0x08020000)));
		expect_row(residue, 'S', "initial_records", "0x10:0", "pass");
		check(find_row(residue.verdict.rows, 'S', "length_set", "0x10:0") == nullptr,
				"S2C 0x10's length set follows the residue: left out");
		// Its length and the length field's size leave the grouping key too
		// (group.length_exempt): a longer last page sized LEN16 groups the same.
		wire::ParityTrace sized = with(3, organic, statics(0x40, 7, 0x08020000));
		for (wire::ParityPacket &p : sized.packets)
			for (wire::ParityWireRecord &r : p.records)
				if (p.dir == 'S' && r.full_tag == 0x010) {
					r.raw_flags = 0x40;
					r.encoded_length += 300;
				}
		expect_row(judge_case(with(1, organic, base_statics), with(2, organic, base_statics), sized),
				'S', "initial_grouping", "", "pass");
		const Case flags = judge_case(with(1, organic, base_statics),
				with(2, organic, base_statics), with(3, organic, statics(0, 0, 0x00020000)));
		expect_row(flags, 'S', "initial_records", "0x10:0", "fail", "INITIAL_RECORD_MISMATCH");
	}
	{
		// The residue decides whether the last statics spill into one more page (x5's
		// 01tr-coop-drive A/A: 53 against 54 S2C 0x10 messages), so the stream stands
		// as one group at its first page however many packets carry it
		// (group.S.0x10.run); a run with no stream at all still differs.
		const std::vector<uint8_t> organic = organics(100, 5, 0);
		auto spilled = [](wire::ParityTrace trace, const std::vector<uint8_t> &page) {
			for (size_t i = 0; i < trace.packets.size(); ++i) {
				const wire::ParityPacket &host = trace.packets[i];
				if (host.dir != 'S' || host.tags.empty() || host.tags[0] != 0x07B)
					continue;
				wire::ParityPacket p = host;
				p.seq = 8001;
				p.tags = { 0x010 };
				wire::ParityWireRecord r;
				r.full_tag = 0x010;
				r.raw_flags = 0x20;
				r.encoded_length = uint32_t(page.size());
				p.records = { r };
				wire::ParityEvent e;
				e.frame = p.frame;
				e.ts_ns = p.ts_ns;
				e.dir = 'S';
				e.session = kClientPort;
				e.participant = 1;
				e.full_tag = 0x010;
				e.length = page.size();
				e.material = true;
				e.body = page;
				trace.packets.insert(trace.packets.begin() + long(i) + 1, p);
				trace.events.push_back(e);
				break;
			}
			return trace;
		};
		expect_row(judge_case(with(1, organic, base_statics), with(2, organic, base_statics),
						   spilled(with(3, organic, base_statics), base_statics)),
				'S', "initial_grouping", "", "pass");
		wire::ParityTrace none = scripted(3);
		add_initial(none, 'S', 0x07B, 0x00C, organic);
		expect_row(judge_case(with(1, organic, base_statics), with(2, organic, base_statics), none),
				'S', "initial_grouping", "", "fail", "INITIAL_PACKET_GROUPING");
	}
}

// S2C 0x0D compares by slot with a live record's pose left out: a hull on water
// (entity+0x24 bit 0x8000), an awake vehicle (bit 0x40 or an occupant) and a record
// carried on a live one, its carrier on an earlier page. An awake vehicle's contact bit
// and ground link follow its physics too; a carried record's carrier link compares. A
// parked vehicle, a record carried on it and every other field compare.
void test_pool_live_pose() {
	struct Pose {
		int32_t hull = 0, manned = 0, parked = 0, on_hull = 0, on_parked = 0;
		uint8_t hull_team = 1;
		bool manned_airborne = false, manned_on_hull = false, on_hull_moved = false,
			 parked_airborne = false;
	};
	auto record = [](uint16_t slot, uint32_t entity_flags, int32_t jitter) {
		PoolSpawnRecord r;
		r.spawn_flags = kPoolSpawnHasEntityFlags | kPoolSpawnHasEulerZ | kPoolSpawnHasEulerX |
				kPoolSpawnHasEulerY | kPoolSpawnHasTeamByte;
		r.slot_id = slot;
		r.item_type_id = 0x0500;
		r.entity_flags = entity_flags;
		r.pos_x = 0x100000 + jitter;
		r.pos_y = 0x200000 - jitter;
		r.pos_z = 0x0d0000 + jitter;
		r.euler_z = 0x40000000 + jitter * 64;
		r.euler_x = jitter * 32;
		r.euler_y = -jitter * 16;
		r.team_byte = 1;
		return r;
	};
	auto pages = [&](const Pose &p) {
		PoolSpawnBatch first, second;
		first.records.push_back(record(0x1003, 0x00028400, p.hull));
		first.records.back().team_byte = p.hull_team;
		first.records.push_back(
				record(0x1001, 0x00020440 | (p.manned_airborne ? 0x2000u : 0u), p.manned));
		first.records.back().spawn_flags |= kPoolSpawnHasParentHandle;
		first.records.back().parent_handle = 0x0055;
		if (p.manned_on_hull) {
			first.records.back().spawn_flags |= kPoolSpawnHasTargetHandle;
			first.records.back().target_handle = 0x1003;
		}
		first.records.push_back(
				record(0x1000, 0x00020400 | (p.parked_airborne ? 0x2000u : 0u), p.parked));
		second.records.push_back(record(0x1004, 0x00020000, p.on_hull));
		second.records.back().spawn_flags |= kPoolSpawnHasTargetHandle;
		second.records.back().target_handle = p.on_hull_moved ? 0x1001 : 0x1003;
		second.records.push_back(record(0x1005, 0x00020000, p.on_parked));
		second.records.back().spawn_flags |= kPoolSpawnHasTargetHandle;
		second.records.back().target_handle = 0x1000;
		return std::make_pair(encode_pool_spawn_batch(first), encode_pool_spawn_batch(second));
	};
	auto with = [&](uint32_t seed, const Pose &p) {
		wire::ParityTrace trace = scripted(seed);
		const auto bodies = pages(p);
		add_initial(trace, 'S', 0x07B, 0x00D, bodies.first);
		add_initial(trace, 'S', 0x07B, 0x00D, bodies.second);
		return trace;
	};
	const Pose rest;
	Pose live = rest;
	live.hull = 0x1e2;
	live.manned = 5;
	live.on_hull = 0x1e2;
	live.manned_airborne = true;
	live.manned_on_hull = true;
	const Case moved = judge_case(with(1, rest), with(2, rest), with(3, live));
	expect_row(moved, 'S', "initial_records", "0x0d:0", "pass");
	check(find_row(moved.verdict.rows, 'S', "initial_body", "0x0d:0") == nullptr,
			"S2C 0x0D compares as records, not bytes");
	check(find_row(moved.verdict.rows, 'S', "length_set", "0x0d:0") == nullptr,
			"S2C 0x0D's page lengths follow its live records' value-gated fields: left out");
	for (const auto &change : std::vector<std::pair<const char *, Pose>>{
				 { "a parked vehicle's z", [&] { Pose p = rest; p.parked = 1; return p; }() },
				 { "a record carried on a parked vehicle",
						 [&] { Pose p = rest; p.on_parked = 1; return p; }() },
				 { "a live hull's team", [&] { Pose p = live; p.hull_team = 2; return p; }() },
				 { "a parked vehicle's contact bit",
						 [&] { Pose p = rest; p.parked_airborne = true; return p; }() },
				 { "a carried record's carrier link",
						 [&] { Pose p = rest; p.on_hull_moved = true; return p; }() } }) {
		const Case c = judge_case(with(1, rest), with(2, rest), with(3, change.second));
		const VerdictRow *row = find_row(c.verdict.rows, 'S', "initial_records", "0x0d:0");
		check(row && row->status == "fail", std::string("0x0D: ") + change.first + " compares");
	}
	{
		// The detail names every differing slot, each with every field it differs in
		// (a fix lane reads the whole list, LX-s17): four slots here, past the old
		// three-slot sample, the parked vehicle in its pose and its contact bit.
		Pose four = live;
		four.parked = 1;
		four.parked_airborne = true;
		four.on_parked = 1;
		four.hull_team = 2;
		four.on_hull_moved = true;
		const Case c = judge_case(with(1, live), with(2, live), with(3, four));
		const VerdictRow *row = find_row(c.verdict.rows, 'S', "initial_records", "0x0d:0");
		check(row && row->status == "fail" && row->value.distance == 4.0,
				"0x0D: four slots differ" + (row ? " (" + row->value.detail + ")" : std::string()));
		if (row) {
			const std::string &detail = row->value.detail;
			for (const char *slot : { "slot 0x1000: ", "slot 0x1003: ", "slot 0x1004: ", "slot 0x1005: " })
				check(detail.find(slot) != std::string::npos,
						std::string("0x0D detail lists ") + slot + "(" + detail + ")");
			const size_t parked = detail.find("slot 0x1000: ");
			const size_t next = detail.find("; ", parked);
			const std::string entry = detail.substr(parked, next - parked);
			check(entry.find(", y ") != std::string::npos && entry.find(", euler_y ") != std::string::npos &&
							entry.find(", in_air ") != std::string::npos,
					"0x0D detail names every field a slot differs in (" + entry + ")");
		}
	}
}

void test_reply_rules() {
	auto page = [](uint32_t start, size_t dwords) {
		std::vector<uint8_t> body;
		put_u32(body, start);
		for (size_t i = 0; i < dwords; ++i)
			put_u32(body, 0);
		return body;
	};
	using Pages = std::initializer_list<std::pair<uint32_t, size_t>>;
	auto with_pages = [&page](uint32_t seed, Pages pages) {
		wire::ParityTrace trace = scripted(seed);
		for (const auto &entry : pages)
			add_initial(trace, 'C', 0x047, 0x03D, page(entry.first, entry.second));
		return trace;
	};
	{
		// C2S 0x3D: a start only the run asked is the host's cadence; a common start's
		// length is the snapshot's.
		const Case extra = judge_case(with_pages(1, { { 50, 50 }, { 100, 50 } }),
				with_pages(2, { { 50, 50 }, { 100, 50 } }),
				with_pages(3, { { 50, 50 }, { 100, 50 }, { 150, 5 } }));
		expect_row(extra, 'C', "reply_length", "0x3d:0", "pass");
		check(find_row(extra.verdict.rows, 'C', "length_set", "0x3d:0") == nullptr,
				"C2S 0x3D's length set is replaced by the per-start rule");
		const Case shorter = judge_case(with_pages(1, { { 50, 50 }, { 100, 50 } }),
				with_pages(2, { { 50, 50 }, { 100, 50 } }),
				with_pages(3, { { 50, 50 }, { 100, 16 } }));
		expect_row(shorter, 'C', "reply_length", "0x3d:0", "fail", "REPLY_LENGTH_MISMATCH");
	}
	{
		// The RTT pong echoes a host ping; a pong of another stamp is a fault.
		auto with_rtt = [](uint32_t seed, uint32_t pong) {
			wire::ParityTrace trace = scripted(seed);
			std::vector<uint8_t> ping, echo;
			put_u32(ping, 0x1A578416 + seed);
			ping.push_back(1);
			put_u32(echo, pong == 0 ? 0x1A578416 + seed : pong);
			echo.push_back(0);
			add_initial(trace, 'S', 0x07B, 0x057, ping);
			add_initial(trace, 'C', 0x00B, 0x02C, echo);
			return trace;
		};
		expect_row(judge_case(with_rtt(1, 0), with_rtt(2, 0), with_rtt(3, 0)), 'C', "rtt_echo",
				"0x2c:0", "pass");
		expect_row(judge_case(with_rtt(1, 0), with_rtt(2, 0), with_rtt(3, 0x12345678)), 'C',
				"rtt_echo", "0x2c:0", "fail", "RTT_ECHO_MISMATCH");
	}
}

void test_order_rules() {
	{
		// A joiner's first S2C 0x40s ride the join bundle or follow the per-slot block a
		// boundary later, by the C2S 0x0B's arrival (order.S.0x40): x6's aikill RR-aa
		// and late RR carry them after the first gameplay frame, their partners before.
		static_assert(first_order_excluded('S', 0x040));
		auto overlay_at = [](uint32_t seed, uint64_t at) {
			wire::ParityTrace trace = scripted(seed);
			splice(trace, 'S', 0x040, at, bytes({ 1, 0, 0, 0, 0, 0, 0 }));
			return trace;
		};
		expect_row(judge_case(overlay_at(1, 1900 * kMs), overlay_at(2, 1900 * kMs),
						   overlay_at(3, 2500 * kMs)),
				'S', "first_order", "", "pass");
		// The join packet keys its 0x0A once: a second behind the bundle is the
		// per-slot block's, by the same phase (initial.S.join_tail).
		wire::ParityTrace tail = scripted(3);
		for (wire::ParityPacket &p : tail.packets)
			if (p.dir == 'S' && carries_low_tag(p, 0x00A)) {
				p.tags.push_back(0x00A);
				p.records.push_back(p.records.front());
				break;
			}
		expect_row(judge_case(scripted(1), scripted(2), tail), 'S', "initial_grouping", "", "pass");
	}
	{
		// S2C 0x46 is no milestone (the scoreboard's phase); C2S 0x09 -> S2C 0x2C is.
		wire::ParityTrace early = scripted(3), late = scripted(3);
		for_events(early, 'S', 0x046, [](wire::ParityEvent &e) { e.frame = 0; });
		expect_row(judge_case(scripted(1), scripted(2), early), 'B', "milestones", "", "pass");
		expect_row(judge_case(scripted(1), scripted(2), early), 'B', "milestone_order", "", "pass");
		int frame_0f = 0;
		for_events(late, 'S', 0x00F, [&frame_0f](wire::ParityEvent &e) { frame_0f = e.frame; });
		for_events(late, 'C', 0x009, [frame_0f](wire::ParityEvent &e) { e.frame = frame_0f; });
		expect_row(judge_case(scripted(1), scripted(2), late), 'B', "milestone_order", "", "fail",
				"MILESTONE_ORDER");
	}
	{
		// The integrity replies follow the host's process-global phase.
		wire::ParityTrace subject = scripted(3);
		size_t first_20 = 0, first_21 = 0;
		for (size_t i = 0; i < subject.events.size(); ++i) {
			const wire::ParityEvent &e = subject.events[i];
			if (e.dir == 'C' && e.full_tag == 0x020 && !first_20)
				first_20 = i;
			if (e.dir == 'C' && e.full_tag == 0x021 && !first_21)
				first_21 = i;
		}
		std::swap(subject.events[first_20], subject.events[first_21]);
		expect_row(judge_case(scripted(1), scripted(2), subject), 'C', "first_order", "", "pass");
	}
	{
		// Off the scoreboard clock: a list under 4 s after the one before it, on the
		// external capture's wire time.
		auto judge_external = [](const wire::ParityTrace &subject_external) {
			Case c;
			c.rr = make_run_facts(info_for("rr", "RR"), scripted(1));
			c.aa = make_run_facts(info_for("rr_aa", "RR"), scripted(2));
			c.subject = make_run_facts(info_for("subject", "OR"), scripted(3));
			attach_external(c.rr, scripted(1));
			attach_external(c.aa, scripted(2));
			attach_external(c.subject, subject_external);
			c.verdict = judge(c.rr, c.aa, { { "OR", &c.subject } });
			return c;
		};
		expect_row(judge_external(scripted(3)), 'S', "scoreboard_cadence", "0x16:0", "pass");
		wire::ParityTrace pushed = scripted(3);
		wire::ParityEvent extra;
		for_events(pushed, 'S', 0x016, [&extra](wire::ParityEvent &e) {
			if (extra.full_tag == 0)
				extra = e;
		});
		extra.ts_ns += 1 * kSec; // a list one second after the first
		check(extra.full_tag == 0x016, "the scripted case carries a S2C 0x16");
		pushed.events.push_back(extra);
		std::stable_sort(pushed.events.begin(), pushed.events.end(),
				[](const auto &x, const auto &y) { return x.ts_ns < y.ts_ns; });
		expect_row(judge_external(pushed), 'S', "scoreboard_cadence", "0x16:0", "fail",
				"SCOREBOARD_OFF_CADENCE");
		const Case none = judge_case(scripted(1), scripted(2), scripted(3));
		expect_row(none, 'S', "scoreboard_cadence", "0x16:0", "skip");
	}
}

// order.S.clock (Q-LX-s28-1 (a)): a S2C event on the host's mission clock and one on the
// joiner's admission order by the host tick the joiner was admitted on. The 0x0A timer
// block carries the round clock as g_RoundTimeRemaining / 62 [orig: NetPacket_WritePlayerState
// @0x4FF6B0 (@0x4FFA8D..0x4FFAA8)], stepped down a tick per 16 ms with g_CurrentTick
// [orig: Game_ProcessMainFrame @0x5263F0 (@0x5265B4, @0x526602)]. Every scripted run takes
// its S2C 0x0F at 1.38 s; `zero` is the wall time its round clock reaches zero.
void test_order_clock() {
	auto clocked = [](uint32_t seed, int64_t zero_ms, uint64_t death, uint64_t datagram) {
		wire::ParityTrace trace = scripted(seed);
		for (wire::ParityFrameState &f : trace.frames)
			if (f.timer_present)
				f.timer_seconds = zero_ms < 0
						? -1
						: int32_t((zero_ms * int64_t(kMs) - int64_t(f.ts_ns)) /
								  (16 * int64_t(kMs)) / 62);
		splice(trace, 'S', 0x013, death, bytes({ 0x14, 0x10, 0 }));
		splice(trace, 'S', 0x039, datagram, bytes({ 1 }));
		return trace;
	};
	// 03TR's shape: RR (and RR-aa) take the death 2 s before the datagram; the subject was
	// admitted 4 s earlier on the mission clock, so the same death comes 2 s after it.
	const wire::ParityTrace rr = clocked(1, 600000, 8 * kSec, 10 * kSec);
	const wire::ParityTrace aa = clocked(2, 600000, 8 * kSec, 10 * kSec);
	{
		const Case c = judge_case(rr, aa, clocked(3, 604000, 12 * kSec, 10 * kSec));
		expect_row(c, 'S', "first_order", "", "pass");
		const VerdictRow *row = find_row(c.verdict.rows, 'S', "first_order");
		check(row && row->value.distance == 0.0 &&
						row->value.detail.find(
								"; 1 across the admission and mission clocks left out "
								"(admitted -") != std::string::npos,
				"the cross-clock pair is left out, and the row says so (" +
						(row ? row->value.detail : std::string("no row")) + ")");
	}
	// Without the round clock (co-op's -1) nothing tells the clocks apart: the pair counts.
	expect_row(judge_case(rr, aa, clocked(3, -1, 12 * kSec, 10 * kSec)), 'S', "first_order", "",
			"fail", "FIRST_ORDER_MISMATCH");
	// Admitted at the same mission time: the death's 4 s is its own, on neither clock.
	expect_row(judge_case(rr, aa, clocked(3, 600000, 12 * kSec, 10 * kSec)), 'S', "first_order", "",
			"fail", "FIRST_ORDER_MISMATCH");
	// Admitted 2 s earlier, the death 4 s later: off the mission clock by 2 s, it counts.
	expect_row(judge_case(rr, aa, clocked(3, 602000, 12 * kSec, 10 * kSec)), 'S', "first_order", "",
			"fail", "FIRST_ORDER_MISMATCH");
	// The C2S order takes no clock: a C2S pair inverted the same way still counts.
	{
		auto c2s = [&clocked](uint32_t seed, int64_t zero_ms, uint64_t first, uint64_t second) {
			wire::ParityTrace trace = clocked(seed, zero_ms, 8 * kSec, 10 * kSec);
			splice(trace, 'C', 0x026, first, bytes({ 1 }));
			splice(trace, 'C', 0x027, second, bytes({ 2 }));
			return trace;
		};
		expect_row(
				judge_case(c2s(1, 600000, 8 * kSec, 10 * kSec), c2s(2, 600000, 8 * kSec, 10 * kSec),
						c2s(3, 604000, 12 * kSec, 10 * kSec)),
				'C', "first_order", "", "fail", "FIRST_ORDER_MISMATCH");
	}
}

void test_frame_rules() {
	{
		// flags2 counts sends: an offset counter passes, a step of two does not.
		wire::ParityTrace offset = scripted(3), doubled = scripted(3);
		for (wire::ParityFrameState &f : offset.frames)
			f.flags2 = (f.flags2 + 64) & 0xFFu;
		uint32_t n = 0;
		for (wire::ParityFrameState &f : doubled.frames)
			f.flags2 = (2 * n++) & 0xFFu;
		expect_row(judge_case(scripted(1), scripted(2), offset), 'S', "frame_flags2", "", "pass");
		expect_row(judge_case(scripted(1), scripted(2), offset), 'S', "frame_phase_step", "",
				"pass");
		expect_row(judge_case(scripted(1), scripted(2), doubled), 'S', "frame_phase_step", "",
				"fail", "FRAME_HEADER_FLAGS_MISMATCH");
	}
	{
		// The route nibbles: over a full cycle (16 sends) every nibble shows, so a
		// counter at another phase passes; a window cut to a few frames (x5's kick,
		// about six) holds a phase slice, which the set does not compare.
		auto phased = [](uint32_t seed, uint32_t shift, size_t keep) {
			wire::ParityTrace trace = scripted(seed);
			std::vector<wire::ParityFrameState> frames;
			size_t steady = 0;
			for (wire::ParityFrameState f : trace.frames) {
				f.flags2 = (f.flags2 + shift) & 0xFFu;
				if (f.ts_ns >= kSteadyStart && steady++ >= keep)
					continue;
				frames.push_back(f);
			}
			trace.frames = std::move(frames);
			return trace;
		};
		constexpr size_t kAll = ~size_t(0);
		expect_row(judge_case(phased(1, 0, kAll), phased(2, 0, kAll), phased(3, 5, kAll)), 'S',
				"frame_flags2", "", "pass");
		expect_row(judge_case(phased(1, 0, 6), phased(2, 3, 6), phased(3, 9, 6)), 'S',
				"frame_flags2", "", "skip");
		// A full RR window against a subject cut short: the subject is unexercised,
		// which fails while the A/A compared.
		expect_row(judge_case(phased(1, 0, kAll), phased(2, 0, kAll), phased(3, 9, 6)), 'S',
				"frame_flags2", "", "fail");
	}
	{
		// The mounted-ammo record rides the route nibble 8's frame alone
		// (frame.mount_phase): over full windows a run that never carries it fails;
		// six frames that hold it against six that do not are a phase slice (x5's
		// kick: RR {0, none} against RO's {0}), which the set does not compare.
		auto mounted = [](uint32_t seed, bool mount, size_t keep) {
			wire::ParityTrace trace = scripted(seed);
			std::vector<wire::ParityFrameState> frames;
			size_t steady = 0;
			for (wire::ParityFrameState f : trace.frames) {
				const bool in_steady = f.ts_ns >= kSteadyStart;
				if (in_steady && steady++ >= keep)
					continue;
				if (mount && ((f.flags2 & 0x0Fu) == 8u || (in_steady && steady == 1))) {
					f.mount_present = true;
					f.mount_handle = 0xFFFF;
				}
				frames.push_back(f);
			}
			trace.frames = std::move(frames);
			return trace;
		};
		constexpr size_t kAll = ~size_t(0);
		expect_row(judge_case(mounted(1, true, kAll), mounted(2, true, kAll),
						   mounted(3, false, kAll)),
				'S', "frame_mount", "", "fail", "FRAME_PASSENGER_PHASE_MISMATCH");
		expect_row(judge_case(mounted(1, true, 6), mounted(2, true, 6), mounted(3, false, 6)),
				'S', "frame_mount", "", "skip");
		expect_row(judge_case(mounted(1, true, kAll), mounted(2, true, kAll),
						   mounted(3, true, 6)),
				'S', "frame_mount", "", "fail");
	}
	{
		// The fps and CPU bytes are measurements; a CPU byte never written is not.
		auto cpu = [](uint32_t seed, uint32_t fps, uint32_t share) {
			wire::ParityTrace trace = scripted(seed);
			for (wire::ParityFrameState &f : trace.frames)
				if (f.timer_present) {
					f.timer_state[2] = fps;
					f.timer_state[3] = share;
				}
			return trace;
		};
		const Case fps = judge_case(cpu(1, 54, 2), cpu(2, 41, 1), cpu(3, 33, 3));
		expect_row(fps, 'S', "frame_timer", "", "pass");
		expect_row(fps, 'S', "frame_server_cpu_fill", "", "pass");
		expect_row(judge_case(cpu(1, 54, 2), cpu(2, 41, 1), cpu(3, 33, 0)), 'S',
				"frame_server_cpu_fill", "", "fail", "FRAME_TIMER_PHASE_MISMATCH");
	}
	{
		// The anchor's own offset: retail's recipient record sits at -CameraOffset.
		wire::ParityTrace body = scripted(3);
		for (wire::ParityEntity &e : body.entities)
			if (e.handle == 0x1000) {
				e.pos_z = 0;
				e.offset_z = 0;
			}
		const Case c = judge_case(scripted(1), scripted(2), body, "OR");
		expect_row(c, 'S', "frame_self_offset", "", "fail", "FRAME_ANCHOR_OFFSET_MISMATCH");
		expect_row(judge_case(scripted(1), scripted(2), scripted(3)), 'S', "frame_self_offset", "",
				"pass");
	}
	{
		// Positions compare in world units: a vehicle parked in the world while the
		// recipient (the anchor) moves changes its codes every frame, and is no motion.
		auto parked = [](uint32_t seed) {
			wire::ParityTrace trace = scripted(seed);
			std::map<int, const wire::ParityFrameState *> frames;
			for (const wire::ParityFrameState &f : trace.frames)
				frames[f.frame] = &f;
			for (wire::ParityEntity &e : trace.entities) {
				if (e.cls != 'V')
					continue;
				const wire::ParityFrameState &f = *frames[e.frame];
				e.pos_x = network_compress_fixedpoint(1100 * 65536 - f.anchor_x);
				e.pos_y = network_compress_fixedpoint(2010 * 65536 - f.anchor_y);
				e.pos_z = network_compress_fixedpoint(0);
				place(e, f);
				e.world_x = 1100 * 65536; // the decompressor's rounding aside
				e.world_y = 2010 * 65536;
			}
			return trace;
		};
		const Case c = judge_case(parked(1), parked(2), parked(3));
		const VerdictRow *motion = find_row(c.verdict.rows, 'S', "entity_motion", "V");
		check(motion && motion->value.reference == "never filled" && motion->status == "pass",
				"a parked vehicle's codes move with the anchor; its world position does not");
	}
	{
		// The priority scores are continuous weights: occupancy exact, medians banded.
		wire::ParityTrace near = scripted(3), far = scripted(3), occupied = scripted(3);
		for (wire::ParityUplinkState &u : near.uplinks)
			u.priority_score[0] = 105;
		for (wire::ParityUplinkState &u : far.uplinks)
			u.priority_score[0] = 2000;
		for (wire::ParityUplinkState &u : occupied.uplinks)
			u.priority_handle[1] = 0x2004;
		expect_row(judge_case(scripted(1), scripted(2), near), 'C', "uplink_priority_score_p50",
				"0", "pass");
		expect_row(judge_case(scripted(1), scripted(2), far), 'C', "uplink_priority_score_p50",
				"0", "fail", "UPLINK_PRIORITY_MISMATCH");
		expect_row(judge_case(scripted(1), scripted(2), occupied), 'C', "uplink_priority", "",
				"fail", "UPLINK_PRIORITY_MISMATCH");
	}
	check(p90({ 1.0, 2.0 }) == 2.0 && p90({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }) == 9.0 &&
					p90({ 4.0 }) == 4.0 && p90({}) == 0.0,
			"p90 is the nearest rank: two samples report the larger");
}

// The frame rows whose values a draw or a sampling phase picks, each judged by the rule
// retail's own runs set: the mount record by the own carrier state it rides
// (frame.mount_phase), the eye offset by its rests and transitions
// (frame.self_offset_rests), the health by its full and dead values (frame.health_hit).
void test_frame_draws() {
	// Rewrites each steady frame (and the own record in it, handle 0x1000) of a scripted
	// run through `f(index, frame, self)`, the index counting steady frames.
	auto rewrite = [](wire::ParityTrace trace,
						   const std::function<void(
								   size_t, wire::ParityFrameState &, wire::ParityEntity &)> &f) {
		std::map<int, wire::ParityEntity *> self;
		for (wire::ParityEntity &e : trace.entities)
			if (e.handle == 0x1000)
				self[e.frame] = &e;
		size_t index = 0;
		for (wire::ParityFrameState &frame : trace.frames)
			if (frame.ts_ns >= kSteadyStart && self.count(frame.frame))
				f(index++, frame, *self[frame.frame]);
		return trace;
	};
	{
		// x6's drive: the turret stay is 10 frames; RR and RR-aa caught its nibble-8 frame,
		// OR's stay fell between two. A stay shorter than the 16-frame cycle compares only
		// when both runs carried a record in it.
		auto turret = [&rewrite](uint32_t seed, uint32_t shift, uint32_t clip) {
			return rewrite(scripted(seed),
					[shift, clip](size_t i, wire::ParityFrameState &f, wire::ParityEntity &e) {
						const bool mounted = i >= 40 && i < 50;
						f.flags2 = uint32_t(i + shift) & 0xFFu;
						f.mount_present = (f.flags2 & 0x0Fu) == 8u;
						f.mount_handle = mounted ? 0x102E : 0xFFFF;
						f.mount_bound = mounted;
						f.mount_clip = mounted ? clip : 0;
						f.mount_reserve = mounted ? 500 : 0;
						e.carrier = mounted ? 0x102E : 0xFFFF;
						e.carrier_bone = mounted ? 6 : 0;
						e.carrier_local = mounted;
					});
		};
		// Nibble 8 at steady frame 40 on RR and RR-aa (inside 40..49), at 37 and 53 on the
		// subject.
		const Case missed =
				judge_case(turret(1, 0, 65535), turret(2, 0, 65535), turret(3, 3, 65535), "OR");
		expect_row(missed, 'S', "frame_mount", "", "pass");
		// The same stay caught with another record fails.
		expect_row(judge_case(turret(1, 0, 65535), turret(2, 0, 65535), turret(3, 0, 7), "OR"), 'S',
				"frame_mount", "", "fail", "FRAME_PASSENGER_PHASE_MISMATCH");
		// A stay of a full cycle on both sides compares whether or not a run carried it.
		auto seated = [&rewrite](uint32_t seed, bool carry) {
			return rewrite(scripted(seed),
					[carry](size_t i, wire::ParityFrameState &f, wire::ParityEntity &e) {
						const bool mounted = i >= 40 && i < 70;
						f.flags2 = uint32_t(i) & 0xFFu;
						f.mount_present = (f.flags2 & 0x0Fu) == 8u && (carry || !mounted);
						f.mount_handle = mounted ? 0x1002 : 0xFFFF;
						f.mount_bound = mounted;
						e.carrier = mounted ? 0x1002 : 0xFFFF;
						e.carrier_bone = mounted ? 4 : 0;
						e.carrier_local = mounted;
					});
		};
		expect_row(judge_case(seated(1, true), seated(2, true), seated(3, false), "OR"), 'S',
				"frame_mount", "", "fail", "FRAME_PASSENGER_PHASE_MISMATCH");
	}
	{
		// x6's death eye: one curve sampled at another phase by each run, the rests and the
		// transition between them shared; a tilt between two standing rests is its own
		// transition (x6 hostkill OR before D-NET-518's roll fix).
		static constexpr int32_t kStand = -0xD001, kDead = -9889;
		auto dying = [&rewrite](uint32_t seed, int phase, bool tilt) {
			return rewrite(scripted(seed),
					[phase, tilt](size_t i, wire::ParityFrameState &, wire::ParityEntity &e) {
						e.offset_x = e.offset_y = 0;
						e.offset_z = kStand;
						if (tilt && i == 60)
							e.offset_x = 3840;
						if (i >= 100 && i < 104) {
							const int k = int(i - 100) * 3 + phase; // the tick each frame sampled
							e.offset_z = kStand + (kDead - kStand) * (k + 1) / 13;
						} else if (i >= 104) {
							e.offset_z = kDead;
						}
					});
		};
		const Case phased =
				judge_case(dying(1, 0, false), dying(2, 1, false), dying(3, 2, false), "OO");
		expect_row(phased, 'S', "frame_self_offset", "", "pass");
		const VerdictRow *row = find_row(phased.verdict.rows, 'S', "frame_self_offset");
		check(row && row->value.reference == "{0,0,-13,0,0,-13>0,0,-2,0,0,-2}",
				"the death compares as its rests and one transition (got " +
						(row ? row->value.reference : std::string("no row")) + ")");
		expect_row(judge_case(dying(1, 0, false), dying(2, 1, false), dying(3, 2, true), "OR"), 'S',
				"frame_self_offset", "", "fail", "FRAME_ANCHOR_OFFSET_MISMATCH");
		// x8's aikill: each death drew its own clip (the own record's anim state, state_b's
		// high bits: RR 196, RR-aa 198, OO 216) and rested at that clip's eye (-9217 on RR and
		// RR-aa, -8193 then -13153 on OO). A rest in a clip only one run plays is 'drawn'.
		auto clipped = [&rewrite](uint32_t seed, uint32_t clip, int32_t first, int32_t last) {
			return rewrite(scripted(seed),
					[clip, first, last](size_t i, wire::ParityFrameState &, wire::ParityEntity &e) {
						e.offset_x = e.offset_y = 0;
						e.offset_z = kStand;
						e.state_b = (44u << 8) | 9u;
						if (i < 100)
							return;
						e.state_b = (clip << 8) | 9u;
						if (i < 104)
							e.offset_z = kStand + (first - kStand) * int32_t(i - 99) / 5;
						else
							e.offset_z = i < 120 ? first : last;
					});
		};
		const Case drawn = judge_case(clipped(1, 196, -9217, -9217), clipped(2, 198, -9217, -9217),
				clipped(3, 216, -8193, -13153), "OO");
		expect_row(drawn, 'S', "frame_self_offset", "", "pass");
		const VerdictRow *drawn_row = find_row(drawn.verdict.rows, 'S', "frame_self_offset");
		check(drawn_row && drawn_row->aa_distance == 0.0 &&
						drawn_row->value.subject == "{0,0,-13,0,0,-13>drawn,drawn}",
				"two death clips' rests compare as drawn (got " +
						(drawn_row ? drawn_row->value.subject : std::string("no row")) + ")");
		// The clip RR played, resting elsewhere, compares by value.
		expect_row(judge_case(clipped(1, 196, -9217, -9217), clipped(2, 198, -9217, -9217),
						   clipped(3, 196, -13153, -13153), "OO"),
				'S', "frame_self_offset", "", "fail", "FRAME_ANCHOR_OFFSET_MISMATCH");
	}
	{
		// x5's aikill: RR's first hit left 36, RR-aa's 8 (base 114 at zone 1.0 and 1.25).
		auto hit = [&rewrite](uint32_t seed, int32_t full, int32_t residual) {
			return rewrite(scripted(seed),
					[full, residual](size_t i, wire::ParityFrameState &f, wire::ParityEntity &) {
						f.health = i < 100 ? full : i < 150 ? residual : 0;
					});
		};
		expect_row(judge_case(hit(1, 150, 8), hit(2, 150, 8), hit(3, 150, 36), "OO"), 'S',
				"frame_health", "", "pass");
		// x8's aikill OR: 150 straight to 0 between two consecutive frames, the hit and the
		// kill inside one frame step: no residual to compare.
		expect_row(judge_case(hit(1, 150, 8), hit(2, 150, 8), hit(3, 150, 0), "OO"), 'S',
				"frame_health", "", "pass");
		// A run that never dies still misses the dead value.
		auto alive = rewrite(scripted(3),
				[](size_t, wire::ParityFrameState &f, wire::ParityEntity &) { f.health = 150; });
		expect_row(judge_case(hit(1, 150, 8), hit(2, 150, 8), alive, "OO"), 'S', "frame_health", "",
				"fail", "FRAME_HEADER_VALUES_MISMATCH");
		expect_row(judge_case(hit(1, 150, 8), hit(2, 150, 8), hit(3, 120, 36), "OO"), 'S',
				"frame_health", "", "fail", "FRAME_HEADER_VALUES_MISMATCH");
	}
	{
		// Push 208's aikill OR: the AI's 75 hit landed between the last frame before the
		// window (150) and the window's first frame (75), so the window opens on 150
		// (frame.health_open). Every frame of the run is written, the window's by its
		// steady index.
		auto opened = [](uint32_t seed, int32_t before, int32_t first, int32_t residual) {
			wire::ParityTrace trace = scripted(seed);
			size_t index = 0;
			for (wire::ParityFrameState &f : trace.frames) {
				if (f.ts_ns < kSteadyStart) {
					f.health = before;
					continue;
				}
				const size_t i = index++;
				f.health = i < 50 ? first : i < 100 ? residual : 0;
			}
			return trace;
		};
		expect_row(judge_case(opened(1, 150, 150, 36), opened(2, 150, 150, 8),
						   opened(3, 150, 75, 75), "OR"),
				'S', "frame_health", "", "pass");
		// A hit the last frame before the window already carried leaves full unseen.
		expect_row(judge_case(opened(1, 150, 150, 36), opened(2, 150, 150, 8),
						   opened(3, 75, 75, 75), "OR"),
				'S', "frame_health", "", "fail", "FRAME_HEADER_VALUES_MISMATCH");
		// A 0 before a living window is an earlier life's (a respawn at the open): it opens
		// nothing, so a run that never dies inside the window fails against a straight death
		// inside it ({150} against {150, 0}, where the prepended 0 would have read {0, 150}).
		auto reborn = [](uint32_t seed) {
			wire::ParityTrace trace = scripted(seed);
			for (wire::ParityFrameState &f : trace.frames)
				f.health = f.ts_ns < kSteadyStart ? 0 : 150;
			return trace;
		};
		expect_row(judge_case(opened(1, 150, 150, 0), opened(2, 150, 150, 0), reborn(3), "OR"),
				'S', "frame_health", "", "fail", "FRAME_HEADER_VALUES_MISMATCH");
		// Both sides reborn at the open, neither dying inside the window: {150} on both.
		expect_row(judge_case(reborn(1), reborn(2), reborn(3), "OR"), 'S', "frame_health", "",
				"pass");
	}
	{
		// x6's hostkill: the +0x168 countdown (weapon[1]) from `top` and the +0x170 one
		// (weapon[2]) from 120, both started at steady frame `start` `skipped` seconds in, a
		// second every 8 frames (two weapon blocks), +0x170 still running at the window's end
		// (frame.weapon_countdown).
		auto countdown = [&rewrite](uint32_t seed, size_t start, uint32_t skipped, uint32_t top) {
			return rewrite(scripted(seed),
					[start, skipped, top](
							size_t i, wire::ParityFrameState &f, wire::ParityEntity &) {
						if (!f.weapon_present)
							return;
						f.weapon[1] = f.weapon[2] = 0;
						if (i < start)
							return;
						const uint32_t step = uint32_t(i - start) / 8 + skipped;
						f.weapon[1] = step < top ? top - step : 0;
						f.weapon[2] = 120 - step;
					});
		};
		// RR-aa's first sample is the second second (x6: (4, 119) against RR's (5, 120)).
		const Case late_sample =
				judge_case(countdown(1, 40, 0, 5), countdown(2, 48, 1, 5), countdown(3, 48, 1, 5));
		const VerdictRow *aa = find_row(late_sample.verdict.rows, 'S', "frame_weapon");
		check(aa && aa->aa_distance == 0.0, "a countdown's first second missed is no A/A distance");
		expect_row(late_sample, 'S', "frame_weapon", "", "pass");
		// A countdown started two seconds later ends two higher at the window's end
		// (x6: 79 on RR, 76 on OO).
		expect_row(judge_case(countdown(1, 40, 0, 5), countdown(2, 40, 0, 5),
						   countdown(3, 56, 0, 5), "OO"),
				'S', "frame_weapon", "", "pass");
		// A countdown from another value is no phase.
		expect_row(judge_case(countdown(1, 40, 0, 5), countdown(2, 40, 0, 5),
						   countdown(3, 40, 0, 7), "OO"),
				'S', "frame_weapon", "", "fail", "FRAME_WEAPON_PHASE_MISMATCH");
		// x8's swap: the team change starts +0x168 and +0x16C (weapon[1], weapon[3]) at 60 and
		// the re-arm a second or less later +0x164 (weapon[4]) at 30, the per-slot pass between
		// the two accepts or not: RR (60, 60, 30) and RR-aa a second later (59, 59, 29) stand 30
		// apart, OR (59, 59, 30) 29 apart.
		auto swap = [&rewrite](uint32_t seed, uint32_t skipped, uint32_t apart) {
			return rewrite(scripted(seed),
					[skipped, apart](size_t i, wire::ParityFrameState &f, wire::ParityEntity &) {
						if (!f.weapon_present)
							return;
						f.weapon[1] = f.weapon[3] = f.weapon[4] = 0;
						if (i < 40)
							return;
						const uint32_t step = uint32_t(i - 40) / 8 + skipped;
						f.weapon[1] = f.weapon[3] = step < 60 ? 60 - step : 0;
						f.weapon[4] = step + apart < 60 ? 60 - apart - step : 0;
					});
		};
		const Case passed = judge_case(swap(1, 0, 30), swap(2, 1, 30), swap(3, 1, 29), "OR");
		const VerdictRow *swap_row = find_row(passed.verdict.rows, 'S', "frame_weapon");
		check(swap_row && swap_row->aa_distance == 0.0,
				"a countdown pair started a second later is no A/A distance");
		expect_row(passed, 'S', "frame_weapon", "", "pass");
		// Three seconds apart is no pass between two accepts.
		expect_row(judge_case(swap(1, 0, 30), swap(2, 1, 30), swap(3, 1, 27), "OR"), 'S',
				"frame_weapon", "", "fail", "FRAME_WEAPON_PHASE_MISMATCH");
	}
}

// x8's 01tr-coop: a parked vehicle's records carry its offset from the moving anchor rounded to
// the compressed step (state.position_quantum), so its world position wobbles inside one step
// and is no motion; a vehicle that drives further than its step still compares.
void test_position_quantum() {
	// The vehicle record (handle 0x2003) at world x 4000 + speed * k for its k-th record.
	auto vehicle = [](uint32_t seed, int32_t speed) {
		wire::ParityTrace trace = scripted(seed);
		std::map<int, const wire::ParityFrameState *> frames;
		for (const wire::ParityFrameState &f : trace.frames)
			frames[f.frame] = &f;
		int32_t k = 0;
		for (wire::ParityEntity &e : trace.entities) {
			if (e.cls != 'V')
				continue;
			const wire::ParityFrameState &f = *frames.at(e.frame);
			e.pos_x = network_compress_fixedpoint(4000 * 65536 + speed * k++ - f.anchor_x);
			e.pos_y = network_compress_fixedpoint(2100 * 65536 - f.anchor_y);
			e.pos_z = network_compress_fixedpoint(50 * 65536 - f.anchor_z);
			place(e, f);
		}
		return trace;
	};
	{
		const Case parked = judge_case(vehicle(1, 0), vehicle(2, 0), vehicle(3, 0), "OO");
		for (const char *row : { "entity_V_position_range_p90",
					 "entity_V_position_displacement_p90", "entity_V_position_step_p90" })
			check(find_row(parked.verdict.rows, 'S', row) == nullptr,
					std::string("a parked vehicle has no ") + row);
	}
	constexpr int32_t kDrive = 8 * 65536; // 8 units a record, beyond the 2-unit step
	expect_row(judge_case(vehicle(1, kDrive), vehicle(2, kDrive), vehicle(3, kDrive), "OO"), 'S',
			"entity_V_position_displacement_p90", "", "pass");
	expect_row(judge_case(vehicle(1, kDrive), vehicle(2, kDrive), vehicle(3, 0), "OO"), 'S',
			"entity_V_position_displacement_p90", "", "fail", "DYNAMIC_RANGE_MISMATCH");
}

// x8's 01tr-coop (L7-s19): a civilian standing on the building under its placed spot streams
// its position and heading local to that building (entity.carrier), where our x8 host sent
// world records. Each handle's records run one sequence per carrier word: a walk on the
// building compares with the same walk in world units, a still record against a walk fails,
// and a change of carrier is a change of frame, never a step or a turn.
void test_entity_carrier() {
	// The civilian's pose at its k-th record and capture time: the carrier word (0xFFFF: none,
	// the position a world one), the position in that frame (16.16) and the heading byte.
	struct Pose {
		uint32_t carrier = 0xFFFF;
		int32_t x = 0, y = 0;
		int32_t yaw = 0;
	};
	using Walk = std::function<Pose(int32_t k, uint64_t ts_ns)>;
	auto civilian = [](uint32_t seed, const Walk &walk) {
		wire::ParityTrace trace = scripted(seed);
		std::map<int, const wire::ParityFrameState *> frames;
		for (const wire::ParityFrameState &f : trace.frames)
			frames[f.frame] = &f;
		std::vector<wire::ParityEntity> entities;
		int32_t k = 0;
		for (const wire::ParityEntity &v : trace.entities) {
			entities.push_back(v);
			if (v.cls != 'V')
				continue;
			const wire::ParityFrameState &f = *frames.at(v.frame);
			const Pose pose = walk(k++, v.ts_ns);
			wire::ParityEntity e = v;
			e.cls = 'I';
			e.handle = 0x0032;
			e.type_id = 2069;
			e.orient_a = pose.yaw;
			e.orient_b = 0;
			e.state_a = e.state_b = e.vital = 0;
			e.carrier = pose.carrier;
			e.carrier_local = pose.carrier != 0xFFFF;
			if (e.carrier_local) {
				// The decode: a carrier-local offset is the position in the carrier's frame.
				e.pos_x = network_compress_fixedpoint(pose.x);
				e.pos_y = network_compress_fixedpoint(pose.y);
				e.pos_z = network_compress_fixedpoint(-45000);
				place(e, f);
				e.world_x = e.offset_x;
				e.world_y = e.offset_y;
				e.world_z = e.offset_z;
			} else {
				e.pos_x = network_compress_fixedpoint(pose.x - f.anchor_x);
				e.pos_y = network_compress_fixedpoint(pose.y - f.anchor_y);
				e.pos_z = network_compress_fixedpoint(52 * 65536 - f.anchor_z);
				place(e, f);
			}
			entities.push_back(e);
		}
		trace.entities = std::move(entities);
		return trace;
	};
	static constexpr int32_t kUnit = 65536;
	const Walk on_building = [](int32_t k, uint64_t) {
		return Pose{ 0x213F, 2 * kUnit + k * kUnit, kUnit / 2, 40 };
	};
	const Walk in_world = [](int32_t k, uint64_t) {
		return Pose{ 0xFFFF, -265 * kUnit + k * kUnit, 755 * kUnit, 90 };
	};
	const Walk still_on_building = [](int32_t, uint64_t) {
		return Pose{ 0x213F, 2 * kUnit, kUnit / 2, 40 };
	};
	{
		const Case c = judge_case(
				civilian(1, on_building), civilian(2, on_building), civilian(3, in_world), "OO");
		expect_row(c, 'S', "entity_motion", "I", "pass");
		expect_row(c, 'S', "entity_I_position_step_p90", "", "pass");
		expect_row(c, 'S', "entity_I_position_range_p90", "", "pass");
	}
	{
		const Case c = judge_case(civilian(1, on_building), civilian(2, on_building),
				civilian(3, still_on_building), "OO");
		expect_row(c, 'S', "entity_motion", "I", "fail", "FRAME_ENTITY_CONSTANT");
		expect_row(c, 'S', "entity_I_position_range_p90", "", "fail", "DYNAMIC_RANGE_MISMATCH");
	}
	{
		// A still civilian whose carrier changes (x8's 46: 0x206D, then 0x213D) has two
		// constant sequences: no step, no turn, against a still world record.
		const Walk two_buildings = [](int32_t, uint64_t ts_ns) {
			const bool later = ts_ns >= 12 * kSec;
			return later ? Pose{ 0x213D, -40 * kUnit, 12 * kUnit, 200 }
						 : Pose{ 0x206D, 3 * kUnit, kUnit, 40 };
		};
		const Walk still_in_world = [](int32_t, uint64_t) {
			return Pose{ 0xFFFF, -265 * kUnit, 755 * kUnit, 90 };
		};
		const Case c = judge_case(civilian(1, two_buildings), civilian(2, two_buildings),
				civilian(3, still_in_world), "OO");
		expect_row(c, 'S', "entity_motion", "I", "pass");
		for (const char *row : { "entity_I_position_range_p90", "entity_I_position_step_p90",
					 "entity_I_orientation_range_p90", "entity_I_orientation_step_p90" })
			check(find_row(c.verdict.rows, 'S', row) == nullptr,
					std::string("a change of carrier makes no ") + row);
	}
}

void test_steady_rules() {
	{
		// A steady packet groups as its record multiset, without the RTT replies its
		// gameplay record carries; a packet of replies alone keeps them.
		wire::ParityTrace reordered = scripted(3), alone = scripted(3);
		for (wire::ParityPacket &p : reordered.packets)
			if (p.dir == 'S' && p.tags.size() == 2 && p.tags[0] == 0x00A && p.tags[1] == 0x02C) {
				std::swap(p.tags[0], p.tags[1]);
				std::swap(p.records[0], p.records[1]);
			}
		for (wire::ParityPacket &p : reordered.packets)
			if (p.dir == 'S' && p.tags.size() == 1 && p.tags[0] == 0x00A && p.frame % 3 == 0) {
				p.tags.push_back(0x057);
				p.records.push_back(p.records.front());
				p.records.back().full_tag = 0x057;
				p.records.back().encoded_length = 5;
			}
		for (wire::ParityPacket &p : alone.packets)
			if (p.dir == 'S' && p.tags.size() == 1 && p.tags[0] == 0x00A && p.frame % 3 == 0) {
				p.tags[0] = 0x057;
				p.records.front().full_tag = 0x057;
			}
		expect_row(judge_case(scripted(1), scripted(2), reordered), 'S',
				"steady_group_distribution", "", "pass");
		expect_row(judge_case(scripted(1), scripted(2), alone), 'S', "steady_group_distribution",
				"", "fail", "GAMEPLAY_GROUP_DISTRIBUTION");
	}
	{
		// The order runs over the producers' records (group.S.0x57.order): x7's ash-capture
		// OR mostly packed [0x57, 0x40, 0x0A] where RR packed [0x40, 0x57, 0x0A], the 0x57's
		// place its C2S 0x2C's arrival in the host's send window. The producers' own order
		// still compares.
		auto packed = [](uint32_t seed, const std::vector<uint16_t> &order) {
			wire::ParityTrace trace = scripted(seed);
			for (wire::ParityPacket &p : trace.packets)
				if (p.dir == 'S' && p.tags.size() == 1 && p.tags[0] == 0x00A && p.frame % 3 == 0) {
					const wire::ParityWireRecord gameplay = p.records.front();
					p.tags.clear();
					p.records.clear();
					for (const uint16_t tag : order) {
						wire::ParityWireRecord record = gameplay;
						record.full_tag = tag;
						if (tag != 0x00A)
							record.encoded_length = tag == 0x057 ? 5 : 25;
						p.tags.push_back(tag);
						p.records.push_back(record);
					}
				}
			return trace;
		};
		const std::vector<uint16_t> retail{ 0x040, 0x057, 0x046, 0x00A },
				early{ 0x057, 0x040, 0x046, 0x00A }, swapped{ 0x057, 0x046, 0x040, 0x00A };
		expect_row(judge_case(packed(1, retail), packed(2, retail), packed(3, early)), 'S',
				"steady_order_distribution", "", "pass");
		expect_row(judge_case(packed(1, retail), packed(2, retail), packed(3, swapped)), 'S',
				"steady_order_distribution", "", "fail", "GAMEPLAY_ORDER_DISTRIBUTION");
	}
	{
		// A share distribution compares at 13 samples or more on both sides
		// (steady.distribution_floor): one sample's 1 / n share stays under the 0.08
		// unit. x5's kick held 11 to 14 steady packets before the punt.
		auto trimmed = [](uint32_t seed, size_t keep) {
			wire::ParityTrace trace = scripted(seed);
			std::vector<wire::ParityPacket> packets;
			size_t steady = 0;
			for (const wire::ParityPacket &p : trace.packets) {
				if (p.dir == 'S' && p.ts_ns >= kSteadyStart && !p.records.empty() &&
						steady++ >= keep)
					continue;
				packets.push_back(p);
			}
			trace.packets = std::move(packets);
			return trace;
		};
		expect_row(judge_case(trimmed(1, 13), trimmed(2, 13), trimmed(3, 13)), 'S',
				"steady_group_distribution", "", "skip");
		expect_row(judge_case(trimmed(1, 14), trimmed(2, 14), trimmed(3, 14)), 'S',
				"steady_group_distribution", "", "pass");
		// One side short skips whichever run it is: x4..x6's kick retail runs held 10 to
		// 14 steady packets before the punt, so a subject under 13 against an RR over
		// it is the punt's phase, not an unexercised stream.
		constexpr size_t kAll = ~size_t(0);
		expect_row(judge_case(trimmed(1, kAll), trimmed(2, kAll), trimmed(3, 13)), 'S',
				"steady_group_distribution", "", "skip");
		expect_row(judge_case(trimmed(1, 13), trimmed(2, kAll), trimmed(3, kAll)), 'S',
				"steady_group_distribution", "", "skip");
	}
}

// --- the round events' fire target (round.target)

struct RoundRun {
	uint32_t shooter;
	uint32_t flags;
	size_t count;
};

// x8 / x9 01tr-coop-aikill: each shooter's S2C 0x0A round events to the joiner 0x005D by flag
// byte, as L4-s31 counted them over the cells' decodes, every 0x40 target the joiner. The
// rifles key I.alt (0x01 / 0x41), the mounted gunner 0x0006 I.adm (0x32 / 0x72).
const std::vector<RoundRun> kAikillRR = { { 0x0019, 0x01, 4 }, { 0x0019, 0x41, 6 },
	{ 0x0027, 0x01, 2 }, { 0x0027, 0x41, 10 }, { 0x0028, 0x01, 4 }, { 0x0028, 0x41, 8 },
	{ 0x0001, 0x01, 9 }, { 0x0001, 0x41, 3 }, { 0x0000, 0x41, 1 }, { 0x0006, 0x32, 1 },
	{ 0x0006, 0x72, 8 } };
const std::vector<RoundRun> kAikillRRaa = { { 0x0019, 0x41, 5 }, { 0x0027, 0x01, 4 },
	{ 0x0027, 0x41, 8 }, { 0x0028, 0x41, 6 }, { 0x0001, 0x01, 9 }, { 0x0001, 0x41, 3 },
	{ 0x0000, 0x41, 2 }, { 0x0006, 0x72, 6 } };
const std::vector<RoundRun> kAikillRO = { { 0x0019, 0x01, 4 }, { 0x0019, 0x41, 6 },
	{ 0x0027, 0x41, 12 }, { 0x0028, 0x01, 3 }, { 0x0028, 0x41, 9 }, { 0x0001, 0x01, 6 },
	{ 0x0001, 0x41, 6 }, { 0x0000, 0x41, 1 }, { 0x0006, 0x72, 13 } };
// Our host before D-NET-NEW-L4-27 (x8 and x9 OR / OO): every rifle round 0x01, the gunner's
// 0x32, none naming a target (RR's per-shooter totals).
const std::vector<RoundRun> kAikillOursBefore = { { 0x0019, 0x01, 10 }, { 0x0027, 0x01, 12 },
	{ 0x0028, 0x01, 12 }, { 0x0001, 0x01, 12 }, { 0x0000, 0x01, 1 }, { 0x0006, 0x32, 9 } };

constexpr uint32_t kAikillJoiner = 0x005D;

// The scripted case with the joiner as 0x005D, the aikill shooters' records in the window's
// first frame only and `runs` one round per frame after it, each 0x40 round naming `target`:
// a round's shooter resolves to its latest earlier record's class.
wire::ParityTrace aikill(
		uint32_t seed, const std::vector<RoundRun> &runs, uint32_t target = kAikillJoiner) {
	wire::ParityTrace trace = scripted(seed);
	for (wire::ParityUplinkState &u : trace.uplinks)
		u.handle = kAikillJoiner;
	for (wire::ParityEntity &e : trace.entities)
		if (e.handle == 0x1000)
			e.handle = kAikillJoiner;
	std::vector<wire::ParityFrameState> window;
	for (const wire::ParityFrameState &f : trace.frames)
		if (f.ts_ns >= kSteadyStart && f.ts_ns <= kSteadyStart + uint64_t(kSteadySeconds * 1e9))
			window.push_back(f);
	for (const uint32_t shooter : { 0x0000u, 0x0001u, 0x0006u, 0x0019u, 0x0027u, 0x0028u }) {
		wire::ParityEntity e;
		e.frame = window.front().frame;
		e.ts_ns = window.front().ts_ns;
		e.session = kClientPort;
		e.cls = 'I';
		e.handle = shooter;
		e.type_id = 1799;
		e.pos_x = 4000 + shooter;
		e.pos_y = 4000;
		e.pos_z = 100;
		place(e, window.front());
		trace.entities.push_back(e);
	}
	size_t k = 1;
	for (const RoundRun &run : runs)
		for (size_t i = 0; i < run.count; ++i, ++k) {
			wire::ParityRoundEvent round;
			round.frame = window.at(k).frame;
			round.ts_ns = window.at(k).ts_ns;
			round.session = kClientPort;
			round.shooter = run.shooter;
			round.flags = run.flags;
			round.adm = (run.flags & 0x02) ? 38 : 21;
			round.target = (run.flags & 0x40) ? target : 0xFFFF;
			round.seq = uint32_t(k);
			trace.rounds.push_back(round);
		}
	return trace;
}

// Whether a kind of shooter's round events ever name a target, and whom, never their share:
// retail's tag-2 writer reads the shooter's AI slot word live at the send [orig:
// NetPacket_SerializeRoundEvent @0x504820 (@0x50484E / @0x504855, the flag
// @0x5048BF..0x5048C1)], so the rounds before it acquires the joiner go without one.
void test_round_target() {
	auto share = [](const std::vector<RoundRun> &runs) {
		size_t targeted = 0, total = 0;
		for (const RoundRun &run : runs) {
			total += run.count;
			if (run.flags & 0x40)
				targeted += run.count;
		}
		return std::to_string(targeted) + "/" + std::to_string(total);
	};
	check(share(kAikillRR) == "36/56" && share(kAikillRRaa) == "30/43" &&
					share(kAikillRO) == "47/60" && share(kAikillOursBefore) == "0/56",
			"the aikill oracle's per-cell shares (L4-s31)");
	{
		// Retail against retail: the shares differ, the rows do not.
		const Case c =
				judge_case(aikill(1, kAikillRR), aikill(2, kAikillRRaa), aikill(3, kAikillRO));
		for (const char *kind : { "I.alt", "I.adm" }) {
			expect_row(c, 'S', "round_target", kind, "pass");
			expect_row(c, 'S', "round_target_relation", kind, "pass");
		}
		const VerdictRow *rr = find_row(c.verdict.rows, 'S', "round_target", "I.alt");
		check(rr && rr->value.reference == "filled (28 of 47)" &&
						rr->value.subject == "filled (34 of 47)",
				"round_target renders each side's targeted rounds of the kind");
	}
	{
		// Our host before L4-27: the rows x8 and x9's OR / OO would have failed on.
		const Case c = judge_case(
				aikill(1, kAikillRR), aikill(2, kAikillRRaa), aikill(3, kAikillOursBefore), "OR");
		expect_row(c, 'S', "round_target", "I.alt", "fail", "ROUND_TARGET_MISSING");
		expect_row(c, 'S', "round_target", "I.adm", "fail", "ROUND_TARGET_MISSING");
		expect_row(c, 'S', "round_target_relation", "I.alt", "skip");
	}
	{
		// Every round naming the joiner (no acquisition phase at all) is no divergence.
		const std::vector<RoundRun> every = { { 0x0019, 0x41, 10 }, { 0x0027, 0x41, 12 },
			{ 0x0028, 0x41, 12 }, { 0x0001, 0x41, 12 }, { 0x0000, 0x41, 1 }, { 0x0006, 0x72, 9 } };
		const Case c =
				judge_case(aikill(1, kAikillRR), aikill(2, kAikillRRaa), aikill(3, every), "OO");
		expect_row(c, 'S', "round_target", "I.alt", "pass");
		expect_row(c, 'S', "round_target", "I.adm", "pass");
		expect_row(c, 'S', "round_target_relation", "I.adm", "pass");
	}
	{
		// Whom: another entity, or the word for a pointer in no pool (@0x504882..0x504886).
		const Case other = judge_case(
				aikill(1, kAikillRR), aikill(2, kAikillRRaa), aikill(3, kAikillRO, 0x0042), "OO");
		expect_row(other, 'S', "round_target", "I.alt", "pass");
		expect_row(other, 'S', "round_target_relation", "I.alt", "fail",
				"ROUND_TARGET_RELATION_MISMATCH");
		const Case unpooled = judge_case(
				aikill(1, kAikillRR), aikill(2, kAikillRRaa), aikill(3, kAikillRO, 0xFFFF), "OO");
		expect_row(unpooled, 'S', "round_target_relation", "I.adm", "fail",
				"ROUND_TARGET_RELATION_MISMATCH");
		const VerdictRow *row =
				find_row(unpooled.verdict.rows, 'S', "round_target_relation", "I.adm");
		check(row && row->value.subject == "{unpooled}" && row->value.reference == "{self}",
				"the relation renders 0xFFFF as unpooled and the joiner as self");
	}
	{
		// The other way: RR's rounds never name a target and the run's do.
		const Case c = judge_case(
				aikill(1, kAikillOursBefore), aikill(2, kAikillOursBefore), aikill(3, kAikillRO));
		expect_row(c, 'S', "round_target", "I.alt", "fail", "ROUND_TARGET_UNEXPECTED");
	}
	{
		// A kind the run never fires is short: it fails unless RR-aa never fired it either.
		const std::vector<RoundRun> rifles(kAikillRO.begin(), kAikillRO.end() - 1);
		const std::vector<RoundRun> rifles_aa(kAikillRRaa.begin(), kAikillRRaa.end() - 1);
		expect_row(judge_case(aikill(1, kAikillRR), aikill(2, kAikillRRaa), aikill(3, rifles)), 'S',
				"round_target", "I.adm", "fail");
		expect_row(judge_case(aikill(1, kAikillRR), aikill(2, rifles_aa), aikill(3, rifles)), 'S',
				"round_target", "I.adm", "pass");
	}
	{
		// No round in any run: no round rows.
		const Case c = judge_case(scripted(1), scripted(2), scripted(3));
		check(find_row(c.verdict.rows, 'S', "round_target", "I.alt") == nullptr,
				"a case with no round events has no round rows");
	}
}

} // namespace

int main() {
	test_registry();
	test_body_masks();
	test_cadence_counts();
	test_pool_live_pose();
	test_tag_window();
	test_stream_anchor_and_burst_tail();
	test_stream_anchor_rotation();
	test_rotation_load_gap();
	test_dead_joiner_uplink();
	test_session_end();
	test_timestamps();
	test_identical();
	test_tag_histogram();
	test_aa_widening();
	test_initial_stream();
	test_steady_and_envelope();
	test_handshake();
	test_integrity();
	test_reload_echo();
	test_milestones_and_rates();
	test_state();
	test_dynamic_rules();
	test_evidence();
	test_document();
	test_handshake_rules();
	test_spawn_pump_boundary();
	test_initial_rules();
	test_world_records();
	test_reply_rules();
	test_order_rules();
	test_order_clock();
	test_steady_rules();
	test_frame_rules();
	test_frame_draws();
	test_position_quantum();
	test_entity_carrier();
	test_round_target();
	if (g_failures) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("OK\n");
	return 0;
}
