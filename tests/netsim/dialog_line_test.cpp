// D-NET-285 — the co-op dialog line (S2C 0x28), engine side. The authority's
// dialog playback sends [cstr name][i16 line] to every in-match slot but the
// listen host when it loads a line; a non-authority plays that line with its
// player class as the locale, the authority's own client never does.
// [orig: Dialog_UpdatePlayback @0x44E470 -> Server_SendEntityStateToAll
//  @0x50A0D0; NapiNPClientMsg_0x028 @0x425B40 -> Dialog_PlayByNameAndSlot
//  @0x44E3F0]
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_role.h> // authority_dialog_line_loaded
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
#include "../common/title_fixture.h"

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
	test_titles::jo_folds(view);
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

// The effect pass hands the presentation the line as a typed order with the
// local player's class.
void test_effect_pass() {
	auto world = std::make_unique<w::World>();
	test_titles::jo_world(*world);
	world->registry.configure_pool(0, 4);
	w::Entity local;
	local.kind = w::EntityKind::Organic;
	local.player_class = 3;
	world->cached.local_player = world->registry.spawn(0, local);
	inmatch::ClientRuntime runtime("DialogLine");
	test_titles::jo_folds(runtime);
	runtime.set_dialogue_volume(192); // the cfg defaults' DialogueLevel
	runtime.view().apply(s2c::DIALOG_LINE, line_body("dlg007", 1));
	runtime.apply_received_effects(*world);
	CHECK(world->out.dialog_lines.size() == 1);
	if (world->out.dialog_lines.size() == 1) {
		const w::DialogLineOrder &order = world->out.dialog_lines.front();
		CHECK(order.dialog_name == "dlg007" && order.line == 1 && order.player_class == 3);
	}
	world->out.discard_presentation();
	CHECK(world->out.dialog_lines.empty());
}

// D-NET-286: the line's chat legs. A line no bank carries posts "EX Cannot
// load audio: <def name>" on the SYSTEM ring in 0xFFFF2222; a resolved
// subtitle goes to the CHAT ring in white; an unknown dialog posts nothing.
// [orig: Dialog_LoadAudioClipLocalized @0x44DEF0 — @0x44DFD1..0x44DFF8,
//  @0x44E144..0x44E150; Chat_AddSystemMessageIfValid @0x5275B0]
void test_chat_legs() {
	auto world = std::make_unique<w::World>();
	test_titles::jo_world(*world);
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
	test_titles::jo_folds(runtime);
	runtime.set_dialogue_volume(192);
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
	// D-NET-285, the master-volume gate: with the dialogue volume at 0 the missing wave's line
	// still posts (it precedes the gate), but nothing plays and no subtitle posts.
	// [orig: Dialog_LoadAudioClipLocalized @0x44DEF0 - @0x44DFD1..0x44DFF8 ahead of the gate's
	//  call @0x44E0D7 and test @0x44E0E2; Dialog_ApplyDialogueVolumeAndPlayFactor @0x527560 -
	//  0 at g_DialogueVolume 0 @0x527567]
	world->out.discard_presentation();
	runtime.set_dialogue_volume(0);
	posts = play("dlg012", 1);
	CHECK(posts.size() == 1);
	if (posts.size() == 1)
		CHECK(posts[0].sink == hud::ChatSink::System &&
				posts[0].text == "EX Cannot load audio: MISSING");
	CHECK(play("dlg012", 0).empty());
	CHECK(world->out.dialog_lines.empty());
	// Any nonzero volume passes it.
	runtime.set_dialogue_volume(1);
	posts = play("dlg012", 0);
	CHECK(posts.size() == 1 && world->out.dialog_lines.size() == 1);
	world->tables.dialog_bank = nullptr;
	CHECK(play("dlg012", 1).empty());
}

// The host fans the line to the in-match remotes, never to the listen host.
void test_host_fan() {
	ns::LoopbackChannel remote_wire, local_wire, loading_wire;
	inmatch::NapiNPServerCtx ctx;
	test_titles::jo_config(ctx.config);
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
	// [cstr "dlg012"][i16 4] [orig: NetPacket_WriteCStringAndShort @0x5038A0]
	const std::vector<uint8_t> want = {'d', 'l', 'g', '0', '1', '2', 0x00, 0x04, 0x00};
	if (remote.size() == 1)
		CHECK(remote[0].tag == s2c::DIALOG_LINE && remote[0].body == want);
	CHECK(take(local_wire).empty());
	CHECK(take(loading_wire).empty());
	ctx.is_authority = 0;
	inmatch::Server_BroadcastDialogLine(ctx, "dlg012", 5);
	CHECK(take(remote_wire).empty());
}

// The authority's load: its own client posts the line's chat legs (the missing
// wave first, then the subtitle), then the line goes to the in-match remotes,
// never to the listen host's own connection (mask 0x90).
// [orig: Dialog_UpdatePlayback @0x44E599 / @0x44E5A5; Dialog_LoadAudioClip
//  @0x44DD74, @0x44DEC9]
void test_authority_load() {
	auto world = std::make_unique<w::World>();
	test_titles::jo_world(*world);
	dbf::File bank;
	dbf::Group group;
	group.group_name = "dlg012";
	dbf::Line line0;
	line0.def_id_name = "MISSING";
	line0.sequence = "_00001";
	group.lines.push_back(line0);
	bank.groups.push_back(group);
	rtxt::File text;
	text.sections.push_back(rtxt::Section{"Info", 2});
	rtxt::Entry title, second;
	title.key = "title";
	title.text = "Title";
	second.key = "second";
	second.text = "Hold the line.";
	text.entries = {title, second};
	world->tables.dialog_bank = &bank;
	world->tables.mission_text = &text;

	ns::LoopbackChannel remote_wire, local_wire;
	inmatch::NapiNPServerCtx ctx;
	test_titles::jo_config(ctx.config);
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostClient);
	ctx.is_in_session = 1;
	auto &list = ctx.np_protocol.connection_list;
	list.push_back(conn_fixture::make_conn(1, 1, &remote_wire, ns::TransportMode::Client,
			w::EntityHandle{}, true));
	list.push_back(conn_fixture::make_conn(2, 2, &local_wire, ns::TransportMode::Loopback,
			w::EntityHandle{}, true));
	inmatch::ClientRuntime own("ListenHost");
	test_titles::jo_folds(own);
	own.set_dialogue_volume(192);
	audio::DialogLineRef ref;
	ref.wave = "MISSING";
	ref.dialog_name = "dlg012";
	ref.line = 0;
	inmatch::authority_dialog_line_loaded(ctx, &own, *world, ref);
	std::vector<hud::FeedPost> posts = own.drain_ring_posts();
	CHECK(posts.size() == 2);
	if (posts.size() == 2) {
		CHECK(posts[0].sink == hud::ChatSink::System && posts[0].argb == 0xFFFF2222u &&
				posts[0].text == "EX Cannot load audio: MISSING");
		CHECK(posts[1].sink == hud::ChatSink::Chat && posts[1].argb == 0xFFFFFFFFu &&
				posts[1].text == "Hold the line.");
	}
	ns::Datagram d;
	CHECK(remote_wire.client_recv(d) && d.tag == s2c::DIALOG_LINE &&
			d.body == (std::vector<uint8_t>{'d', 'l', 'g', '0', '1', '2', 0x00, 0x00, 0x00}));
	CHECK(!local_wire.client_recv(d));
	// D-NET-285: the master-volume gate holds back the authority's own subtitle (the missing
	// wave's line precedes it); the send does not depend on it [orig: Dialog_LoadAudioClip
	// @0x44DD74 ahead of the gate @0x44DE50..0x44DE5B; Dialog_UpdatePlayback @0x44E599 then
	// the send @0x44E5A5].
	own.set_dialogue_volume(0);
	inmatch::authority_dialog_line_loaded(ctx, &own, *world, ref);
	posts = own.drain_ring_posts();
	CHECK(posts.size() == 1 && posts[0].sink == hud::ChatSink::System);
	CHECK(remote_wire.client_recv(d) && d.tag == s2c::DIALOG_LINE);
	// A Serve Only host has no client of its own: the line goes out all the same.
	inmatch::authority_dialog_line_loaded(ctx, nullptr, *world, ref);
	CHECK(remote_wire.client_recv(d) && d.tag == s2c::DIALOG_LINE);
}

} // namespace

int main() {
	test_fold();
	test_effect_pass();
	test_chat_legs();
	test_host_fan();
	test_authority_load();
	std::printf("dialog_line: %d failures\n", failures);
	return failures ? 1 : 0;
}
