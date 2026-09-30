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
#include <runtime/replication/client_replica_pipeline.h>
#include <net/npwire/ingame_message_id.h>

using namespace opennova;
using namespace opennova::replication;

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

	// THE S2C 0x32 JOIN/LEAVE LANE: the handled subtypes fold verbatim (the
	// signed team byte of the join/leave pair), in wire order; others drop
	// [orig: NapiNPClientMsg_0x032 @0x428060].
	view.apply(s2c::FORMATTED_GAME_TEXT, {1, 'R', 'o', 'c', 'k', 0, 2});
	view.apply(s2c::FORMATTED_GAME_TEXT, {2, 'R', 'o', 'c', 'k', 0, 0xFF});
	view.apply(s2c::FORMATTED_GAME_TEXT, {5, 'S', 'p', 'e', 'c', 0});
	view.apply(s2c::FORMATTED_GAME_TEXT, {9, 'x', 0});
	const std::vector<ClientGameText> texts = view.drain_game_texts();
	CHECK(texts.size() == 3);
	CHECK(texts.size() == 3 && texts[0].subtype == 1 && texts[0].text == "Rock" &&
			texts[0].team == 2);
	CHECK(texts.size() == 3 && texts[1].subtype == 2 && texts[1].team == -1);
	CHECK(texts.size() == 3 && texts[2].subtype == 5 && texts[2].text == "Spec");
	CHECK(view.drain_game_texts().empty());
	// The listen host's own loopback posts them too (no authority gate).
	auto host_owned = std::make_unique<ClientReplicaPipeline>();
	host_owned->set_authority_recipient(true);
	host_owned->apply(s2c::FORMATTED_GAME_TEXT, {2, 'R', 0, 1});
	CHECK(host_owned->drain_game_texts().size() == 1);

	// The talk keys' reset hold: S2C 0x25 raises it on a client (never on the
	// authority's loopback), the next S2C 0x0F lowers it
	// [orig: NapiNPClientMsg_GameReset @0x42284e; NapiNPClientMsg_0x00F @0x42e396].
	CHECK(!view.state().round_reset_hold);
	view.apply(s2c::GAME_RESET, {});
	CHECK(view.state().round_reset_hold);
	view.apply(s2c::WORLD_STATE_LOAD, {});
	CHECK(!view.state().round_reset_hold);
	host_owned->apply(s2c::GAME_RESET, {});
	CHECK(!host_owned->state().round_reset_hold);

	// THE WIRE ORDER ACROSS THE THREE RING LANES: each record takes the
	// dispatch stamp of the message that carried it, so a 0x32 that arrives
	// between two 0x1E events sorts between their lines, and a 0x14 after them
	// [orig: each handler posts as it runs — NetPacket_HandleGameEvent
	// @0x426270, NapiNPClientMsg_0x032 @0x428181..0x428195,
	// Chat_DispatchToChannel @0x42b910].
	{
		auto order_owned = std::make_unique<ClientReplicaPipeline>();
		ClientReplicaPipeline &o = *order_owned;
		const std::vector<uint8_t> kill = {1, 2, 3, 0xFF, 0, 0, 0, 0};
		o.apply(s2c::GAME_EVENT, kill);
		o.apply(s2c::FORMATTED_GAME_TEXT, {2, 'R', 0, 1});
		o.apply(s2c::GAME_EVENT, kill);
		o.apply(s2c::CHAT_BROADCAST, chat_body(0, 7, "Server: hi"));
		const std::vector<ClientGameEvent> events = o.drain_game_events();
		const std::vector<ClientGameText> joins = o.drain_game_texts();
		const std::vector<ClientChatLine> chats = o.drain_chat_lines();
		CHECK(events.size() == 2 && joins.size() == 1 && chats.size() == 1);
		if (events.size() == 2 && joins.size() == 1 && chats.size() == 1) {
			CHECK(events[0].feed_order < joins[0].feed_order);
			CHECK(joins[0].feed_order < events[1].feed_order);
			CHECK(events[1].feed_order < chats[0].feed_order);
			// The merge keeps that order, and a message's own lines keep
			// theirs (stable) whatever lane order the embedder drains in.
			std::vector<hud::FeedPost> posts = {
				{chats[0].feed_order, hud::ChatSink::System, 0, "chat", false},
				{joins[0].feed_order, hud::ChatSink::System, 0, "join", false},
				{events[0].feed_order, hud::ChatSink::System, 0, "kill-a", false},
				{events[0].feed_order, hud::ChatSink::System, 0, "kill-a2", false},
				{events[1].feed_order, hud::ChatSink::System, 0, "kill-b", true},
			};
			hud::order_feed_posts(posts);
			CHECK(posts[0].text == "kill-a" && posts[1].text == "kill-a2" &&
					posts[2].text == "join" && posts[3].text == "kill-b" &&
					posts[4].text == "chat");
		}
	}

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("client_replica_chat_test OK\n");
	return 0;
}
