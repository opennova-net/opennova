#include <net/npruntime/client_runtime.h>

#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace np = opennova::np;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

std::vector<uint8_t> frame_server_session(
		SessionSequencing &sequencing, const std::string &server_scrk,
		uint32_t client_key, uint8_t tag, std::vector<uint8_t> body) {
	std::vector<uint8_t> payload;
	if (!frame_session_packet(
			sequencing, SessionCrypto{server_scrk, {}, client_key},
			{make_protocol_message(tag, std::move(body))}, payload))
		return {};
	return nw_encode_outbound(
			SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(payload));
}

bool feed(np::ClientRuntime &runtime, SessionSequencing &server_tx,
		const std::string &server_scrk, uint8_t tag,
		std::vector<uint8_t> body, uint32_t tick) {
	const std::vector<uint8_t> datagram = frame_server_session(
			server_tx, server_scrk, 1u, tag, std::move(body));
	if (datagram.empty()) return false;
	runtime.receive(datagram.data(), datagram.size());
	(void)runtime.Client_ProcessNetworkFrame(tick);
	return true;
}

void append_u16(std::vector<uint8_t> &out, uint16_t value) {
	out.push_back(static_cast<uint8_t>(value));
	out.push_back(static_cast<uint8_t>(value >> 8));
}

void append_u32(std::vector<uint8_t> &out, uint32_t value) {
	out.push_back(static_cast<uint8_t>(value));
	out.push_back(static_cast<uint8_t>(value >> 8));
	out.push_back(static_cast<uint8_t>(value >> 16));
	out.push_back(static_cast<uint8_t>(value >> 24));
}

std::vector<uint8_t> make_terrain_header_page(
		const std::vector<uint8_t> &til, uint16_t end_index) {
	std::vector<uint8_t> body;
	append_u16(body, 0xFFFFu);
	append_u16(body, end_index);
	body.insert(body.end(), til.begin(), til.begin() + 16u + 12u * end_index);
	return body;
}

std::vector<uint8_t> make_terrain_continuation(
		const std::vector<uint8_t> &til, uint16_t start_index,
		uint16_t end_index) {
	std::vector<uint8_t> body;
	append_u16(body, start_index);
	append_u16(body, end_index);
	body.insert(body.end(), til.begin() + 16u + 12u * start_index,
			til.begin() + 16u + 12u * end_index);
	return body;
}

std::vector<uint8_t> make_til(
		uint32_t advertised_count, uint32_t record_count) {
	std::vector<uint8_t> til;
	append_u32(til, 0x74696C30u);
	append_u32(til, advertised_count);
	append_u32(til, 0x78563412u);
	append_u32(til, 0xF0DEBC9Au);
	for (uint32_t i = 0; i < record_count * 12u; ++i)
		til.push_back(static_cast<uint8_t>(0x20u + i));
	return til;
}

struct TerrainFixture {
	std::string server_scrk = "SERVER-TERRAIN-ASSET-SCRK";
	np::ClientRuntime runtime{"TerrainAssetJoiner"};
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();
	uint32_t tick = 0;

	TerrainFixture() {
		runtime.seed_session(
				0x50607080u, 1u, "CLIENT-TERRAIN-ASSET-SCRK", server_scrk,
				1u, 0u, 0x0002u, 0x14B9u);
	}

	bool send(std::vector<uint8_t> body) {
		return feed(runtime, server_tx, server_scrk, 0x45u,
				std::move(body), ++tick);
	}
};

bool run_exact_bms_header_is_retained_and_reset() {
	const std::string client_scrk = "CLIENT-WIRE-ASSET-SCRK";
	const std::string server_scrk = "SERVER-WIRE-ASSET-SCRK";
	np::ClientRuntime runtime("WireAssetJoiner");
	runtime.seed_session(
			0x10203040u, 1u, client_scrk, server_scrk,
			1u, 0u, 0x0002u, 0x14B9u);
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();

	if (!expect(!runtime.has_mission_header() &&
				runtime.mission_header_bytes().empty(),
			"fresh runtime has no retained S2C 0x0B header"))
		return false;

	std::vector<uint8_t> short_body(615u, 0xA5u);
	if (!feed(runtime, server_tx, server_scrk, 0x0Bu,
			std::move(short_body), 1u))
		return expect(false, "frame short S2C 0x0B");
	if (!expect(!runtime.has_mission_header(),
			"615-byte S2C 0x0B is not retained"))
		return false;

	std::vector<uint8_t> exact_header(616u);
	for (std::size_t i = 0; i < exact_header.size(); ++i)
		exact_header[i] = static_cast<uint8_t>((i * 73u + 19u) & 0xFFu);
	exact_header[0] = 'B';
	exact_header[1] = 'M';
	exact_header[2] = 'S';
	exact_header[3] = 19u;
	if (!feed(runtime, server_tx, server_scrk, 0x0Bu,
			exact_header, 2u))
		return expect(false, "frame exact S2C 0x0B");
	if (!expect(runtime.has_mission_header() &&
				runtime.mission_header_bytes() == exact_header,
			"exact 616-byte S2C 0x0B is retained byte-for-byte"))
		return false;

	std::vector<uint8_t> long_body(617u, 0x5Au);
	if (!feed(runtime, server_tx, server_scrk, 0x0Bu,
			std::move(long_body), 3u))
		return expect(false, "frame long S2C 0x0B");
	if (!expect(runtime.has_mission_header() &&
				runtime.mission_header_bytes() == exact_header,
			"617-byte S2C 0x0B cannot replace the last exact header"))
		return false;

	if (!expect(!runtime.start().empty(), "reconnect emits a fresh ClientHello"))
		return false;
	return expect(!runtime.has_mission_header() &&
				runtime.mission_header_bytes().empty(),
			"reconnect clears the retained S2C 0x0B header");
}

bool run_paged_terrain_is_reassembled_exactly_and_reset() {
	TerrainFixture fixture;
	const std::vector<uint8_t> til = make_til(3u, 3u);

	if (!expect(fixture.runtime.terrain_til_state() ==
				np::TerrainTilState::Absent &&
				!fixture.runtime.has_terrain_til() &&
				fixture.runtime.terrain_til_bytes().empty(),
			"fresh runtime has no complete S2C 0x45 terrain image"))
		return false;

	if (!fixture.send(make_terrain_header_page(til, 2u)))
		return expect(false, "frame first S2C 0x45 page");
	if (!expect(fixture.runtime.terrain_til_state() ==
				np::TerrainTilState::Receiving &&
				!fixture.runtime.has_terrain_til() &&
				fixture.runtime.terrain_til_bytes().empty(),
			"partial terrain stream is not exposed as a complete .til"))
		return false;

	if (!fixture.send(make_terrain_continuation(til, 2u, 3u)))
		return expect(false, "frame final S2C 0x45 page");
	if (!expect(fixture.runtime.terrain_til_state() ==
				np::TerrainTilState::Complete &&
				fixture.runtime.has_terrain_til() &&
				fixture.runtime.terrain_til_bytes() == til,
			"paged S2C 0x45 stream reconstructs exact .til bytes"))
		return false;

	if (!expect(!fixture.runtime.start().empty(),
			"terrain reconnect emits ClientHello"))
		return false;
	return expect(fixture.runtime.terrain_til_state() ==
				np::TerrainTilState::Absent &&
				!fixture.runtime.has_terrain_til() &&
				fixture.runtime.terrain_til_bytes().empty(),
			"reconnect clears the completed S2C 0x45 terrain image");
}

bool run_invalid_terrain_streams_fail_closed_and_reset() {
	const std::vector<uint8_t> til = make_til(3u, 3u);
	auto expect_invalid = [](const TerrainFixture &fixture, const char *message) {
		return expect(fixture.runtime.terrain_til_state() ==
					np::TerrainTilState::Invalid &&
					!fixture.runtime.has_terrain_til() &&
					fixture.runtime.terrain_til_bytes().empty(), message);
	};

	{
		TerrainFixture fixture;
		if (!fixture.send({}) || !expect_invalid(fixture,
				"an actual empty S2C 0x45 is Invalid, not optional absence"))
			return false;
		if (!expect(!fixture.runtime.start().empty() &&
					fixture.runtime.terrain_til_state() ==
						np::TerrainTilState::Absent,
				"reconnect resets an Invalid terrain stream to Absent"))
			return false;
	}
	{
		TerrainFixture fixture;
		if (!fixture.send(make_terrain_continuation(til, 2u, 3u)) ||
				!expect_invalid(fixture,
						"a continuation before its header is Invalid"))
			return false;
		if (!fixture.send(make_terrain_header_page(til, 2u)) ||
				!expect_invalid(fixture,
						"Invalid terrain state remains sticky"))
			return false;
	}
	{
		TerrainFixture fixture;
		if (!fixture.send(make_terrain_header_page(til, 1u)) ||
				!fixture.send(make_terrain_continuation(til, 2u, 3u)) ||
				!expect_invalid(fixture,
						"a gap in the canonical retail-writer cursor is Invalid"))
			return false;
	}
	{
		TerrainFixture fixture;
		if (!fixture.send(make_terrain_header_page(til, 2u)) ||
				!fixture.send(make_terrain_continuation(til, 1u, 3u)) ||
				!expect_invalid(fixture,
						"an overlapping retail-writer page is Invalid"))
			return false;
	}
	{
		TerrainFixture fixture;
		const std::vector<uint8_t> overflow = make_til(3u, 4u);
		if (!fixture.send(make_terrain_header_page(overflow, 2u)) ||
				!fixture.send(make_terrain_continuation(overflow, 2u, 4u)) ||
				!expect_invalid(fixture,
						"a page end beyond the advertised tile count is Invalid"))
			return false;
	}
	{
		TerrainFixture fixture;
		if (!fixture.send(make_terrain_header_page(til, 1u)) ||
				!fixture.send(make_terrain_header_page(til, 2u)) ||
				!expect_invalid(fixture,
						"a second terrain header is Invalid"))
			return false;
	}
	{
		TerrainFixture fixture;
		if (!fixture.send(make_terrain_header_page(til, 0u)) ||
				!expect_invalid(fixture,
						"a zero-length header page is Invalid"))
			return false;
	}
	{
		TerrainFixture fixture;
		const std::vector<uint8_t> oversized = make_til(32768u, 1u);
		if (!fixture.send(make_terrain_header_page(oversized, 1u)) ||
				!expect_invalid(fixture,
						"a tile count beyond retail's signed writer cursor is Invalid"))
			return false;
	}
	{
		TerrainFixture fixture;
		const std::vector<uint8_t> one = make_til(1u, 1u);
		if (!fixture.send(make_terrain_header_page(one, 1u)) ||
				fixture.runtime.terrain_til_state() !=
					np::TerrainTilState::Complete ||
				!fixture.send(make_terrain_header_page(one, 1u)) ||
				!expect_invalid(fixture,
						"a semantic page after completion is Invalid"))
			return false;
	}
	return true;
}

} // namespace

int main() {
	const bool ok = run_exact_bms_header_is_retained_and_reset() &&
			run_paged_terrain_is_reassembled_exactly_and_reset() &&
			run_invalid_terrain_streams_fail_closed_and_reset();
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
