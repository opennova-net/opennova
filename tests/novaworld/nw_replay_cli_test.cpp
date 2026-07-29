// nw_replay CLI + playback core regression.
//
// Two defect classes, both invisible from the outside before this test existed:
//
//  1. Numeric options went through atoi/atof, which report failure as 0 and stop
//     at the first bad character. "--speed junk" silently meant 0, "--max-gap-ms
//     5x" silently meant 5, and "--speed nan" produced a NaN that slipped past a
//     `speed <= 0` guard (all NaN comparisons are false) into the pacing math,
//     where int64_t(NaN) is undefined behavior.
//  2. Playback discarded both the capture-read result and every send result, so
//     a mid-file read error or a dead spectator socket still printed "playback
//     complete" and exited 0.
//
// run_playback takes its stream, its send, and its clock as parameters, so the
// failures below are injected directly instead of staged with real sockets.

#include "nw_replay_cli.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "common/test_expect.h"

using namespace opennova;

namespace {

bool rejects(std::initializer_list<const char *> args) {
	std::vector<const char *> argv;
	argv.push_back("nw_replay");
	for (const char *a : args) {
		argv.push_back(a);
	}
	replay::Args parsed;
	std::string error;
	const bool ok = replay::parse_args(static_cast<int>(argv.size()), argv.data(), parsed, error);
	if (ok) {
		std::fprintf(stderr, "  accepted what it should reject:");
		for (const char *a : argv) std::fprintf(stderr, " %s", a);
		std::fprintf(stderr, "\n");
		return false;
	}
	if (error.empty()) {
		std::fprintf(stderr, "  rejected without a reason\n");
		return false;
	}
	return true;
}

net::PcapDatagram make_datagram(uint64_t ts_nanos, uint8_t tag) {
	net::PcapDatagram d;
	d.ts_nanos = ts_nanos;
	d.srcport = 1000;
	d.dstport = 2000;
	d.payload = {tag};
	return d;
}

// A capture streamer over a fixed datagram list. `read_ok` is what the real
// reader would return: false on bad magic / IO.
replay::StreamFn fake_stream(const std::vector<net::PcapDatagram> &dgrams, bool read_ok) {
	return [&dgrams, read_ok](const std::function<bool(const net::PcapDatagram &)> &on) {
		for (const net::PcapDatagram &d : dgrams) {
			if (!on(d)) break;
		}
		return read_ok;
	};
}

int test_strict_numeric_options() {
	// The four values that used to sail through.
	TEST_EXPECT(rejects({"cap.pcap", "--speed", "nan"}));
	TEST_EXPECT(rejects({"cap.pcap", "--speed", "inf"}));
	TEST_EXPECT(rejects({"cap.pcap", "--speed", "-inf"}));
	TEST_EXPECT(rejects({"cap.pcap", "--speed", "junk"}));
	// Partial conversions: atof stopped at the bad character and kept the prefix.
	TEST_EXPECT(rejects({"cap.pcap", "--speed", "2x"}));
	TEST_EXPECT(rejects({"cap.pcap", "--max-gap-ms", "5x"}));
	TEST_EXPECT(rejects({"cap.pcap", "--max-gap-ms", ""}));
	// Overflow.
	TEST_EXPECT(rejects({"cap.pcap", "--speed", "1e999"}));
	TEST_EXPECT(rejects({"cap.pcap", "--max-gap-ms", "99999999999999999999"}));
	// Out of range, including the zero/negative speeds the old clamp hid.
	TEST_EXPECT(rejects({"cap.pcap", "--speed", "0"}));
	TEST_EXPECT(rejects({"cap.pcap", "--speed", "-1"}));
	TEST_EXPECT(rejects({"cap.pcap", "--control-port", "0"}));
	TEST_EXPECT(rejects({"cap.pcap", "--control-port", "70000"}));
	TEST_EXPECT(rejects({"cap.pcap", "--max-gap-ms", "-1"}));
	// Structural.
	TEST_EXPECT(rejects({"cap.pcap", "--speed"}));
	TEST_EXPECT(rejects({"cap.pcap", "--bogus"}));
	TEST_EXPECT(rejects({"a.pcap", "b.pcap"}));
	TEST_EXPECT(rejects({"--print-roles"})); // no capture

	// And a well-formed line still lands intact.
	const char *argv[] = {"nw_replay", "cap.pcapng", "--control-port", "42001",
	                      "--speed", "2.5", "--max-gap-ms", "100",
	                      "--spectator-timeout-ms", "5000", "--loop", "--print-roles"};
	replay::Args a;
	std::string error;
	TEST_EXPECT(replay::parse_args(12, argv, a, error));
	TEST_EXPECT(a.pcap == "cap.pcapng");
	TEST_EXPECT(a.control_port == 42001);
	TEST_EXPECT(a.speed == 2.5);
	TEST_EXPECT(a.max_gap_ms == 100);
	TEST_EXPECT(a.spectator_timeout_ms == 5000);
	TEST_EXPECT(a.loop);
	TEST_EXPECT(a.print_roles);
	return 0;
}

int test_pacing() {
	// 100 ms of capture time at 1x, under the clamp.
	TEST_EXPECT(replay::paced_gap_ns(100000000ull, 1.0, 250) == 100000000.0);
	// Same gap at 2x plays in half the time.
	TEST_EXPECT(replay::paced_gap_ns(100000000ull, 2.0, 250) == 50000000.0);
	// A long idle stretch is clamped so the spectator does not stall on it.
	TEST_EXPECT(replay::paced_gap_ns(60000000000ull, 1.0, 250) == 250000000.0);
	return 0;
}

int test_playback_reports_failures() {
	const std::vector<net::PcapDatagram> dgrams = {
	    make_datagram(0, 1), make_datagram(1000000, 2), make_datagram(2000000, 3)};
	auto keep_all = [](const net::PcapDatagram &) { return true; };
	auto no_sleep = [](int64_t) {};

	// Happy path: every selected datagram sent, nothing hidden.
	{
		const replay::PlaybackReport r = replay::run_playback(
		    fake_stream(dgrams, true), keep_all,
		    [](const net::PcapDatagram &) { return 1; }, no_sleep, 1.0, 250);
		TEST_EXPECT(r.ok());
		TEST_EXPECT(r.sent == 3);
		TEST_EXPECT(r.send_failures == 0);
	}

	// Only the selected datagrams are sent.
	{
		const replay::PlaybackReport r = replay::run_playback(
		    fake_stream(dgrams, true),
		    [](const net::PcapDatagram &d) { return d.payload[0] != 2; },
		    [](const net::PcapDatagram &) { return 1; }, no_sleep, 1.0, 250);
		TEST_EXPECT(r.ok());
		TEST_EXPECT(r.sent == 2);
	}

	// Capture read failure: reported, never "playback complete".
	{
		const replay::PlaybackReport r = replay::run_playback(
		    fake_stream(dgrams, false), keep_all,
		    [](const net::PcapDatagram &) { return 1; }, no_sleep, 1.0, 250);
		TEST_EXPECT(!r.ok());
		TEST_EXPECT(!r.read_ok);
	}

	// Send failure: counted, and the pass is not a success.
	{
		const replay::PlaybackReport r = replay::run_playback(
		    fake_stream(dgrams, true), keep_all,
		    [](const net::PcapDatagram &d) { return d.payload[0] == 2 ? -1 : 1; }, no_sleep,
		    1.0, 250);
		TEST_EXPECT(!r.ok());
		TEST_EXPECT(r.read_ok);
		TEST_EXPECT(r.sent == 2);
		TEST_EXPECT(r.send_failures == 1);
	}

	// Every send failing is still a complete read — both facts survive.
	{
		const replay::PlaybackReport r = replay::run_playback(
		    fake_stream(dgrams, true), keep_all,
		    [](const net::PcapDatagram &) { return -1; }, no_sleep, 1.0, 250);
		TEST_EXPECT(!r.ok());
		TEST_EXPECT(r.sent == 0);
		TEST_EXPECT(r.send_failures == 3);
	}
	return 0;
}

} // namespace

int main() {
	if (test_strict_numeric_options()) return 1;
	if (test_pacing()) return 1;
	if (test_playback_reports_failures()) return 1;
	std::printf("PASS: nw_replay rejects malformed numeric options and reports "
	            "capture-read and send failures\n");
	return 0;
}
