// The sound lane (ADR 0046; DI-02, DI-16): a sound bank (.lwf) and SndProf.def as record documents over
// the engine's own readers and from-scratch writers, the sound names they define as the graph's symbols
// (a set looked up in the game's bank order, a profile by name), the preview's picks as the game picks,
// and the session's play_sound over them. Minted fixtures alone (fixtures/lwf/menu.lwf, a short PCM wave,
// banks minted here through lwf::encode_lwf); the retail leg (OPENNOVA_JO_ASSETS) reads every shipped
// bank and the shipped SndProf.def through the documents and writes them back from scratch, the same.
#include <editor/assets/asset_import.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/document_types.h>
#include <editor/documents/sound_bank_document.h>
#include <editor/documents/sound_profile_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/project_validation.h>
#include <editor/import/import_context.h>
#include <editor/import/import_plan.h>
#include <editor/import/importer.h>
#include <editor/import/wave_source.h>
#include <editor/preview/sound_preview.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/sound_play.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
#include <formats/lwf/lwf.h>
#include <runtime/audio/sound_profile.h>
#include <runtime/audio/sound_selector.h>

#include <base/io/le.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

constexpr NodeKind kWave = node_kind(SoundBankKind::Wave);
constexpr NodeKind kSet = node_kind(SoundBankKind::Set);
constexpr NodeKind kLayer = node_kind(SoundBankKind::Layer);
constexpr NodeKind kMember = node_kind(SoundBankKind::Member);
constexpr NodeKind kProfile = node_kind(SoundProfileKind::Profile);
constexpr NodeKind kSlot = node_kind(SoundProfileKind::Slot);

std::string repo() { return test_paths_repo_root(__FILE__); }

std::vector<uint8_t> text_bytes(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

Edit set_edit(NodeAddress address, const char *field, Value value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

const SoundBankRow *row_named(const SoundBankDocument &bank, SoundBankKind kind, const std::string &name) {
	for (const SoundBankRow *row : bank.rows_of(kind))
		if (row->name() == name) return row;
	return nullptr;
}

bool has_code(const std::vector<Diagnostic> &findings, const char *code) {
	return std::any_of(findings.begin(), findings.end(), [&](const Diagnostic &d) { return d.code() == code; });
}

// A bank of `waves` (name = file stem) and one set of a layer naming each, flags `flags`.
std::vector<uint8_t> bank_bytes(const std::vector<std::string> &waves, const std::string &set_name, uint32_t flags,
                                uint32_t set_pitch = lwf::kAuthoredSetPitchBase, uint32_t member_pitch = lwf::kPitchUnityQ16) {
	lwf::File bank;
	for (const std::string &wave : waves) {
		lwf::Single single;
		single.name = strutil::to_upper(wave);
		single.path = wave + ".wav";
		single.value_hi = 0xD200;
		bank.singles.push_back(single);
	}
	lwf::Multi set;
	set.name = set_name;
	set.pitch_base = set_pitch;
	set.target_id = 10000;
	set.playlist_indices.push_back(0);
	lwf::Playlist layer;
	layer.falloff_radius = 100;
	layer.flags = flags;
	for (size_t i = 0; i < waves.size(); ++i) {
		layer.sndparm_indices.push_back(uint32_t(i));
		lwf::Sndparm member;
		member.single_index = uint32_t(i);
		member.pitch_scaled = member_pitch;
		member.volume = 200;
		member.clamp_volume = 255;
		bank.sndparms.push_back(member);
	}
	bank.playlists.push_back(layer);
	bank.multis.push_back(set);
	std::vector<uint8_t> out;
	std::string error;
	lwf::encode_lwf(bank, out, error);
	return out;
}

// --- the bank document ------------------------------------------------------------------------------

// The minted bank reads into its waves and sets, writes back the bytes it was minted with (our writer
// made both), and every member names the wave it pointed at.
int test_bank_reads_and_writes_back() {
	const std::vector<uint8_t> bytes = test_io::read_file(repo() + "/fixtures/lwf/menu.lwf");
	TEST_EXPECT(!bytes.empty());
	SoundBankDocument bank;
	Diagnostic error;
	TEST_EXPECT(bank.load_bytes(bytes, "sounds/menu.lwf", AssetKind::SoundBank, "jo", error));
	TEST_EXPECT(bank.rows_of(SoundBankKind::Wave).size() == 3 && bank.rows_of(SoundBankKind::Set).size() == 3);
	const SoundBankRow *tone = row_named(bank, SoundBankKind::Wave, "TONE");
	TEST_EXPECT(tone && tone->wave.file == "tone.wav");
	const SoundBankRow *truck = row_named(bank, SoundBankKind::Set, "V_TRUCK_ILP");
	TEST_EXPECT(truck && truck->set.layers.size() == 1 && truck->set.layers[0].falloff == 2000 &&
	            truck->set.layers[0].members.size() == 1 && truck->set.layers[0].members[0].wave == "TONE");
	const SerializeResult written = bank.serialize();
	TEST_EXPECT(written.ok() && std::vector<uint8_t>(written.text.begin(), written.text.end()) == bytes);
	TEST_EXPECT(bank.rewrite_need() == DocumentBase::RewriteNeed::None);
	TEST_EXPECT(validate_sound_bank_file(bank).empty());
	// The layer's words, the member by the wave it plays.
	const NodeAddress layer{truck->id, kLayer, truck->ids.lists[0][0].id};
	TEST_EXPECT(bank.record_title(layer) == "Layer 1: at random, 1 member");
	TEST_EXPECT(bank.record_title({tone->id, kWave, 0}) == "TONE (tone.wav)");
	return 0;
}

// Every witnessed field edits through the table: a set's pitch as a ratio, its range, a layer's radii and
// flags, a member's pitch, volume and ceiling; a wave and a set added in one batch with its layer and a
// member naming the new wave, written and read back by the engine's reader; the names' widths.
int test_bank_edits() {
	SoundBankDocument bank;
	Diagnostic error;
	TEST_EXPECT(bank.load_bytes(test_io::read_file(repo() + "/fixtures/lwf/menu.lwf"), "sounds/menu.lwf",
	                            AssetKind::SoundBank, "jo", error));
	const SoundBankRow *over = row_named(bank, SoundBankKind::Set, "MOUSE_OVER");
	TEST_EXPECT(over != nullptr);
	if (!over) return 1;
	const NodeAddress set{over->id, kSet, 0};
	const NodeAddress layer{over->id, kLayer, over->ids.lists[0][0].id};
	const NodeAddress member{over->id, kMember, over->ids.lists[0][0].lists[0][0].id};
	TEST_EXPECT(bank.apply({set_edit(set, "pitch", 1.5), set_edit(set, "pitch_jitter", int64_t(0)),
	                        set_edit(set, "range", int64_t(250)), set_edit(layer, "falloff", int64_t(80)),
	                        set_edit(layer, "min_distance", int64_t(5)),
	                        set_edit(layer, "flags", int64_t(lwf::kFlagInternal | lwf::kFlagExternal | lwf::kFlagSequential)),
	                        set_edit(member, "pitch", 0.75), set_edit(member, "volume", int64_t(128)),
	                        set_edit(member, "ceiling", int64_t(200))},
	                       error));
	Value picks;
	TEST_EXPECT(bank.get(layer, "picks", picks) && std::get<std::string>(picks) == "in order");
	// A name past its field, and a volume past a byte, refused.
	TEST_EXPECT(!bank.apply(set_edit(set, "name", std::string(24, 'A')), error));
	TEST_EXPECT(bank.apply(set_edit(set, "name", std::string(23, 'A')), error));
	TEST_EXPECT(!bank.apply(set_edit(member, "volume", int64_t(256)), error));
	// A new wave, a new set, its layer's member playing the new wave: one batch.
	Edit add_wave;
	add_wave.operation = EditOperation::Add;
	add_wave.address = {0, kWave, 0};
	const NodeAddress made_wave{batch_made(0), kWave, 0};
	TEST_EXPECT(bank.apply({add_wave, set_edit(made_wave, "name", std::string("FS_DIRT1")),
	                        set_edit(made_wave, "file", std::string("fs_dirt1.wav"))},
	                       error));
	Edit add_set;
	add_set.operation = EditOperation::Add;
	add_set.address = {0, kSet, 0};
	TEST_EXPECT(bank.apply(add_set, error));
	const SoundBankRow *made = row_named(bank, SoundBankKind::Set, "NEW_SET");
	TEST_EXPECT(made && made->set.layers.empty());
	if (!made) return 1;
	// Its layer and the layer's member in one batch, the member's owner the layer the batch made.
	Edit add_layer;
	add_layer.operation = EditOperation::Add;
	add_layer.address = {made->id, kLayer, 0};
	Edit add_member;
	add_member.operation = EditOperation::Add;
	add_member.address = {made->id, kMember, 0};
	add_member.parent = batch_made(0);
	TEST_EXPECT(bank.apply({add_layer, add_member}, error));
	made = row_named(bank, SoundBankKind::Set, "NEW_SET");
	// The new member plays the bank's first wave until one is named.
	TEST_EXPECT(made && made->set.layers[0].members.size() == 1 && made->set.layers[0].members[0].wave == "MSOVR_2");
	const NodeAddress new_member{made->id, kMember, made->ids.lists[0][0].lists[0][0].id};
	TEST_EXPECT(bank.apply(set_edit(new_member, "wave", std::string("fs_dirt1")), error)); // names compare without case
	// The waves before the sets, as the file holds them.
	TEST_EXPECT(bank.rows()[3]->kind == kWave && bank.rows()[4]->kind == kSet);
	const SerializeResult written = bank.serialize();
	TEST_EXPECT(written.ok());
	lwf::File file;
	std::string message;
	TEST_EXPECT(lwf::parse_lwf_buffer(reinterpret_cast<const uint8_t *>(written.text.data()), written.text.size(), file, message));
	TEST_EXPECT(file.singles.size() == 4 && file.singles[3].name == "FS_DIRT1" && file.singles[3].path == "fs_dirt1.wav");
	TEST_EXPECT(file.multis.size() == 4 && file.multis[0].name == std::string(23, 'A') &&
	            file.multis[0].pitch_base == 0x18000 && file.multis[0].target_id == 250);
	const lwf::Playlist &edited = file.playlists[file.multis[0].playlist_indices[0]];
	TEST_EXPECT(edited.falloff_radius == 80 && edited.min_distance == 5 && (edited.flags & lwf::kFlagSequential));
	const lwf::Sndparm &edited_member = file.sndparms[edited.sndparm_indices[0]];
	TEST_EXPECT(edited_member.pitch_scaled == 0xC000 && edited_member.volume == 128 && edited_member.clamp_volume == 200);
	const lwf::Multi &added = file.multis[3];
	TEST_EXPECT(added.name == "NEW_SET" && file.sndparms[file.playlists[added.playlist_indices[0]].sndparm_indices[0]].single_index == 3);
	// Undo takes the batch back; the bytes as they were.
	return 0;
}

// The bank's findings: a wave named twice, a member naming no wave (the save refuses it), a layer heard in
// neither view, a set with no member, a wave whose file the archives cannot hold.
int test_bank_findings() {
	SoundBankDocument bank;
	Diagnostic error;
	TEST_EXPECT(bank.load_bytes(bank_bytes({"a", "b"}, "PAIR", lwf::kFlagRandom), "game.lwf", AssetKind::SoundBank, "jo", error));
	std::vector<Diagnostic> findings = validate_sound_bank_file(bank);
	TEST_EXPECT(has_code(findings, "sound_bank.layer_unheard"));
	const SoundBankRow *b = row_named(bank, SoundBankKind::Wave, "B");
	const SoundBankRow *pair = row_named(bank, SoundBankKind::Set, "PAIR");
	TEST_EXPECT(b && pair);
	if (!b || !pair) return 1;
	TEST_EXPECT(bank.apply(set_edit({b->id, kWave, 0}, "name", std::string("a")), error)); // a second "A", without case
	TEST_EXPECT(bank.apply(set_edit({b->id, kWave, 0}, "file", std::string("a_name_far_too_long.wav")), error));
	pair = row_named(bank, SoundBankKind::Set, "PAIR");
	const NodeAddress member{pair->id, kMember, pair->ids.lists[0][0].lists[0][1].id};
	TEST_EXPECT(bank.apply(set_edit(member, "wave", std::string("GONE")), error));
	findings = validate_sound_bank_file(bank);
	TEST_EXPECT(has_code(findings, "sound_bank.wave_name_repeated") && has_code(findings, "sound_bank.wave_file_name") &&
	            has_code(findings, "sound_bank.unserializable"));
	TEST_EXPECT(!bank.serialize().ok() && bank.rewrite_need() == DocumentBase::RewriteNeed::Unserializable);
	Edit add_set;
	add_set.operation = EditOperation::Add;
	add_set.address = {0, kSet, 0};
	TEST_EXPECT(bank.apply(add_set, error));
	TEST_EXPECT(has_code(validate_sound_bank_file(bank), "sound_bank.set_silent"));
	return 0;
}

// --- the sound profiles -------------------------------------------------------------------------------

const char *kProfiles =
        "begin \"default\"\r\n\tSSLFootGND FSP_DIRT_L 0 0 0\r\n\tSSRFootGND FSP_DIRT_R 0 0 0\r\nend\r\n"
        "begin \"SP_Truck\"\r\n\tsoundloop_1 V_TRUCK_ILP 0.8 1.2 2\r\n\tmedloopfadeinstart 20\r\nend\r\n";

// SndProf.def reads as the game reads it, each profile's 51 slots in the engine's order; a slot's set and
// numbers and a profile's percent edit through the table and write through audio::write_sound_profiles,
// which the game's walk reads back the same; a column-2 value with no set is a profile the file cannot
// carry (sound_profiles.unserializable), a name twice and no "default" said.
int test_profiles() {
	SoundProfileDocument profiles;
	Diagnostic error;
	TEST_EXPECT(profiles.load_bytes(text_bytes(kProfiles), "SndProf.def", AssetKind::SoundProfileDefs, "jo", error));
	TEST_EXPECT(profiles.rows().size() == 2);
	const auto &truck = static_cast<const SoundProfileRow &>(*profiles.rows()[1]);
	TEST_EXPECT(truck.profile.slots.size() == 51 && truck.profile.slots[0].set == "V_TRUCK_ILP" &&
	            truck.profile.slots[0].param2_q16 == 52428 && truck.profile.slots[0].param4 == 2 &&
	            truck.profile.loop_params[0] == 20 * 655);
	const NodeAddress loop{truck.id, kSlot, truck.ids.lists[0][0].id};
	TEST_EXPECT(profiles.record_title(loop) == "Soundloop_1 (loop 1): V_TRUCK_ILP");
	const auto &defaults = static_cast<const SoundProfileRow &>(*profiles.rows()[0]);
	const NodeAddress water{defaults.id, kSlot, defaults.ids.lists[0][audio::kSlotFootWater].id};
	TEST_EXPECT(profiles.apply({set_edit(water, "set", std::string("FS_WATER")), set_edit(water, "param3", 1.25),
	                            set_edit({truck.id, kProfile, 0}, "medloopfadeinend", int64_t(80))},
	                           error));
	// A set name past its 24 bytes refused; the slot list is fixed.
	TEST_EXPECT(!profiles.apply(set_edit(water, "set", std::string(24, 'X')), error));
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = water;
	TEST_EXPECT(!profiles.apply(remove, error));
	const SerializeResult written = profiles.serialize();
	TEST_EXPECT(written.ok());
	audio::SoundProfileTable table;
	TEST_EXPECT(table.parse(written.text.data(), written.text.size()) == 2);
	TEST_EXPECT(table.entries()[0].set_names[audio::kSlotFootWater] == "FS_WATER" &&
	            table.entries()[0].param3_q16[audio::kSlotFootWater] == 81920 &&
	            table.entries()[1].loop_params[1] == 80 * 655 && table.entries()[1].set_names[0] == "V_TRUCK_ILP");
	TEST_EXPECT(validate_sound_profiles_file(profiles).empty());
	// A column-2 value with no set: the file cannot carry it.
	const NodeAddress death{defaults.id, kSlot, defaults.ids.lists[0][audio::kSlotDeath].id};
	TEST_EXPECT(profiles.apply(set_edit(death, "param2", 2.0), error));
	TEST_EXPECT(has_code(validate_sound_profiles_file(profiles), "sound_profiles.unserializable") && !profiles.serialize().ok());
	profiles.undo();
	// A name twice; no "default".
	TEST_EXPECT(profiles.apply(set_edit({defaults.id, kProfile, 0}, "name", std::string("SP_Truck")), error));
	const std::vector<Diagnostic> findings = validate_sound_profiles_file(profiles);
	TEST_EXPECT(has_code(findings, "sound_profiles.name_repeated") && has_code(findings, "sound_profiles.no_default"));
	// A new profile is "default" where none has the name.
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {0, kProfile, 0};
	TEST_EXPECT(profiles.apply(add, error) && profiles.rows().back()->name() == "default");
	TEST_EXPECT(sound_profile_slot_of("ssrfootgnd") == audio::kSlotFootRGround && sound_profile_slot_of("nope") == -1);
	return 0;
}

// --- the preview's picks --------------------------------------------------------------------------------

std::vector<PreviewBank> preview_banks(const std::vector<std::pair<std::string, std::vector<uint8_t>>> &files) {
	std::vector<PreviewBank> banks;
	for (const auto &[name, bytes] : files) {
		PreviewBank bank;
		bank.name = name;
		bank.path = "sounds/" + name;
		std::string error;
		if (lwf::parse_lwf_buffer(bytes.data(), bytes.size(), bank.file, error)) banks.push_back(std::move(bank));
	}
	return banks;
}

// A set found as the game finds it: the chain's first bank holding it (gamelocl.lwf before game.lwf), a
// bank off the chain never searched unless named; a sequential layer steps on from play to play; a random
// layer picks through the engine's stream, its pitch the set's and the member's composed; the footstep
// slot by the surface as the game's test picks it; an empty slot plays nothing.
int test_preview_picks() {
	const std::vector<PreviewBank> banks = preview_banks({
	        {"game.lwf", bank_bytes({"g1", "g2", "g3"}, "STEP", lwf::kFlagInternal | lwf::kFlagExternal | lwf::kFlagSequential)},
	        {"gamelocl.lwf", bank_bytes({"l1"}, "STEP", lwf::kFlagInternal | lwf::kFlagExternal)},
	        {"game2.lwf", bank_bytes({"r1", "r2", "r3", "r4"}, "RANDOM", lwf::kFlagInternal | lwf::kFlagExternal, 0x10000, 0x8000)},
	        {"menu.lwf", bank_bytes({"m1"}, "MENU_ONLY", lwf::kFlagInternal | lwf::kFlagExternal)},
	});
	TEST_EXPECT(chain_banks(banks, "").size() == 3 && chain_banks(banks, "").front()->name == "gamelocl.lwf");
	audio::SoundSelector selector;
	PreviewPlay step = plan_set_play(banks, "", "step", "", selector);
	TEST_EXPECT(step.found && step.bank == "gamelocl.lwf" && step.voices.size() == 1 && step.voices[0].file == "l1.wav");
	// game.lwf's own set when named: in order, 1, 2, 3, then 1 again.
	std::vector<std::string> sequence;
	for (int i = 0; i < 4; ++i) sequence.push_back(plan_set_play(banks, "", "STEP", "game.lwf", selector).voices.at(0).file);
	TEST_EXPECT(sequence == std::vector<std::string>({"g1.wav", "g2.wav", "g3.wav", "g1.wav"}));
	// Off the chain: not found by name alone, found in its own bank.
	TEST_EXPECT(!plan_set_play(banks, "", "MENU_ONLY", "", selector).found &&
	            plan_set_play(banks, "", "MENU_ONLY", "menu.lwf", selector).found);
	// A random layer through the engine's own stream: the same picks a fresh selector makes, member pitch
	// 0.5 composed with the set's 1.0.
	audio::SoundSelector ours, engine;
	const PreviewPlay random = plan_set_play(banks, "", "RANDOM", "", ours);
	const int expected = engine.select(audio::SoundSelector::make_key(2, 0, 0), 4, audio::kRandom);
	TEST_EXPECT(random.found && random.voices.size() == 1 &&
	            random.voices[0].file == "r" + std::to_string(expected + 1) + ".wav" && random.voices[0].pitch_q16 == 0x8000 &&
	            random.voices[0].volume == 200);
	// The footstep slots by surface, and a profile's slot through the chain.
	TEST_EXPECT(footstep_slot_on(FootSurface::Ground, 0) == audio::kSlotFootLGround &&
	            footstep_slot_on(FootSurface::Snow, 1) == audio::kSlotFootRSnow &&
	            footstep_slot_on(FootSurface::Object, 0) == audio::kSlotFootLObject &&
	            footstep_slot_on(FootSurface::Water, 1) == audio::kSlotFootWater);
	audio::SoundProfileTable table;
	table.parse("begin \"default\"\r\n\tSSLFootGND STEP 0 0 0\r\nend\r\n", 44);
	const PreviewPlay footstep = plan_footstep_play(table.entries(), "SP_Unknown", FootSurface::Ground, 0, banks, "", selector);
	TEST_EXPECT(footstep.found && footstep.set == "STEP" && footstep.words.find("default's SSLFootGND") == 0);
	const PreviewPlay empty = plan_footstep_play(table.entries(), "default", FootSurface::Snow, 0, banks, "", selector);
	TEST_EXPECT(!empty.found && empty.words == "default's SSLFootSnow is empty: the game plays nothing.");
	// An expansion's own bank comes first.
	const std::vector<PreviewBank> expansion = preview_banks({
	        {"game.lwf", bank_bytes({"g1"}, "X", lwf::kFlagInternal)}, {"myexp.lwf", bank_bytes({"e1"}, "X", lwf::kFlagInternal)}});
	TEST_EXPECT(plan_set_play(expansion, "myexp", "X", "", selector).bank == "myexp.lwf" &&
	            plan_set_play(expansion, "", "X", "", selector).bank == "game.lwf");
	return 0;
}

// --- the project: graph, blanks, session -------------------------------------------------------------

struct SoundProject {
	editor_test::TempProjectDir dir{"opennova_editor_sound_documents"};
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	bool made = false;
	SoundProject() {
		if (!session.handle(request::new_project(dir.file("project"), "Sounds"))) return;
		session.run_operations();
		editor_test::create_missing_files(session);
		const std::string root = session.view().project.root;
		// game.lwf plays fs_dirt1.wav for FSP_DIRT_L; menu.lwf, off the chain, holds MENU_ONLY; the profile
		// names both, an item binds it.
		made = editor_test::write_bytes(root + "/sounds/game.lwf", bank_bytes({"fs_dirt1"}, "FSP_DIRT_L",
		                                                                       lwf::kFlagInternal | lwf::kFlagExternal)) &&
		       editor_test::write_bytes(root + "/sounds/menu.lwf", bank_bytes({"tone"}, "MENU_ONLY", lwf::kFlagInternal)) &&
		       editor_test::write_bytes(root + "/sounds/fs_dirt1.wav", test_io::read_file(repo() + "/fixtures/lwf/tone.wav")) &&
		       editor_test::write_text(root + "/SndProf.def",
		                               "begin \"default\"\r\n\tSSLFootGND FSP_DIRT_L 0 0 0\r\n\tSSRFootGND MENU_ONLY 0 0 0\r\nend\r\n"
		                               "begin \"SP_Soldier\"\r\nend\r\n") &&
		       editor_test::write_text(root + "/defs/items.def",
		                               "begin \"Soldier\"\r\nid 100100\r\ntype person\r\nsound_profile SP_Soldier\r\n"
		                               "sounddeath FSP_DIRT_L\r\nend\r\n");
		// The blank SndProf.def Create Missing made goes: the project's own is the one above.
		std::error_code ec;
		std::filesystem::remove(opennova::io::os_path(root + "/defs/SndProf.def"), ec);
		editor_test::handle_to_end(session, request::rescan());
	}
};

const GraphEdge *edge_named(const AssetGraph &graph, const std::string &file, ReferenceKind kind, const std::string &value) {
	for (const GraphEdge *edge : graph.references_of(file))
		if (edge->kind == kind && strutil::iequals(edge->value, value)) return edge;
	return nullptr;
}

// A set is a bank's symbol, found in the game's bank order: a profile slot and an item's sounddeath on
// game.lwf's set resolve and are its users; a slot naming a set only menu.lwf has is missing, in words
// saying why; the item's sound_profile resolves to the profile; a member's wave to the bank's own wave;
// the picker offers the chain's sets, menu.lwf's as one no lookup finds.
int test_graph() {
	SoundProject project;
	TEST_EXPECT(project.made);
	const AssetGraph *graph = project.session.view().findings.graph.get();
	TEST_EXPECT(graph != nullptr);
	if (!graph) return 1;
	TEST_EXPECT(graph->bank_rank("sounds/game.lwf") != SIZE_MAX && !graph->on_bank_chain("menu.lwf"));
	TEST_EXPECT(graph->resolve(ReferenceKind::Sound, "fsp_dirt_l") == ReferenceStatus::Present &&
	            graph->resolve(ReferenceKind::Sound, "MENU_ONLY") == ReferenceStatus::Missing &&
	            graph->resolve(ReferenceKind::Sound, "MENU_ONLY", "MENU.LWF") == ReferenceStatus::Present);
	const GraphEdge *slot = edge_named(*graph, "SndProf.def", ReferenceKind::Sound, "FSP_DIRT_L");
	const GraphEdge *death = edge_named(*graph, "defs/items.def", ReferenceKind::Sound, "FSP_DIRT_L");
	const GraphEdge *profile = edge_named(*graph, "defs/items.def", ReferenceKind::SoundProfile, "SP_Soldier");
	TEST_EXPECT(slot && death && profile && graph->resolve(*profile) == ReferenceStatus::Present);
	const std::vector<const GraphSymbol *> sets = graph->symbols_named(ReferenceKind::Sound, "FSP_DIRT_L");
	TEST_EXPECT(sets.size() == 1);
	if (sets.size() == 1) TEST_EXPECT(graph->users_of(*sets.front()).size() == 2);
	bool said = false;
	for (const GraphEdge *missing : graph->missing())
		said = said || (missing->value == "MENU_ONLY" &&
		                graph->missing_finding(*missing).message.find("only menu.lwf has") != std::string::npos);
	TEST_EXPECT(said);
	const GraphEdge *member = edge_named(*graph, "sounds/game.lwf", ReferenceKind::BankWave, "FS_DIRT1");
	TEST_EXPECT(member && graph->resolve(*member) == ReferenceStatus::Present && member->scope == "GAME.LWF");
	bool live = false, inert = false;
	for (const ReferenceChoice &choice : graph->choices(ReferenceKind::Sound)) {
		live = live || (choice.name == "FSP_DIRT_L" && !choice.inert);
		inert = inert || (choice.name == "MENU_ONLY" && choice.inert && !choice.reason.empty());
	}
	TEST_EXPECT(live && inert);
	return 0;
}

// The blanks: game.lwf (Create Missing's optional role) and New > Sound bank make a bank the reader takes,
// holding nothing.
int test_blanks() {
	for (const char *role : {"game_lwf", "gamelocl_lwf", "game2_lwf", "game3_lwf", "expansion_lwf", "expansion_locl_lwf"}) {
		const BlankFactory *factory = find_blank_factory_for_role(role);
		TEST_EXPECT(factory && factory->kind == AssetKind::SoundBank);
	}
	BlankRequest request;
	request.logical_name = "game.lwf";
	request.role = "game_lwf";
	std::vector<uint8_t> bytes;
	Diagnostic error;
	TEST_EXPECT(make_blank(request, AssetKind::SoundBank, bytes, error));
	SoundBankDocument bank;
	TEST_EXPECT(bank.load_bytes(bytes, "game.lwf", AssetKind::SoundBank, "jo", error) && bank.rows().empty());
	TEST_EXPECT(find_blank_factory_for_kind(AssetKind::SoundBank) != nullptr);
	return 0;
}

// The session's play_sound: a set's voices (the project's wave at the pick's pitch and volume), a profile's
// slot, a footstep on a surface, and the refusals, each said; the wire's workspace section carries them.
int test_session_play() {
	SoundProject project;
	TEST_EXPECT(project.made);
	ProjectSession &session = project.session;
	const WorkspaceView::Sound &sound = session.view().workspace.sound;
	session.handle(request::play_set("FSP_DIRT_L"));
	TEST_EXPECT(sound.state == WorkspaceView::SoundState::Starting && sound.set == "FSP_DIRT_L" && sound.bank == "game.lwf" &&
	            sound.voices.size() == 1 && sound.voices[0].path == "sounds/fs_dirt1.wav" && sound.voices[0].volume == 200 &&
	            sound.path == "sounds/fs_dirt1.wav");
	const uint64_t serial = sound.serial;
	session.handle(request::play_profile_slot("default", "SSLFootGND"));
	TEST_EXPECT(sound.serial == serial + 1 && sound.set == "FSP_DIRT_L" && sound.words.find("default's SSLFootGND") == 0);
	session.handle(request::play_footstep("SP_Soldier", "ground", "left"));
	// SP_Soldier's SSLFootGND is empty: nothing plays, the status line says why.
	TEST_EXPECT(sound.serial == serial + 1 && session.view().activity.status.find("SP_Soldier's SSLFootGND is empty") != std::string::npos);
	session.handle(request::play_set("MENU_ONLY"));
	TEST_EXPECT(sound.serial == serial + 1); // off the chain
	session.handle(request::play_set("MENU_ONLY", "menu.lwf"));
	// Its bank found, its wave the project lacks: refused, saying which.
	TEST_EXPECT(sound.serial == serial + 1 && session.view().activity.status.find("tone.wav") != std::string::npos);
	// The wire: the request's values, and the section's voices.
	JsonValue request = JsonValue::make_object();
	request.set("kind", JsonValue::make_string("play_sound"));
	JsonValue values = JsonValue::make_object();
	values.set("profile", JsonValue::make_string("default"));
	values.set("slot", JsonValue::make_string("17"));
	request.set("values", std::move(values));
	const JsonValue answer = session.handle_json(request);
	TEST_EXPECT(answer.get_bool("ok", false) && sound.serial == serial + 2);
	const JsonValue workspace = workspace_to_json(session.view());
	const JsonValue *played = workspace.get("sound");
	const JsonValue *voices = played ? played->get("voices") : nullptr;
	TEST_EXPECT(played && played->get_string("set", "") == "FSP_DIRT_L" && voices && voices->array.size() == 1);
	session.handle(request::stop_sound());
	TEST_EXPECT(sound.state == WorkspaceView::SoundState::Stopped);
	return 0;
}

// --- the waves ----------------------------------------------------------------------------------------------

// A RIFF WAVE of `frames` frames of a 441 Hz sine at amplitude 0.5, each channel the same, its samples
// `bits` wide (8 unsigned, 16, 24; 32 float with `real`), a LIST chunk before its data when `list`.
std::vector<uint8_t> wave_of(uint16_t channels, uint16_t bits, uint32_t rate, size_t frames, bool list = false,
                             bool real = false) {
	std::vector<uint8_t> data;
	for (size_t f = 0; f < frames; ++f) {
		const double s = 0.5 * std::sin(2.0 * 3.14159265358979 * 441.0 * double(f) / double(rate));
		for (uint16_t c = 0; c < channels; ++c) {
			if (real) {
				float v = float(s);
				uint32_t u;
				std::memcpy(&u, &v, 4);
				io::append_u32_le(data, u);
			} else if (bits == 8) {
				data.push_back(uint8_t(std::lround(s * 127.0) + 128));
			} else if (bits == 16) {
				io::append_u16_le(data, uint16_t(int16_t(std::lround(s * 32767.0))));
			} else {
				const int32_t v = int32_t(std::lround(s * 8388607.0));
				data.push_back(uint8_t(v));
				data.push_back(uint8_t(v >> 8));
				data.push_back(uint8_t(v >> 16));
			}
		}
	}
	std::vector<uint8_t> out;
	const auto text = [&](const char *t) { out.insert(out.end(), t, t + 4); };
	std::vector<uint8_t> info;
	if (list) {
		const char body[] = "INFOISFT\x0e\0\0\0Lavf58.29.100\0";
		info.assign(body, body + sizeof(body) - 1);
	}
	text("RIFF");
	io::append_u32_le(out, uint32_t(4 + 24 + (list ? 8 + info.size() : 0) + 8 + data.size()));
	text("WAVE");
	text("fmt ");
	io::append_u32_le(out, 16);
	io::append_u16_le(out, real ? 3 : 1);
	io::append_u16_le(out, channels);
	io::append_u32_le(out, rate);
	io::append_u32_le(out, rate * channels * (bits / 8));
	io::append_u16_le(out, uint16_t(channels * (bits / 8)));
	io::append_u16_le(out, bits);
	if (list) {
		text("LIST");
		io::append_u32_le(out, uint32_t(info.size()));
		out.insert(out.end(), info.begin(), info.end());
	}
	text("data");
	io::append_u32_le(out, uint32_t(data.size()));
	out.insert(out.end(), data.begin(), data.end());
	return out;
}

// What the game's loader takes, by its own walk: the minted mono 16-bit tone; not a stereo wave, a 24-bit
// or float one, a LIST ahead of the data, an ADPCM wave with no fact chunk. A wave the game refuses
// converts into one it takes, the samples kept; a card's facts: the format, the peak and the RMS of a
// sine at 0.5, its picture.
int test_waves() {
	const std::vector<uint8_t> tone = test_io::read_file(repo() + "/fixtures/lwf/tone.wav");
	TEST_EXPECT(wave_retail_check(tone).plays);
	TEST_EXPECT(wave_retail_check(wave_of(1, 8, 11025, 100)).plays);
	const WaveRetailCheck stereo = wave_retail_check(wave_of(2, 16, 44100, 100));
	TEST_EXPECT(!stereo.plays && stereo.why.find("2 channels") != std::string::npos);
	TEST_EXPECT(wave_retail_check(wave_of(1, 24, 48000, 100)).why.find("24-bit") != std::string::npos);
	TEST_EXPECT(wave_retail_check(wave_of(1, 32, 48000, 100, false, true)).why.find("32-bit") != std::string::npos);
	const WaveRetailCheck listed = wave_retail_check(wave_of(1, 16, 22050, 100, true));
	TEST_EXPECT(!listed.plays && listed.why.find("LIST") != std::string::npos);
	// A LIST after the data is never reached.
	std::vector<uint8_t> after = wave_of(1, 16, 22050, 100);
	const char tail[] = "LIST\x04\0\0\0INFO";
	after.insert(after.end(), tail, tail + sizeof(tail) - 1);
	TEST_EXPECT(wave_retail_check(after).plays);
	std::vector<uint8_t> adpcm = wave_of(1, 16, 22050, 100);
	adpcm[20] = 0x11;
	adpcm[34] = 4;
	TEST_EXPECT(wave_retail_check(adpcm).why.find("fact") != std::string::npos);
	TEST_EXPECT(!wave_retail_check(text_bytes("not a wave")).plays);
	// Converted: mono 16-bit, the rate kept, then resampled.
	const std::vector<uint8_t> source = wave_of(2, 24, 48000, 4800, true);
	std::vector<uint8_t> converted;
	std::string error;
	TEST_EXPECT(convert_wave(source, WaveConversion(), converted, error) && wave_retail_check(converted).plays);
	WaveSamples back;
	TEST_EXPECT(decode_wave_source(converted, back, error) && back.channels == 1 && back.rate == 48000 &&
	            back.format.bits == 16 && back.frames() == 4800);
	WaveConversion halved;
	halved.rate = 24000;
	halved.bits = "8";
	TEST_EXPECT(convert_wave(source, halved, converted, error) && decode_wave_source(converted, back, error) &&
	            back.frames() == 2400 && back.format.bits == 8 && back.rate == 24000);
	// The card's facts.
	const WaveFacts facts = wave_facts(source, 16);
	TEST_EXPECT(facts.read && !facts.retail.plays && facts.format.channels == 2 && facts.format.bits == 24 &&
	            std::fabs(facts.seconds - 0.1) < 1e-6 && std::fabs(facts.peak - 0.5f) < 0.01f &&
	            std::fabs(facts.rms - 0.3536f) < 0.01f && facts.envelope.size() == 16);
	TEST_EXPECT(wave_format_words(facts.format) == "24-bit PCM, stereo, 48000 Hz");
	// The importer: a record's options make the wave the game plays, named by its option.
	ImportOptions options = {{"rate", "22050"}, {"name", "fs_dirt1.wav"}};
	ImportContext context("fs_dirt1_src.wav", source, options, ".", ".");
	ImportProduct product;
	const Importer *importer = importer_for("fs_dirt1_src.wav");
	TEST_EXPECT(importer && std::string(importer->id) == "wave" && !authored_importer_for("x.wav") &&
	            importer->run(context, product) && product.outputs.size() == 1 && product.outputs[0].name == "fs_dirt1.wav" &&
	            wave_retail_check(product.outputs[0].bytes).plays);
	TEST_EXPECT(import_option_row(importer->options, "rate") &&
	            import_option_accepts(*import_option_row(importer->options, "rate"), "32000") &&
	            !import_option_accepts(*import_option_row(importer->options, "rate"), "500"));
	return 0;
}

// An author's stereo wave imported into a project comes in as the game plays it, said; a wave the game
// cannot play left in the project is a warning on the file.
int test_wave_import() {
	SoundProject project;
	TEST_EXPECT(project.made);
	const std::string outside = project.dir.file("outside");
	TEST_EXPECT(editor_test::write_bytes(outside + "/stereo.wav", wave_of(2, 16, 44100, 441, true)));
	ImportChoice choice;
	choice.path = outside + "/stereo.wav";
	const SessionView &view = project.session.view();
	const ImportResult result = import_assets({choice}, ProjectPaths::for_root(view.project.root), *view.project.document, false);
	TEST_EXPECT(result.imported.size() == 1);
	bool said = false;
	for (const Diagnostic &d : result.diagnostics) said = said || (d.code() == "import.wave" && d.severity == DiagnosticSeverity::Info);
	TEST_EXPECT(said);
	const std::vector<uint8_t> landed = test_io::read_file(view.project.root + "/" + result.imported[0]);
	TEST_EXPECT(wave_retail_check(landed).plays);
	// A refused wave written over a project file: Problems says so.
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/sounds/fs_dirt1.wav", wave_of(2, 16, 44100, 441)));
	editor_test::handle_to_end(project.session, request::rescan());
	bool warned = false;
	for (const Diagnostic &d : view.findings.diagnostics)
		warned = warned || (d.code() == "asset.wave_unplayable" && d.asset == "sounds/fs_dirt1.wav");
	TEST_EXPECT(warned);
	return 0;
}

// --- retail ---------------------------------------------------------------------------------------------

// Every shipped bank through the document: its waves, sets, layers and members as the engine reads them,
// written from scratch and read back the same (each member on the wave of the same name); SndProf.def's 49
// profiles the same.
int test_retail_banks() {
	const std::string assets = retail::assets();
	if (assets.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS (the shipped banks through the bank document)");
		return 0;
	}
	size_t banks = 0;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(opennova::io::os_path(assets), ec)) {
		const std::string name = opennova::io::utf8_path(entry.path().filename());
		if (strutil::to_lower(entry.path().extension().string()) != ".lwf") continue;
		const std::vector<uint8_t> bytes = test_io::read_file(opennova::io::utf8_path(entry.path()));
		lwf::File original;
		std::string message;
		if (!lwf::parse_lwf_buffer(bytes.data(), bytes.size(), original, message)) continue; // D-SND-3's refusals
		SoundBankDocument bank;
		Diagnostic error;
		TEST_EXPECT(bank.load_bytes(bytes, name, AssetKind::SoundBank, "jo", error));
		const SerializeResult written = bank.serialize();
		TEST_EXPECT(written.ok());
		lwf::File again;
		TEST_EXPECT(lwf::parse_lwf_buffer(reinterpret_cast<const uint8_t *>(written.text.data()), written.text.size(), again, message));
		bool same = again.singles.size() == original.singles.size() && again.multis.size() == original.multis.size() &&
		            again.sndparms.size() == original.sndparms.size();
		for (size_t i = 0; same && i < original.singles.size(); ++i)
			same = again.singles[i].name == original.singles[i].name && again.singles[i].path == original.singles[i].path &&
			       (again.singles[i].value_hi >> 8) == (original.singles[i].value_hi >> 8);
		for (size_t s = 0; same && s < original.multis.size(); ++s) {
			const lwf::Multi &a = original.multis[s], &b = again.multis[s];
			same = a.name == b.name && a.pitch_base == b.pitch_base && a.pitch_random_range == b.pitch_random_range &&
			       a.target_id == b.target_id && a.set_flags == b.set_flags && a.playlist_indices.size() == b.playlist_indices.size();
			for (size_t l = 0; same && l < a.playlist_indices.size(); ++l) {
				const lwf::Playlist &pa = original.playlists[a.playlist_indices[l]], &pb = again.playlists[b.playlist_indices[l]];
				same = pa.falloff_radius == pb.falloff_radius && pa.min_distance == pb.min_distance && pa.flags == pb.flags &&
				       pa.sndparm_indices.size() == pb.sndparm_indices.size();
				for (size_t m = 0; same && m < pa.sndparm_indices.size(); ++m) {
					const lwf::Sndparm &ma = original.sndparms[pa.sndparm_indices[m]], &mb = again.sndparms[pb.sndparm_indices[m]];
					same = original.singles[ma.single_index].name == again.singles[mb.single_index].name &&
					       ma.pitch_scaled == mb.pitch_scaled && ma.random_pitch_scaled == mb.random_pitch_scaled &&
					       ma.volume == mb.volume && ma.clamp_volume == mb.clamp_volume;
				}
			}
		}
		if (!same) std::fprintf(stderr, "%s does not read back the same\n", name.c_str());
		TEST_EXPECT(same);
		++banks;
	}
	std::printf("retail: %zu banks through the document\n", banks);
	// Every shipped wave the game's loader takes, by the check's own walk, but one: DSkid.wav, the 16-bit
	// stereo wave game.lwf's IMP_TMBL_DSKID plays, which the loader's channel test refuses [orig:
	// Audio_LoadWavFileFromArchive @ 0x7666db], so retail plays nothing for that set (D-SND-33).
	size_t waves = 0;
	std::vector<std::string> refused;
	for (const auto &entry : std::filesystem::directory_iterator(opennova::io::os_path(assets), ec)) {
		if (strutil::to_lower(entry.path().extension().string()) != ".wav") continue;
		const WaveRetailCheck check = wave_retail_check(test_io::read_file(opennova::io::utf8_path(entry.path())));
		++waves;
		if (!check.plays) refused.push_back(strutil::to_lower(opennova::io::utf8_path(entry.path().filename())));
	}
	std::printf("retail: %zu waves, %zu the check refuses\n", waves, refused.size());
	TEST_EXPECT(waves > 100 && refused == std::vector<std::string>({"dskid.wav"}));
	TEST_EXPECT(banks >= 3);
	const std::string sndprof = retail::asset_file("sndprof.def");
	if (!sndprof.empty()) {
		SoundProfileDocument profiles;
		Diagnostic error;
		TEST_EXPECT(profiles.load_bytes(test_io::read_file(sndprof), "SndProf.def", AssetKind::SoundProfileDefs, "jo", error));
		TEST_EXPECT(profiles.rows().size() == 49 && profiles.serialize().ok());
	}
	return 0;
}

// An import of the install's ammo.def with the files it needs brings the banks its sounds are sets of
// (DI-02: a set is a bank's symbol, followed like any other): game.lwf among the plan's rows, no sound left
// unfollowed.
int test_retail_import_follows_sounds() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (an install import following ammo.def's sounds to their banks)");
		return 0;
	}
	// A project of no bank of its own (one of the name would keep its copy: ImportPlan::shadowed).
	editor_test::TempProjectDir dir("opennova_editor_sound_import_plan");
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Plan"));
	const SessionView &view = session.view();
	TEST_EXPECT(view.project.open && view.findings.graph);
	if (!view.findings.graph) return 1;
	ImportChoice ammo;
	ammo.path = install;
	ammo.entry = "ammo.def";
	ammo.install = true;
	const ImportPlan plan = plan_import({ammo}, true, ProjectPaths::for_root(view.project.root), *view.project.document,
	                                    *view.project.scan, *view.findings.graph, install);
	bool bank = false;
	for (const ImportPlanRow &row : plan.rows) bank = bank || (strutil::iequals(row.name, "game.lwf") || strutil::iequals(row.name, "gamelocl.lwf"));
	bool unfollowed = false;
	for (const ImportNotFollowed &entry : plan.not_followed) unfollowed = unfollowed || entry.reference == ReferenceKind::Sound;
	std::printf("retail: ammo.def's plan, %zu rows, a bank among them: %s\n", plan.rows.size(), bank ? "yes" : "no");
	for (const ImportNotFollowed &entry : plan.undefined)
		std::printf("  undefined: %s x%zu, first in %s\n", reference_row(entry.reference).token, entry.count, entry.first.c_str());
	TEST_EXPECT(bank && !unfollowed);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failed = 0;
	failed += test_bank_reads_and_writes_back();
	failed += test_bank_edits();
	failed += test_bank_findings();
	failed += test_profiles();
	failed += test_preview_picks();
	failed += test_graph();
	failed += test_blanks();
	failed += test_session_play();
	failed += test_waves();
	failed += test_wave_import();
	failed += test_retail_banks();
	failed += test_retail_import_follows_sounds();
	if (failed == 0) std::printf("editor sound documents: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
