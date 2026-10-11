// opennova-parity's command line over run directories shaped like
// scripts/net/run_parity_topology.ps1's cells (run-summary.json beside the
// captures), built from the committed self-capture session: a real encrypted
// session the shared decoder walks end to end (fixtures/novaworld, LFS, zero retail
// bytes; its timestamps are 16 ms per datagram). Pins the exit codes (0 PASS,
// 1 FAIL, 2 usage or input error), the PARITY_* stdout contract, the verdict file,
// the evidence-capture rebase onto a moved run directory, --evidence external, and
// one real executable run.
//
// Usage: parity_cli_test <path to opennova-parity>

#include "cli.h"
#include "parity_trace.h"

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/pcapio/pcap_reader.h>
#include <base/pcapio/pcap_writer.h>

#include "common/run_command.h"
#include "common/temp_dir.h"

#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#ifndef SELF_CAPTURE_FIXTURE
#define SELF_CAPTURE_FIXTURE ""
#endif
#ifndef ITEMS_FIXTURE
#define ITEMS_FIXTURE ""
#endif

using namespace opennova;

namespace {

int g_failures = 0;

void check(bool cond, const std::string &what) {
	if (cond)
		return;
	std::fprintf(stderr, "FAIL: %s\n", what.c_str());
	++g_failures;
}

struct Result {
	int code = -1;
	std::string out, err;
};

std::string slurp(std::FILE *f) {
	std::string text;
	std::rewind(f);
	char buf[4096];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		text.append(buf, n);
	return text;
}

Result run(const std::vector<std::string> &args) {
	std::FILE *out = std::tmpfile();
	std::FILE *err = std::tmpfile();
	Result r;
	if (!out || !err) {
		check(false, "tmpfile");
		return r;
	}
	r.code = parity::run_cli(args, out, err);
	r.out = slurp(out);
	r.err = slurp(err);
	std::fclose(out);
	std::fclose(err);
	return r;
}

bool contains(const std::string &text, const std::string &what) {
	return text.find(what) != std::string::npos;
}

void write_pcap(const std::string &path, const std::vector<net::PcapDatagram> &datagrams) {
	const std::vector<uint8_t> bytes = net::build_pcap_udp(datagrams);
	std::string error;
	check(io::write_file_atomic(path, bytes.data(), bytes.size(), error), "write " + path);
}

// A run directory: its capture under joiner/ and a summary recording an absolute
// path under a run root that no longer exists (the directory moved), so the
// comparator must rebase it.
std::string make_run(const test_temp::TempDir &dir, const std::string &name,
		const std::string &topology, const std::vector<net::PcapDatagram> &capture,
		const std::string &steady_started = "1970-01-01T00:00:00.9000000Z",
		const std::string &mission = "SELF.BMS") {
	const std::string run_dir = dir.file(name.c_str());
	std::error_code ec;
	std::filesystem::create_directories(io::os_path(run_dir + "/joiner"), ec);
	write_pcap(run_dir + "/joiner/traffic.pcap", capture);
	write_pcap(run_dir + "/external.pcapng", capture);
	const std::string old_root = "C:/moved-away/.scratch/runs/" + name;
	io::JsonValue json = io::JsonValue::make_object();
	json.set("topology", io::json_string(topology));
	json.set("mission", io::json_string(mission));
	json.set("game_type", io::json_string("65568"));
	json.set("port", io::json_number(32786));
	json.set("steady_seconds", io::json_number(0.5));
	json.set("steady_started_utc", io::json_string(steady_started));
	json.set("steady_completed_utc", io::json_string("1970-01-01T00:00:01.4500000Z"));
	json.set("run_id", io::json_string(name));
	json.set("run_root", io::json_string(old_root));
	json.set("evidence_capture", io::json_string(old_root + "/joiner/traffic.pcap"));
	json.set("evidence_perspective", io::json_string("retail-client"));
	std::string error;
	const std::string text = "\xEF\xBB\xBF" + io::json_write(json); // PowerShell 5.1 writes a BOM
	check(io::write_file_atomic(run_dir + "/run-summary.json", text, error), "write summary");
	return run_dir;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: parity_cli_test <opennova-parity>\n");
		return 2;
	}
	const std::string exe = argv[1];
	const std::string items = ITEMS_FIXTURE;
	std::vector<net::PcapDatagram> session;
	if (!net::read_pcap_udp_file(SELF_CAPTURE_FIXTURE, session) || session.size() < 50) {
		std::fprintf(stderr, "FAIL: the self-capture fixture is missing or an LFS pointer (%s)\n",
				SELF_CAPTURE_FIXTURE);
		return 1;
	}

	// The subject that diverges: every other S2C datagram carrying a 0x0A frame in
	// the steady window dropped, so the host's frame cadence halves.
	wire::ParityTrace decoded;
	check(wire::read_parity_trace(SELF_CAPTURE_FIXTURE, nullptr, decoded), "decode the fixture");
	std::set<int> frame_datagrams;
	for (const wire::ParityPacket &packet : decoded.packets)
		if (packet.dir == 'S' && packet.ts_ns >= 900000000ull)
			for (const uint16_t tag : packet.tags)
				if (tag == 0x00A)
					frame_datagrams.insert(packet.frame);
	std::vector<net::PcapDatagram> halved;
	size_t seen = 0;
	for (const net::PcapDatagram &d : session)
		if (!frame_datagrams.count(d.frame_index) || seen++ % 2 == 0)
			halved.push_back(d);
	check(frame_datagrams.size() >= 6, "the fixture streams S2C 0x0A in the window");

	test_temp::TempDir dir("parity_cli");
	const std::string rr = make_run(dir, "case-rr", "RR", session);
	const std::string aa = make_run(dir, "case-rr-aa", "RR", session);
	const std::string ro = make_run(dir, "case-ro", "RO", session);
	const std::string oo = make_run(dir, "case-oo", "OO", session);
	const std::string ro_bad = make_run(dir, "case-ro-bad", "RO", halved);
	const std::string verdict = dir.file("verdict.json");
	const std::vector<std::string> base = { "--items", items, "--rr", rr, "--rr-aa", aa };

	{
		std::vector<std::string> args = base;
		args.insert(args.end(), { "--ro", ro, "--oo", oo, "--out", verdict });
		const Result r = run(args);
		check(r.code == parity::kExitPass,
				"identical runs PASS (exit " + std::to_string(r.code) + "): " + r.out + r.err);
		check(contains(r.out, "PARITY_STATUS=PASS\n"), "PARITY_STATUS=PASS");
		check(contains(r.out, "PARITY_FAILURES=0\n"), "PARITY_FAILURES=0");
		check(contains(r.out, "PARITY_REPORT=" + verdict + "\n"), "PARITY_REPORT names the file");
		std::string text, error;
		io::JsonValue json;
		check(io::read_file_text(verdict, text, error) && io::json_parse(text, json, error),
				"the verdict file is JSON");
		check(json.get_string("schema", "") == "opennova.parity-verdict.v1", "verdict schema");
		check(json.get_string("status", "") == "PASS", "verdict status");
		const io::JsonValue *runs = json.get("runs");
		check(runs && runs->get("ro") && runs->get("oo") && runs->get("rr_aa"),
				"the verdict lists every run");
		check(runs && runs->get("rr") &&
						runs->get("rr")->get_string("capture", "") == rr + "/joiner/traffic.pcap",
				"the summary's capture path is rebased onto the moved run directory");
	}
	{
		std::vector<std::string> args = base;
		args.insert(args.end(), { "--ro", ro_bad, "--out", verdict });
		const Result r = run(args);
		check(r.code == parity::kExitFail,
				"a halved frame cadence FAILs (exit " + std::to_string(r.code) + "): " + r.out +
						r.err);
		check(contains(r.out, "PARITY_STATUS=FAIL\n"), "PARITY_STATUS=FAIL");
		check(contains(r.out, "metric=gameplay_cadence") &&
						contains(r.out, "code=GAMEPLAY_CADENCE"),
				"a PARITY_FAIL line names the metric and code: " + r.out);
	}
	{
		std::vector<std::string> args = base;
		args.insert(args.end(), { "--ro", ro, "--out", verdict, "--evidence", "external" });
		const Result r = run(args);
		check(r.code == parity::kExitPass, "--evidence external reads external.pcapng: " + r.err);
	}
	{
		// The A/A sanity: RR-aa as the reference, RR as both the floor and the subject.
		const Result r = run({ "--items", items, "--rr", aa, "--rr-aa", rr, "--ro", rr, "--out",
				verdict });
		check(r.code == parity::kExitPass,
				"an RR run in a subject slot is judged and passes (exit " + std::to_string(r.code) +
						"): " + r.out + r.err);
		std::string text, error;
		io::JsonValue json;
		check(io::read_file_text(verdict, text, error) && io::json_parse(text, json, error),
				"the A/A sanity verdict is JSON");
		const io::JsonValue *runs = json.get("runs");
		check(runs && runs->get("ro") && runs->get("rr") &&
						runs->get("ro")->get_string("topology", "") == "RR" &&
						runs->get("rr")->get_string("run_id", "") == "case-rr-aa",
				"the subject keeps its slot's entry beside the reference's");
	}

	// Usage and input errors: exit 2, nothing judged.
	const std::string ro_wrong = make_run(dir, "case-ro-is-oo", "OO", session);
	const std::string bad_time =
			make_run(dir, "case-bad-time", "RO", session, "1970-01-01T00:00:00.9+00:00");
	const std::string no_capture = make_run(dir, "case-no-capture", "RO", session);
	{
		std::error_code ec;
		std::filesystem::remove(io::os_path(no_capture + "/joiner/traffic.pcap"), ec);
	}
	const struct {
		const char *what;
		std::vector<std::string> args;
	} kUsage[] = {
		{ "no arguments", {} },
		{ "an unknown argument", { "--bogus" } },
		{ "a missing --rr-aa", { "--items", items, "--rr", rr, "--ro", ro } },
		{ "no subject", base },
		{ "a flag without its value", { "--items", items, "--rr", rr, "--rr-aa" } },
		{ "an unreadable items.def",
				{ "--items", dir.file("none.def"), "--rr", rr, "--rr-aa", aa, "--ro", ro } },
		{ "a run directory without a summary",
				{ "--items", items, "--rr", rr, "--rr-aa", aa, "--ro", dir.root() } },
		{ "a subject of the wrong topology",
				{ "--items", items, "--rr", rr, "--rr-aa", aa, "--ro", ro_wrong } },
		{ "RR as its own A/A", { "--items", items, "--rr", rr, "--rr-aa", rr, "--ro", ro } },
		{ "a malformed steady timestamp",
				{ "--items", items, "--rr", rr, "--rr-aa", aa, "--ro", bad_time } },
		{ "a missing evidence capture",
				{ "--items", items, "--rr", rr, "--rr-aa", aa, "--ro", no_capture } },
		{ "a bad --evidence",
				{ "--items", items, "--rr", rr, "--rr-aa", aa, "--ro", ro, "--evidence",
						"joiner" } },
	};
	for (const auto &usage : kUsage) {
		const Result r = run(usage.args);
		check(r.code == parity::kExitUsage,
				std::string(usage.what) + " exits 2 (got " + std::to_string(r.code) + ")");
		check(!contains(r.out, "PARITY_STATUS="), std::string(usage.what) + " judges nothing");
	}
	{
		const Result r = run({ "--help" });
		check(r.code == parity::kExitPass && contains(r.out, "usage: opennova-parity"), "--help");
	}

	// The executable itself.
	const std::string pass_cmd = test_cmd::quoted(exe) + " --items " + test_cmd::quoted(items) +
			" --rr " + test_cmd::quoted(rr) + " --rr-aa " + test_cmd::quoted(aa) + " --ro " +
			test_cmd::quoted(ro) + " --out " + test_cmd::quoted(verdict) + " > " +
			test_cmd::quoted(dir.file("exe.txt"));
	check(test_cmd::run(pass_cmd) == 0, "opennova-parity exits 0 on PASS");
	std::string exe_out, error;
	check(io::read_file_text(dir.file("exe.txt"), exe_out, error) &&
					contains(exe_out, "PARITY_STATUS=PASS"),
			"opennova-parity prints PARITY_STATUS=PASS");
	const std::string usage_cmd =
			test_cmd::quoted(exe) + " > " + test_cmd::quoted(dir.file("usage.txt")) + " 2>&1";
	check(test_cmd::run(usage_cmd) != 0, "opennova-parity without arguments exits nonzero");

	if (g_failures) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("OK\n");
	return 0;
}
