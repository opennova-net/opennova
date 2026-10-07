// D-NET-285 — the co-op dialog line (S2C 0x28), engine side. The authority's
// dialog playback sends [cstr name][i16 line] to every in-match slot but the
// listen host when it loads a line; a non-authority plays that line with its
// player class as the locale, the authority's own client never does.
// [orig: Dialog_UpdatePlayback @0x44E470 -> Server_SendEntityStateToAll
//  @0x50A0D0; NapiNPClientMsg_0x028 @0x425B40 -> Dialog_PlayByNameAndSlot
//  @0x44E3F0]
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/server_chat.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/audio/dialog_queue.h>
#include <runtime/hud/feed_format.h>
#include <runtime/world/world.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <cstdio>
#include <memory>
#include <variant>
#include <vector>

#include "../npruntime/conn_fixture.h"

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

std::vector<uint8_t> line_body(const char *name, int16_t line) {
	DialogLine record;
	record.dialog_name = name;
	record.line = line;
	return encode_dialog_line(record);
}

// A non-authority surfaces the line; the authority's own client does not.
void test_fold() {
	ns::ClientReplicaPipeline view;
	view.apply(s2c::DIALOG_LINE, line_body("dlg012", 2));
	std::vector<ns::ClientEffectCommand> out = view.drain_effect_commands();
	CHECK(out.size() == 1);
	if (out.size() == 1) {
		const auto *line = std::get_if<ns::DialogLineCommand>(&out[0]);
		CHECK(line != nullptr && line->dialog_name == "dlg012" && line->line == 2);
	}
	view.set_authority_recipient(true);
	view.apply(s2c::DIALOG_LINE, line_body("dlg012", 3));
	CHECK(view.drain_effect_commands().empty());
}

// The effect pass hands the presentation the line with the local player's
// class.
void test_effect_pass() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 4);
	w::Entity local;
	local.kind = w::EntityKind::Organic;
	local.player_class = 3;
	world->cached.local_player = world->registry.spawn(0, local);
	inmatch::ClientRuntime runtime("DialogLine");
	runtime.view().apply(s2c::DIALOG_LINE, line_body("dlg007", 1));
	runtime.apply_received_effects(*world);
	CHECK(world->out.effects.count("dialog_line") == 1);
	for (const w::Effect &e : world->out.effects.entries()) {
		if (e.kind != "dialog_line") continue;
		CHECK(e.str == "dlg007" && e.a == 1 && e.b == 3);
	}
}

// D-NET-286: the line's chat legs. A line no bank carries posts "EX Cannot
// load audio: <def name>" on the SYSTEM ring in 0xFFFF2222; a resolved
// subtitle goes to the CHAT ring in white; an unknown dialog posts nothing.
// [orig: Dialog_LoadAudioClipLocalized @0x44DEF0 — @0x44DFD1..0x44DFF8,
//  @0x44E144..0x44E150; Chat_AddSystemMessageIfValid @0x5275B0]
void test_chat_legs() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 4);
	w::Entity local;
	local.kind = w::EntityKind::Organic;
	local.player_class = 2;
	world->cached.local_player = world->registry.spawn(0, local);
	dbf::File bank;
	dbf::Group group;
	group.group_name = "dlg012";
	for (const char *def : {"SynR100", "MISSING"}) {
		dbf::Line line;
		line.def_id_name = def;
		line.sequence = std::string("_0000") + (def[0] == 'S' ? "0" : "1");
		group.lines.push_back(line);
	}
	bank.groups.push_back(group);
	// The dialog bank's sounds: a wave (a single) of the line's name, which the
	// line finds by name [orig: SoundBank_FindEntryByName @0x75BBA0].
	lwf::File sounds;
	lwf::Single wave;
	wave.name = "SynR100";
	wave.path = "SynR100.wav";
	sounds.singles.push_back(wave);
	rtxt::File text;
	text.sections.push_back(rtxt::Section{"Info", 2});
	text.sections.push_back(rtxt::Section{"Mission Dialog", 1});
	rtxt::Entry title, second, line0;
	title.key = "title";
	title.text = "Title";
	second.key = "second";
	second.text = "Hold the line.";
	line0.key = "SynR100";
	line0.text = "Move out!";
	line0.section_index = 1;
	text.entries = {title, second, line0};
	world->tables.dialog_bank = &bank;
	world->tables.dialog_sounds = &sounds;
	world->tables.mission_text = &text;

	inmatch::ClientRuntime runtime("DialogText");
	const auto play = [&](const char *name, int16_t line) {
		runtime.view().apply(s2c::DIALOG_LINE, line_body(name, line));
		runtime.apply_received_effects(*world);
		return runtime.drain_ring_posts();
	};
	std::vector<hud::FeedPost> posts = play("dlg012", 0);
	CHECK(posts.size() == 1);
	if (posts.size() == 1)
		CHECK(posts[0].sink == hud::ChatSink::Chat && posts[0].argb == 0xFFFFFFFFu &&
				posts[0].text == "Move out!");
	posts = play("dlg012", 1);
	CHECK(posts.size() == 2);
	if (posts.size() == 2) {
		CHECK(posts[0].sink == hud::ChatSink::System && posts[0].argb == 0xFFFF2222u &&
				posts[0].text == "EX Cannot load audio: MISSING");
		CHECK(posts[1].sink == hud::ChatSink::Chat && posts[1].text == "Hold the line.");
		CHECK(posts[0].order < posts[1].order);
	}
	CHECK(play("dlg999", 0).empty());
	world->tables.dialog_bank = nullptr;
	CHECK(play("dlg012", 1).empty());
}

// The host fans the line to the in-match remotes, never to the listen host.
void test_host_fan() {
	ns::LoopbackChannel remote_wire, local_wire, loading_wire;
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	ctx.is_in_session = 1;
	auto &list = ctx.np_protocol.connection_list;
	list.push_back(conn_fixture::make_conn(1, 1, &remote_wire, ns::TransportMode::Client,
			w::EntityHandle{}, true));
	list.push_back(conn_fixture::make_conn(2, 2, &local_wire, ns::TransportMode::Loopback,
			w::EntityHandle{}, true));
	list.push_back(conn_fixture::make_conn(3, 1, &loading_wire, ns::TransportMode::Client,
			w::EntityHandle{}, false));
	inmatch::Server_BroadcastDialogLine(ctx, "dlg012", 4);
	const auto take = [](ns::LoopbackChannel &wire) {
		std::vector<ns::Datagram> out;
		ns::Datagram d;
		while (wire.client_recv(d)) out.push_back(d);
		return out;
	};
	const std::vector<ns::Datagram> remote = take(remote_wire);
	CHECK(remote.size() == 1);
	if (remote.size() == 1)
		CHECK(remote[0].tag == s2c::DIALOG_LINE && remote[0].body == line_body("dlg012", 4));
	CHECK(take(local_wire).empty());
	CHECK(take(loading_wire).empty());
	ctx.is_authority = 0;
	inmatch::Server_BroadcastDialogLine(ctx, "dlg012", 5);
	CHECK(take(remote_wire).empty());
}

} // namespace

int main() {
	test_fold();
	test_effect_pass();
	test_chat_legs();
	test_host_fan();
	std::printf("dialog_line: %d failures\n", failures);
	return failures ? 1 : 0;
}
