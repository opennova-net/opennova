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
		const char *topology = "RO") {
	Case c;
	c.rr = make_run_facts(info_for("rr", "RR"), std::move(rr));
	c.aa = make_run_facts(info_for("rr_aa", "RR"), std::move(aa));
	c.subject = make_run_facts(info_for("subject", topology), std::move(subject));
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
		add_initial(extra, 'C', 0x022, 0x022, bytes({ 1 }));
		expect_row(judge_case(scripted(1), scripted(2), split), 'C', "initial_grouping", "",
				"pass");
		expect_row(judge_case(scripted(1), scripted(2), extra), 'C', "initial_grouping", "",
				"fail", "INITIAL_PACKET_GROUPING");
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
		const Case flags = judge_case(with(1, organic, base_statics),
				with(2, organic, base_statics), with(3, organic, statics(0, 0, 0x00020000)));
		expect_row(flags, 'S', "initial_records", "0x10:0", "fail", "INITIAL_RECORD_MISMATCH");
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
		// S2C 0x46 is no milestone (the scoreboard's phase); C2S 0x09 -> S2C 0x2C is.
		wire::ParityTrace early = scripted(3), late = scripted(3);
		for_events(early, 'S', 0x046, [](wire::ParityEvent &e) { e.frame = 0; });
		expect_row(judge_case(scripted(1), scripted(2), early), 'B', "milestones", "", "pass");
		int frame_0f = 0;
		for_events(late, 'S', 0x00F, [&frame_0f](wire::ParityEvent &e) { frame_0f = e.frame; });
		for_events(late, 'C', 0x009, [frame_0f](wire::ParityEvent &e) { e.frame = frame_0f; });
		expect_row(judge_case(scripted(1), scripted(2), late), 'B', "milestones", "", "fail",
				"MILESTONE_MISMATCH");
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
}

} // namespace

int main() {
	test_registry();
	test_body_masks();
	test_cadence_counts();
	test_pool_live_pose();
	test_timestamps();
	test_identical();
	test_tag_histogram();
	test_aa_widening();
	test_initial_stream();
	test_steady_and_envelope();
	test_handshake();
	test_integrity();
	test_milestones_and_rates();
	test_state();
	test_evidence();
	test_document();
	test_handshake_rules();
	test_initial_rules();
	test_world_records();
	test_reply_rules();
	test_order_rules();
	test_steady_rules();
	test_frame_rules();
	if (g_failures) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("OK\n");
	return 0;
}
