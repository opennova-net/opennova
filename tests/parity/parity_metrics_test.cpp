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

#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <set>
#include <string>
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
		f.flags2 = f.sub_block;
		f.anchor_x = int32_t(1000 * 65536 + tick * 16384 + int32_t(seed_) * 65536);
		f.anchor_y = 2000 * 65536;
		f.anchor_z = 50 * 65536;
		f.local_tail = true;
		f.health = 100;
		f.records = 2;
		f.players = 1;
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
		wire::ParityEntity p;
		p.frame = frame;
		p.ts_ns = at;
		p.session = kClientPort;
		p.cls = 'P';
		p.handle = 0x1000;
		p.type_id = 5305;
		p.pos_x = uint32_t(tick * 64 % 60000);
		p.pos_y = 3000 + seed_;
		p.pos_z = 100;
		p.orient_a = (tick * 5) % 256;
		p.orient_b = 2;
		p.state_a = 4;
		p.vital = 100;
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
		trace.entities.push_back(v);
	}

	uint32_t seed_;
	int frame_ = 0;
	uint32_t seq_c_ = 0, seq_s_ = 0;
};

constexpr uint64_t kSteadyStart = 5 * kSec;
constexpr double kSteadySeconds = 15.0;

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
		tlv_u32(b, "EIP", 0x0100007F + seed);
		tlv_u32(b, "EPN", 30000 + seed);
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
		tlv_text(b, "SCRK", "K" + std::to_string(100000 + seed * 31337));
		tlv_cu(b, 1, "UdpCode1", std::to_string(1000 + seed * 7));
		tlv_cu(b, 2, "Callsign", "ParityJoin");
		s.handshake(t + 20 * kMs, 0x42, b);
	}
	{
		std::vector<uint8_t> b;
		tlv_u32(b, "CI", seed * 11);
		tlv_u32(b, "MI", seed);
		tlv_u32(b, "SK", 0xABC00000 + seed);
		tlv_text(b, "SCRK", "S" + std::to_string(10000 + seed * 4241));
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
			{ { 0x046, bytes({ int(seed), 0, 1 }) }, { 0x02C, bytes({ 5, 5, 5, 5 }) } });
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
		// The host challenges another ammo row: unwitnessed for any subject, and a
		// row-set mismatch only where the host is OpenNova.
		wire::ParityTrace subject = scripted(3);
		for_events(subject, 'S', 0x031, [](wire::ParityEvent &e) { e.body[0] = 0x19; });
		for_events(subject, 'C', 0x021, [](wire::ParityEvent &e) { e.body[0] = 0x19; });
		const Case ro = judge_case(scripted(1), scripted(2), subject, "RO");
		expect_row(
				ro, 'B', "integrity_row_unwitnessed", "ammo", "fail", "INTEGRITY_ROW_UNWITNESSED");
		check(find_row(ro.verdict.rows, 'B', "integrity_rows", "ammo") == nullptr,
				"a retail host's rows are not the subject's");
		const Case oo = judge_case(scripted(1), scripted(2), subject, "OO");
		expect_row(oo, 'B', "integrity_rows", "ammo", "fail", "HOST_AMMO_ROW_MISMATCH");
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
		wire::ParityTrace subject = scripted(3);
		for (wire::ParityEntity &e : subject.entities)
			if (e.cls == 'P')
				e.pos_x = 777;
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

} // namespace

int main() {
	test_registry();
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
	if (g_failures) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("OK\n");
	return 0;
}
