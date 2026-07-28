// Retail GSB dispatcher semantics (NapiGameList_ProcessEncryptedResponse
// @ 0x63d740, docs/net/novaworld-net-re.md D-NET-190..193, 2026-07-27 grill):
//
//   1. SVRS records ACCUMULATE across the stream — retail appends rows with no
//      per-record clear [orig: append loop @ 0x63dbec..0x63dc0d] (D-NET-191).
//      A multi-SVRS blob must yield the union of all records, not the last one.
//   2. A "GSB " record is a RESET — frees the field table and every accumulated
//      row — but only when its payload begins with u32 0x00010000
//      [orig: @ 0x63d8f2] (D-NET-192); any other "GSB " payload is skipped.
//   3. Undersized records are skipped, not errors: FLDS payload < 2
//      [orig: @ 0x63d7c2], SVRS payload < 2 [orig: @ 0x63da43], "GSB "
//      payload < 4 [orig: @ 0x63d8f2 guard].
//   4. A blob that never reaches "XXXX" is rejected by OUR parser (D-NET-193
//      host hardening): retail is an incremental HTTP callback with no error
//      path — its parse offset just persists — but a one-shot host parser must
//      require the terminator.
//
// Streams are composed by re-sequencing chunks cut from real gsb_build_response
// output (each chunk payload is independently encrypted, so raw chunks can be
// spliced freely), plus hand-framed chunks for the malformed cases.

#include <novacrypto/nwu.h>
#include <novaworld/gsb.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int g_failures = 0;

bool expect(bool cond, const char *msg) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		++g_failures;
	}
	return cond;
}

uint32_t read_le32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
	       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Cut a built blob into raw [magic][u32 len][encrypted payload] chunks.
std::vector<std::vector<uint8_t>> split_chunks(const std::vector<uint8_t> &blob) {
	std::vector<std::vector<uint8_t>> chunks;
	size_t pos = 0;
	while (pos + 8 <= blob.size()) {
		const uint32_t len = read_le32(blob.data() + pos + 4);
		const size_t end = pos + 8 + len;
		if (end > blob.size()) break;
		chunks.emplace_back(blob.begin() + pos, blob.begin() + end);
		pos = end;
	}
	return chunks;
}

// Hand-frame one chunk; the payload is encrypted with the ADD chain
// (nwu_decrypt) exactly like the builder, so the parser's SUBTRACT-chain
// decode recovers it.
std::vector<uint8_t> make_chunk(const char (&magic)[5], std::vector<uint8_t> payload) {
	if (!payload.empty()) {
		nwu_decrypt(payload.data(), payload.size(), GSB_NWU_KEY);
	}
	std::vector<uint8_t> out;
	out.insert(out.end(), magic, magic + 4);
	const uint32_t len = static_cast<uint32_t>(payload.size());
	out.push_back(static_cast<uint8_t>(len & 0xFFu));
	out.push_back(static_cast<uint8_t>((len >> 8) & 0xFFu));
	out.push_back(static_cast<uint8_t>((len >> 16) & 0xFFu));
	out.push_back(static_cast<uint8_t>((len >> 24) & 0xFFu));
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

std::vector<uint8_t> concat(const std::vector<std::vector<uint8_t>> &parts) {
	std::vector<uint8_t> out;
	for (const auto &p : parts) out.insert(out.end(), p.begin(), p.end());
	return out;
}

} // namespace

int main() {
	GsbServerEntry a{};
	a.rid = 101;
	a.ip = "203.0.113.7";
	a.server_name = "Alpha";

	GsbServerEntry b{};
	b.rid = 202;
	b.ip = "198.51.100.23";
	b.server_name = "Bravo";

	// blob_x chunks: [0]="GSB " init  [1]=FLDS  [2]=SVRS  [3]=XXXX.
	const auto ca = split_chunks(gsb_build_response({a}));
	const auto cb = split_chunks(gsb_build_response({b}));
	if (!expect(ca.size() == 4 && cb.size() == 4, "builder emits 4 chunks")) return 1;
	const auto &init_a = ca[0];
	const auto &flds_a = ca[1];
	const auto &svrs_a = ca[2];
	const auto &xxxx = ca[3];
	const auto &init_b = cb[0];
	const auto &flds_b = cb[1];
	const auto &svrs_b = cb[2];

	// 1. Accumulation: two SVRS records (an unknown "IVAR"-style tag between
	//    them, walked past like retail) -> the union of rows, in stream order.
	{
		const auto wire = concat({init_a, flds_a, svrs_a,
		                          make_chunk("IVAR", {1, 2, 3}), svrs_b, xxxx});
		GsbResponse r;
		if (expect(gsb_parse_response(wire.data(), wire.size(), r),
		           "multi-SVRS blob parses") &&
		    expect(r.servers.size() == 2, "two SVRS records accumulate to 2 rows")) {
			expect(r.servers[0].rid == 101 && r.servers[0].ip == "203.0.113.7" &&
			           r.servers[0].server_name == "Alpha",
			       "row 0 is the first record's server");
			expect(r.servers[1].rid == 202 && r.servers[1].ip == "198.51.100.23" &&
			           r.servers[1].server_name == "Bravo",
			       "row 1 is the second record's server");
			expect(r.total_servers == 2, "total_servers counts accumulated rows");
		}
	}

	// 2a. Mid-stream valid "GSB " reset: everything before it is discarded.
	{
		const auto wire = concat({init_a, flds_a, svrs_a,
		                          init_b, flds_b, svrs_b, xxxx});
		GsbResponse r;
		if (expect(gsb_parse_response(wire.data(), wire.size(), r),
		           "reset blob parses") &&
		    expect(r.servers.size() == 1, "valid GSB reset clears accumulated rows")) {
			expect(r.servers[0].rid == 202, "only the post-reset server survives");
		}
	}

	// 2b. "GSB " record whose payload dword0 != 0x00010000 is skipped — NOT a
	//     reset (retail checks the prefix before freeing anything @ 0x63d8f2).
	{
		const auto wire = concat({init_a, flds_a, svrs_a,
		                          make_chunk("GSB ", {0xEF, 0xBE, 0xAD, 0xDE}),
		                          svrs_b, xxxx});
		GsbResponse r;
		if (expect(gsb_parse_response(wire.data(), wire.size(), r),
		           "mismatched GSB record blob parses") &&
		    expect(r.servers.size() == 2, "mismatched GSB record does not reset")) {
			expect(r.servers[0].rid == 101 && r.servers[1].rid == 202,
			       "both rows survive the skipped GSB record");
		}
	}

	// 3. Undersized records are skipped in place: a 1-byte SVRS, a 1-byte FLDS
	//    (must not clobber the field table), and a 3-byte "GSB " (must not
	//    reset). The real rows around them still parse.
	{
		const auto wire = concat({init_a, flds_a,
		                          make_chunk("SVRS", {0x42}), svrs_a,
		                          make_chunk("FLDS", {0x42}),
		                          make_chunk("GSB ", {0x00, 0x00, 0x01}),
		                          svrs_b, xxxx});
		GsbResponse r;
		if (expect(gsb_parse_response(wire.data(), wire.size(), r),
		           "undersized-record blob parses") &&
		    expect(r.servers.size() == 2, "undersized records skipped, rows kept")) {
			expect(r.servers[0].server_name == "Alpha" &&
			           r.servers[1].server_name == "Bravo",
			       "field table survives the undersized FLDS record");
		}
		expect(r.field_names.size() == 26, "undersized FLDS did not clobber the table");
	}

	// 4. No "XXXX" terminator -> rejected (host hardening; retail would just
	//    keep waiting for more HTTP body).
	{
		const auto wire = concat({init_a, flds_a, svrs_a});
		GsbResponse r;
		expect(!gsb_parse_response(wire.data(), wire.size(), r),
		       "blob without XXXX terminator rejected");
	}

	if (g_failures == 0) {
		std::printf("OK: retail GSB dispatcher semantics verified "
		            "(accumulate / gated reset / undersized skip / terminator)\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
