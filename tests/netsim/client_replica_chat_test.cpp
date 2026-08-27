// The player-chat lane fold (S2C 0x14): the D-NET-215 byte order
// [channel][sender_slot][cstr], the drain, and the HUD channel table that
// routes each line to its ring and colour.
// [orig: NapiNPClientMsg_ChatMessage @0x42f240 -> Chat_DispatchToChannel
//  @0x42b910; the colours HUD_InitTeamColorTable @0x51f245..0x51f2b3]
#include <cstdio>
#include <cstdint>
#include <memory>
#include <vector>

#include <runtime/hud/feed_format.h>
#include <net/netsim/client_replica_pipeline.h>
#include <net/npwire/ingame_message_id.h>

using namespace opennova;
using namespace opennova::netsim;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

std::vector<uint8_t> chat_body(int8_t channel, uint8_t sender, const char *text) {
	std::vector<uint8_t> b;
	b.push_back(static_cast<uint8_t>(channel));
	b.push_back(sender);
	for (const char *p = text; *p; ++p) b.push_back(static_cast<uint8_t>(*p));
	b.push_back(0);
	return b;
}

} // namespace

int main() {
	// Heap-allocated: ClientState is far too large for the 1 MB Windows test
	// stack (the same trap #515 fixed in ai_test).
	auto view_owned = std::make_unique<ClientReplicaPipeline>();
	ClientReplicaPipeline &view = *view_owned;

	// Two lines fold in order with the witnessed byte roles: body[0] is the
	// channel, body[1] the sender slot (distinct values, so a re-swap fails).
	view.apply(s2c::CHAT_BROADCAST, chat_body(2, 3, "P: hi"));
	view.apply(s2c::CHAT_BROADCAST, chat_body(0, 7, "Server: welcome"));
	const std::vector<ClientChatLine> lines = view.drain_chat_lines();
	CHECK(lines.size() == 2);
	CHECK(lines.size() == 2 && lines[0].channel == 2 && lines[0].sender_slot == 3 &&
			lines[0].text == "P: hi");
	CHECK(lines.size() == 2 && lines[1].channel == 0 && lines[1].sender_slot == 7);
	// Draining empties the lane — no replay on the next frame.
	CHECK(view.drain_chat_lines().empty());
	// A body too short to carry the header is malformed, not folded.
	const std::size_t malformed_before = view.malformed_bodies();
	view.apply(s2c::CHAT_BROADCAST, std::vector<uint8_t>{2});
	CHECK(view.malformed_bodies() == malformed_before + 1);
	CHECK(view.drain_chat_lines().empty());

	// The channel table: team chat (2) lands in the CHAT ring in green; the
	// all-channel (0) and the default land in the SYSTEM ring in white;
	// 8 enqueues; 14 is the unported third channel.
	CHECK(hud::chat_channel_sink(2) == hud::ChatSink::Chat);
	CHECK(hud::chat_channel_color(2) == hud::kHudColorGreen);
	CHECK(hud::chat_channel_sink(0) == hud::ChatSink::System);
	CHECK(hud::chat_channel_color(0) == hud::kHudColorWhite);
	CHECK(hud::chat_channel_sink(-1) == hud::ChatSink::System);
	CHECK(hud::chat_channel_sink(15) == hud::ChatSink::System);
	CHECK(hud::chat_channel_sink(8) == hud::ChatSink::Queue);
	CHECK(hud::chat_channel_sink(14) == hud::ChatSink::Channel3);
	CHECK(hud::chat_channel_color(1) == hud::kHudColorLightBlue);
	CHECK(hud::chat_channel_color(4) == hud::kHudColorLightBlue);
	CHECK(hud::chat_channel_color(5) == hud::kHudColorLightBlue);
	CHECK(hud::chat_channel_color(3) == hud::kHudColorYellow);
	CHECK(hud::chat_channel_color(7) == hud::kHudColorOrange);
	CHECK(hud::chat_channel_color(9) == hud::kHudColorSalmon);
	CHECK(hud::chat_channel_color(11) == hud::kHudColorCyan);
	CHECK(hud::chat_channel_color(12) == hud::kHudColorMagenta);
	CHECK(hud::chat_channel_color(13) == hud::kHudColorWhite);
	// The witnessed immediates themselves.
	CHECK(hud::kHudColorLightBlue == 0xFF80A0FFu && hud::kHudColorSalmon == 0xFFFF5050u &&
			hud::kHudColorCyan == 0xFF00EAE7u && hud::kHudColorOrange == 0xFFFF8020u &&
			hud::kHudColorMagenta == 0xFFFF40FFu && hud::kHudColorYellow == 0xFFF0F000u);

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("client_replica_chat_test OK\n");
	return 0;
}
