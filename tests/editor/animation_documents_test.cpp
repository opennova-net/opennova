// The clip and animation map documents (ADR 0046 S10e) over the neutral core. A clip
// (fixtures/anim/idle.bad, walk.bad) saves untouched as its own bytes; its engine data
// (frame rate, loop flag, bone names, each frame's step, triggers and heights) edits
// round-trip through the writer; the motion's fields and structure are refused. A table
// (fixtures/anim/soldier.adm) saves in the canonical form and reads back its rows; rows
// and clips add, move and remove, at most 8 clips; a table with no anim_reset row or a
// row the writer refuses does not save; the validator's findings; a row's clip is the
// graph's edge to a .bad. S12: a table's rows duplicate, move and remove; the lines a table
// leaves out are findings Save drops (a ten-clip row blocks); a slot two rows name is a note;
// a bone's parent is named by the parent bone, and one numbered after it is a warning.
// A SKIP-LEG opens every retail clip and table and saves each untouched field-equal /
// row-equal.
#include <editor/assets/asset_registry.h>
#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/animation_slots.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/project_validation.h>
#include <editor/graph/graph_layer.h>
#include <editor/preview/animation_uses.h>
#include <editor/preview/model_preview_rig.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <formats/adm/adm.h>
#include <formats/bad/bad.h>
#include <formats/bad/bad_build.h>
#include <formats/bad/bad_write.h>

#include "common/bad_equal.h"
#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"

using namespace opennova;
using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

constexpr NodeKind kBone = node_kind(AnimationKind::Bone);
constexpr NodeKind kEvent = node_kind(AnimationKind::Event);
constexpr NodeKind kRow = node_kind(AnimationMapKind::Row);
constexpr NodeKind kMapClip = node_kind(AnimationMapKind::Clip);

std::string anim_dir() { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/anim"; }

std::vector<uint8_t> serialized(const Document &document) {
	const SerializeResult result = document.serialize();
	return std::vector<uint8_t>(result.text.begin(), result.text.end());
}

Edit set(NodeAddress address, const char *field, Value value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

Edit op(EditOperation operation, NodeAddress address, size_t position = SIZE_MAX, NodeId parent = 0) {
	Edit edit;
	edit.operation = operation;
	edit.address = address;
	edit.position = position;
	edit.parent = parent;
	return edit;
}

// A four-bone chain minted through the construction seam, each bone hung off the one before
// it, two frames at rest.
std::vector<uint8_t> chain_clip() {
	bad::BadBuildClip clip;
	clip.name = "chain";
	clip.frame_count = 1;
	const char *const names[] = {"BN01 Root", "BN02 Hips", "BN03 Spine", "BN04 Head"};
	for (int i = 0; i < 4; ++i) {
		bad::BadBuildBone bone;
		bone.name = names[i];
		bone.parent = i - 1;
		bone.pivot = bad::BadBuildVec3{0.0, 0.0, double(i)};
		bone.length = 1.0;
		bone.keys.assign(clip.frame_count + 1, bad::BadBuildQuat{});
		clip.bones.push_back(bone);
	}
	std::vector<uint8_t> bytes;
	std::string error;
	if (!bad::bad_build_mint(clip, nullptr, bad::bad_retail_limits(), bytes, &error)) std::printf("chain_clip: %s\n", error.c_str());
	return bytes;
}

// S12: a bone's parent is a choice among the clip's bones, named by the parent bone (a root's
// by none), the index the file writes kept as its token. S13 D2: the bone's own choices
// (Document::record_choices), of the records the clip row holds.
int bone_parents() {
	AnimationDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(chain_clip(), "chain.bad", AssetKind::Animation, "jo", error));
	const ClipRow *clip = document.clip();
	TEST_EXPECT(clip && clip->bones.size() == 4);
	if (!clip || clip->bones.size() != 4) return 1;
	for (const size_t i : {size_t(0), size_t(2)}) {
		const NodeAddress bone{clip->id, kBone, clip->collections[0][i]};
		FieldUse parent;
		for (const FieldSchema &field : document.fields(kBone))
			if (field.id == "parent") parent = document.field_on(bone, field);
		TEST_EXPECT(parent.schema && parent.own_choices && parent.schema->choices.empty() &&
		            parent.record_owner == NodeAddress({clip->id, clip->kind, 0}));
		if (!parent.schema) return 1;
		std::vector<FieldChoice> own;
		const std::vector<FieldChoice> &choices = document.choices_on(bone, parent, own);
		const int64_t index = clip->bones[i].parent_index;
		const auto named = std::find_if(choices.begin(), choices.end(),
		                                [&](const FieldChoice &choice) { return choice.value == index; });
		TEST_EXPECT(parent.read_only && &choices == &own && choices.size() == 5 && named != choices.end());
		if (named == choices.end()) return 1;
		TEST_EXPECT(named->name == std::to_string(index) &&
		            named->label == (i == 0 ? std::string("None (a root)") : std::string(clip->bones[1].name)));
	}
	std::printf("bone parents: a root's none, a bone's by its parent's name\n");
	return 0;
}

int clips() {
	for (const char *name : {"idle.bad", "walk.bad"}) {
		AnimationDocument document;
		Diagnostic error;
		TEST_EXPECT(document.load(anim_dir() + "/" + name, name, AssetKind::Animation, "jo", error));
		TEST_EXPECT(serialized(document) == test_io::read_file(anim_dir() + "/" + name));
	}
	editor_test::TempProjectDir dir("opennova_animation_documents_test");
	TEST_EXPECT(editor_test::write_bytes(dir.file("walk.bad"), test_io::read_file(anim_dir() + "/walk.bad")));
	AnimationDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load(dir.file("walk.bad"), "walk.bad", AssetKind::Animation, "jo", error));
	const ClipRow *clip = document.clip();
	TEST_EXPECT(clip && !clip->bones.empty() && !clip->events.empty());
	const NodeAddress row{clip->id, node_kind(AnimationKind::Clip), 0};
	const NodeAddress event{clip->id, kEvent, clip->collections[1][0]};
	const NodeAddress bone{clip->id, kBone, clip->collections[0][0]};
	Value value;

	// The engine data: the frame rate, the loop flag, a bone's name, a frame's step,
	// triggers and heights.
	TEST_EXPECT(document.apply(set(row, "fps", int64_t(25)), error));
	TEST_EXPECT(document.apply(set(bone, "name", std::string("BN01 Hips")), error));
	TEST_EXPECT(document.apply(set(event, "velocity.x", 0.25), error));
	TEST_EXPECT(document.get(event, "velocity.x", value) && std::get<double>(value) == 0.25);
	TEST_EXPECT(document.apply(set(event, "bottom", 1.0), error));
	if (clip->version == 1) {
		TEST_EXPECT(document.apply(set(event, "trigger", int64_t(1)), error));
		// S17: a frame event by its frame from 0, as the timeline counts it, and what it fires in words.
		TEST_EXPECT(document.record_title(event) == "Frame 0: left footstep");
		TEST_EXPECT(document.apply(set(event, "trigger", int64_t(0x2 | 0x40 | 0x10000)), error));
		TEST_EXPECT(document.record_title(event) == "Frame 0: right footstep, sound 2, an unread bit (0x10000)");
		TEST_EXPECT(document.apply(set(event, "trigger", int64_t(0)), error) && document.record_title(event) == "Frame 0");
		TEST_EXPECT(document.apply(set(event, "trigger", int64_t(1)), error));
	}
	const NodeAddress second{clip->id, kEvent, clip->collections[1][1]};
	// The record's own name counts from 0 too (what the MCP's batches name it by).
	TEST_EXPECT(document.record_title(second).rfind("Frame 1", 0) == 0 && document.record_name(second) == "Frame event 1");
	// The last record is the end pose: its event never fires, which its title and a finding say.
	const size_t last = clip->events.size() - 1;
	const NodeAddress end_pose{clip->id, kEvent, clip->collections[1][last]};
	TEST_EXPECT(clip->base && last == clip->base->frame_count && animation_end_pose(*clip, last) &&
	            !animation_end_pose(*clip, last - 1));
	TEST_EXPECT(document.record_title(end_pose).rfind("Frame " + std::to_string(last) + " (the end pose", 0) == 0);
	const auto end_pose_findings = [&] {
		size_t n = 0;
		for (const Diagnostic &d : validate_animation_file(document))
			n += d.row() == &finding_code(AnimationFinding::EndPoseTrigger) && d.child_id == end_pose.child ? 1 : 0;
		return n;
	};
	if (clip->version == 1) {
		TEST_EXPECT(document.apply(set(end_pose, "trigger", int64_t(0)), error) && end_pose_findings() == 0);
		TEST_EXPECT(document.apply(set(end_pose, "trigger", int64_t(2)), error));
		TEST_EXPECT(document.record_title(end_pose) ==
		            "Frame " + std::to_string(last) + " (the end pose): right footstep, which never fires");
		TEST_EXPECT(end_pose_findings() == 1);
		TEST_EXPECT(document.apply(set(end_pose, "trigger", int64_t(0)), error) && end_pose_findings() == 0);
	}
	TEST_EXPECT(document.record_title(bone) == document.record_name(bone));
	// The motion's: the translation flag, the keys, the structure.
	const int64_t flags = int64_t(document.clip()->flags);
	TEST_EXPECT(!document.apply(set(row, "flags", flags ^ int64_t(bad::BAD_FLAG_TRANSLATION)), error));
	TEST_EXPECT(document.apply(set(row, "flags", flags ^ int64_t(bad::BAD_FLAG_LOOP)), error));
	TEST_EXPECT(!document.apply(set(bone, "length", 2.0), error));
	TEST_EXPECT(!document.apply(op(EditOperation::Remove, event), error));
	if (document.clip()->version == 1 && document.clip()->events[0].trigger != 0)
		TEST_EXPECT(!document.apply(set(row, "version", int64_t(0)), error));

	// The written clip reads back with the edits and the motion as it was.
	const std::vector<uint8_t> bytes = serialized(document);
	bad::BadFile back{}, original{};
	const std::vector<uint8_t> source = test_io::read_file(anim_dir() + "/walk.bad");
	TEST_EXPECT(bad::bad_parse_buffer(bytes.data(), bytes.size(), &back) == 0);
	TEST_EXPECT(bad::bad_parse_buffer(source.data(), source.size(), &original) == 0);
	TEST_EXPECT(back.fps == 25 && std::strcmp(back.bones[0].name, "BN01 Hips") == 0 && back.events[0].bottom == 1.0f);
	TEST_EXPECT(back.num_channels == original.num_channels);
	for (size_t c = 0; c < back.num_channels; ++c)
		TEST_EXPECT(back.channels[c].frame_count == original.channels[c].frame_count &&
		            std::memcmp(back.channels[c].rotations, original.channels[c].rotations,
		                        sizeof(bad::BadQuaternion) * original.channels[c].frame_count) == 0);
	bad::bad_free(&back);
	bad::bad_free(&original);

	TEST_EXPECT(document.save(error));
	while (document.can_undo()) document.undo();
	TEST_EXPECT(serialized(document) == source);
	std::printf("clips: untouched saves, engine data edits, the motion refused, undo\n");
	return 0;
}

int tables() {
	editor_test::TempProjectDir dir("opennova_animation_map_document_test");
	const std::vector<uint8_t> source = test_io::read_file(anim_dir() + "/soldier.adm");
	TEST_EXPECT(editor_test::write_bytes(dir.file("soldier.adm"), source));
	AnimationMapDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load(dir.file("soldier.adm"), "soldier.adm", AssetKind::AnimationMap, "jo", error));
	TEST_EXPECT(document.rows().size() == 7 && document.rows()[0]->name() == "anim_reset");
	// S11e: a slot shows as its words, the key the file writes kept as the choice's token.
	bool walk_forward = false;
	for (const FieldChoice &choice : document.fields(kRow).front().choices)
		walk_forward = walk_forward || (choice.name == "anim_walk_forward" && choice.label == "walk forward");
	TEST_EXPECT(walk_forward);
	// The windows name a row by those words and the clips it plays (S17; the key, as the game compares
	// it, kept for the graph, Problems and the MCP); a key naming no slot as it is; a clip by its file.
	const NodeAddress first{document.rows()[0]->id, kRow, 0};
	TEST_EXPECT(document.record_title(first) == "reset: idle.bad" && document.record_name(first) == "anim_reset");
	TEST_EXPECT(animation_key_title("ANIM_WALK_FORWARD") == "walk forward");
	TEST_EXPECT(animation_key_title("anim_no_such_slot") == "anim_no_such_slot" && animation_key_title("") == "");
	const Document::Collection clips = document.collections_of(first).front();
	TEST_EXPECT(!clips.ids.empty() && document.record_title({first.row, kMapClip, clips.ids.front()}) ==
	                                          document.record_name({first.row, kMapClip, clips.ids.front()}));
	// The rows under their slots' families, in the families' order (S17).
	std::vector<std::vector<RowHeading>> headings;
	animation_map_row_headings(document, nullptr, headings);
	TEST_EXPECT(headings.size() == document.rows().size());
	TEST_EXPECT(headings[0].size() == 1 && headings[0][0].text == "Reset (the rest pose)");
	TEST_EXPECT(headings[1][0].text == "Standing still" && headings[3][0].text == "Walking and running");
	TEST_EXPECT(headings[0][0].key < headings[3][0].key && headings[3][0].key < headings[1][0].key);
	// What the game does with a row, in words: the walk's meaning, the reset row's rules and the body
	// slots the map leaves out (233 of 239: it names six besides the reset).
	const NodeAddress walk_row{document.rows()[3]->id, kRow, 0};
	const std::vector<std::string> walk_notes = map_row_notes(document, walk_row);
	TEST_EXPECT(walk_notes.size() == 1 && walk_notes[0] == "walk forward (slot 1): Walking forward.");
	TEST_EXPECT(map_unauthored_slots(document).size() == 233);
	// The slots left out by what the game does with each (S17 review: most are never picked without
	// a clip, not played as the reset clip).
	const std::vector<std::string> reset_notes = map_row_notes(document, first);
	TEST_EXPECT(reset_notes.size() == 3 && reset_notes[0].find("The rest pose") != std::string::npos);
	TEST_EXPECT(reset_notes.size() == 3 &&
	            reset_notes[1].rfind("This map leaves out 233 slots, each serving this row's first clip: the game plays it in ", 0) == 0 &&
	            reset_notes[1].find(", never picks ") != std::string::npos &&
	            reset_notes[1].find(" whether it picks them then is not traced. A mission's forced animation of any of them plays "
	                                "this row's first clip.") != std::string::npos);
	TEST_EXPECT(reset_notes.size() == 3 && reset_notes[2].find("crashes the game as it loads") != std::string::npos);
	// A row none of whose clips loads authors nothing: the walk, its file not in the project.
	const ClipLoads no_walk = [](const std::string &clip) { return clip != "walk.bad"; };
	TEST_EXPECT(map_unauthored_slots(document, no_walk).size() == 236);
	const std::vector<std::string> lost = map_row_notes(document, walk_row, no_walk);
	TEST_EXPECT(lost.size() == 2 && lost[1] == "None of its clips loads. Left out, the body plays the reset clip in it.");
	// Every slot has a meaning and a family; a slot past the table has neither.
	for (int slot = 0; slot < 252; ++slot)
		TEST_EXPECT(!animation_slot_meaning(slot).empty() && animation_slot_family(slot) >= 0);
	TEST_EXPECT(animation_slot_meaning(252).empty() && animation_slot_family(-1) < 0);
	TEST_EXPECT(animation_slot_meaning(232) == "Shot dead in the right foot, from the front: the bone the round hit "
	                                           "picks the part, its heading the side.");
	// S17 review: what the records witness, and "not traced" only where they do not. Head tracking's
	// look idles stand among the still; pre attack, out of ground and the guard's look are witnessed.
	for (const int slot : {125, 126, 141, 152, 153, 165, 166})
		TEST_EXPECT(animation_slot_meaning(slot).find("not traced") == std::string::npos);
	TEST_EXPECT(std::string(animation_slot_family_row(animation_slot_family(125)).heading) == "Standing still" &&
	            std::string(animation_slot_family_row(animation_slot_family(126)).heading) == "Standing still");
	TEST_EXPECT(animation_slot_meaning(153) == "Out of the ground: an NPC reset or respawned plays it when the map has "
	                                           "it, else idle 2.");
	for (const int slot : {159, 160, 161, 162, 163, 164, 240})
		TEST_EXPECT(animation_slot_meaning(slot).find("not traced") != std::string::npos);
	// The emplaced gunner: phrase_set N plays slot 67 + N (phrase_set 4 plays emplaced 5).
	TEST_EXPECT(animation_slot_words(71) == "emplaced 5" && animation_slot_meaning(71).find("phrase_set is 4") != std::string::npos);
	// What a slot left out comes to: the reset clip where its selector does not test for the clip, never
	// picked where it does, and not traced elsewhere.
	for (const int slot : {1, 44, 49, 50, 64, 67, 200, 241})
		TEST_EXPECT(animation_slot_absence(slot) == AnimSlotAbsence::PlaysReset);
	for (const int slot : {9, 10, 27, 36, 47, 68, 75, 115, 125, 140, 141, 147, 148, 151, 153, 155, 165, 167, 168})
		TEST_EXPECT(animation_slot_absence(slot) == AnimSlotAbsence::NotPicked);
	for (const int slot : {2, 11, 43, 62, 76, 130, 159, 163, 240})
		TEST_EXPECT(animation_slot_absence(slot) == AnimSlotAbsence::Untraced);
	TEST_EXPECT(animation_slot_when_missing(147) == "Left out, the game never picks it: the NPC stands in the default idle instead.");
	TEST_EXPECT(animation_slot_when_missing(1) == "Left out, the body plays the reset clip in it.");
	TEST_EXPECT(animation_slot_when_missing(241) == "Left out, an action that plays it plays the reset clip in it.");
	TEST_EXPECT(animation_slot_when_missing(2).find("not traced") != std::string::npos);
	TEST_EXPECT(animation_slot_when_missing(0).empty());
	// The canonical form reads back the rows it was read from.
	{
		const std::vector<uint8_t> bytes = serialized(document);
		adm::AdmFile a{}, b{};
		TEST_EXPECT(adm::adm_parse_buffer(reinterpret_cast<const char *>(source.data()), source.size(), &a) == 0);
		TEST_EXPECT(adm::adm_parse_buffer(reinterpret_cast<const char *>(bytes.data()), bytes.size(), &b) == 0);
		TEST_EXPECT(a.count == b.count);
		for (size_t i = 0; i < a.count; ++i) TEST_EXPECT(std::strcmp(a.entries[i].key, b.entries[i].key) == 0);
		adm::adm_free(&a);
		adm::adm_free(&b);
	}
	// A row added (anim_idle: the table has its reset row), given a clip, moved first, removed.
	TEST_EXPECT(document.apply(op(EditOperation::Add, {0, kRow, 0}), error));
	const NodeId added = document.last_added();
	Value key;
	TEST_EXPECT(document.get({added, kRow, 0}, "key", key) && std::get<std::string>(key) == "anim_idle");
	TEST_EXPECT(!document.serialize().ok()); // a row with no clip
	TEST_EXPECT(document.apply(op(EditOperation::Add, {added, kMapClip, 0}, SIZE_MAX, 0), error));
	TEST_EXPECT(document.apply(set({added, kMapClip, document.last_added()}, "clip", std::string("walk.bad")), error));
	TEST_EXPECT(document.apply(set({added, kRow, 0}, "key", std::string("anim_crouch_idle")), error));
	TEST_EXPECT(document.serialize().ok());
	TEST_EXPECT(document.apply(op(EditOperation::Move, {added, kRow, 0}, 0), error));
	TEST_EXPECT(document.rows()[0]->id == added);
	for (int i = 0; i < 7; ++i) TEST_EXPECT(document.apply(op(EditOperation::Add, {added, kMapClip, 0}), error));
	TEST_EXPECT(!document.apply(op(EditOperation::Add, {added, kMapClip, 0}), error)); // a ninth
	TEST_EXPECT(document.apply(op(EditOperation::Remove, {added, kRow, 0}), error));
	// No anim_reset row: the game cannot load the table.
	const NodeId reset = document.rows()[0]->id;
	TEST_EXPECT(document.apply(op(EditOperation::Remove, {reset, kRow, 0}), error));
	TEST_EXPECT(!document.serialize().ok());
	document.undo();
	TEST_EXPECT(document.serialize().ok());
	// S12: the rows are a list like a row's clips: a row duplicated right after itself with
	// its clips, moved first, removed, each one step.
	const NodeId idle = document.rows()[1]->id;
	TEST_EXPECT(document.apply(op(EditOperation::Duplicate, {idle, kRow, 0}, 2), error));
	const NodeId copy = document.last_added();
	TEST_EXPECT(document.rows().size() == 8 && document.rows()[2]->id == copy && copy != idle);
	TEST_EXPECT(document.rows()[2]->name() == "anim_idle" && document.collections_of({copy, kRow, 0}).front().ids.size() ==
	                                                                  document.collections_of({idle, kRow, 0}).front().ids.size());
	TEST_EXPECT(document.apply(op(EditOperation::Move, {copy, kRow, 0}, 0), error) && document.rows()[0]->id == copy);
	TEST_EXPECT(document.apply(op(EditOperation::Remove, {copy, kRow, 0}), error) && document.rows().size() == 7);
	document.undo();
	TEST_EXPECT(document.rows().size() == 8 && document.rows()[0]->id == copy);
	document.undo();
	document.undo();
	TEST_EXPECT(document.rows().size() == 7 && document.rows()[1]->id == idle);
	// The key takes a key the list does not know, written as typed (the validator names it).
	TEST_EXPECT(document.fields(kRow).front().open_choices);
	std::printf("tables: canonical saves, rows and clips, the 8-clip cap, the reset row, rows as a list\n");
	return 0;
}

// S12: the lines a table leaves out. A comment line and a comment after a row's clips are
// input the game ignores: source findings on their lines (the second on its row) that the
// toolbar counts and Save drops; the save's text reports none. A row naming ten clips blocks
// the file: no edit, no save.
// S17 review: a map's slots left out are its own kind's. A weapon's map (only first-person slots
// past its reset) leaves out first-person slots, wpn_reset among them; a body's map that names a
// first-person slot too stays a body's.
int maps_by_kind() {
	editor_test::TempProjectDir dir("opennova_animation_map_kinds_test");
	const auto load = [&](const char *name, const std::string &text, AnimationMapDocument &document) -> int {
		TEST_EXPECT(editor_test::write_text(dir.file(name), text));
		Diagnostic error;
		TEST_EXPECT(document.load(dir.file(name), name, AssetKind::AnimationMap, "jo", error));
		return 0;
	};
	AnimationMapDocument weapon;
	if (load("wpn.adm", "anim_reset\t\"wpn.bad\"\r\nanim_wpn_idle\t\"wpn.bad\"\r\nanim_wpn_fire\t\"wpn.bad\"\r\n", weapon)) return 1;
	const std::vector<int> left_out = map_unauthored_slots(weapon);
	TEST_EXPECT(left_out.size() == 10 && left_out.front() == 240 && left_out.back() == 251);
	TEST_EXPECT(std::find(left_out.begin(), left_out.end(), 241) == left_out.end() &&
	            std::find(left_out.begin(), left_out.end(), 243) == left_out.end());
	AnimationMapDocument body;
	if (load("body.adm", "anim_reset\t\"r.bad\"\r\nanim_walk_forward\t\"w.bad\"\r\nanim_wpn_idle\t\"x.bad\"\r\nanim_idle\r\n", body))
		return 1;
	const std::vector<int> body_out = map_unauthored_slots(body);
	TEST_EXPECT(body_out.size() == 238 && body_out.front() == 2 && body_out.back() == 239);
	return 0;
}

// S17 review: a clip no map names, or a map no item names, says what the game makes of it: the
// files the game names itself are played without one naming them; with a base layer mounted, whose
// files make no edges, it says only that no file of the project names it. And the timeline's
// letters: a shot (an NPC's), a footstep, a sound, none for bits the engine does not read alone.
int unused_files_and_letters() {
	AssetGraph graph;
	TEST_EXPECT(clip_unused_words(graph, "anims/walk.bad") == "No map of the project names this clip: the game never plays it.");
	TEST_EXPECT(clip_unused_words(graph, "anims/FailSafe.BAD").find("in place of any clip") != std::string::npos);
	TEST_EXPECT(clip_unused_words(graph, "anims/dt1rst.bad").find("player preview binds its skeleton") != std::string::npos);
	TEST_EXPECT(clip_unused_words(graph, "PI_Idle.bad").find("player preview plays it") != std::string::npos);
	TEST_EXPECT(map_unused_words(graph, "anims/Default.adm").find("any whose map it lacks") != std::string::npos);
	TEST_EXPECT(map_unused_words(graph, "anims/x.adm") == "No item or weapon of the project names this map: the game plays it for none.");
	graph.set_base(GraphLayer::build({}, "jo"));
	TEST_EXPECT(clip_unused_words(graph, "anims/walk.bad").find("base layer") != std::string::npos &&
	            clip_unused_words(graph, "anims/walk.bad").find("never plays") == std::string::npos);
	TEST_EXPECT(map_unused_words(graph, "anims/x.adm").find("base layer") != std::string::npos);
	TEST_EXPECT(std::string(preview_event_letter(0x4 | 0x1)) == "F" && std::string(preview_event_letter(0x1)) == "L" &&
	            std::string(preview_event_letter(0x2 | 0x40)) == "R" && std::string(preview_event_letter(0x400)) == "S" &&
	            std::string(preview_event_letter(0x10000)).empty());
	// A joint by the model's part name (BN##, 1-based), the bind clip's own name kept beside it.
	TEST_EXPECT(preview_joint_name(0, "BN01 Hips") == "BN01 Hips" && preview_joint_name(7, "RArm") == "BN08 RArm" &&
	            preview_joint_name(3, "MDL3") == "BN04" && preview_joint_name(11, "") == "BN12");
	return 0;
}

int table_lines() {
	editor_test::TempProjectDir dir("opennova_animation_map_lines_test");
	TEST_EXPECT(editor_test::write_text(dir.file("notes.adm"),
	                                    "// the soldier\r\nanim_reset\t\"idle.bad\"\r\nanim_idle\t\"idle.bad\" // old\r\n"));
	AnimationMapDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load(dir.file("notes.adm"), "notes.adm", AssetKind::AnimationMap, "jo", error));
	TEST_EXPECT(!document.blocked() && document.ignored_lines() == 2 && document.rows().size() == 2);
	if (document.issues().size() != 2) return 1;
	const SourceIssue &comment = document.issues()[0];
	const SourceIssue &tail = document.issues()[1];
	TEST_EXPECT(comment.line == 1 && comment.locator.empty() && !comment.blocks);
	TEST_EXPECT(tail.line == 3 && tail.record == "anim_idle" && tail.field == "key" &&
	            document.address_at(tail.locator).row == document.rows()[1]->id);
	TEST_EXPECT(document.rewrite_need() == Document::RewriteNeed::Rewrite);
	TEST_EXPECT(document.save(error) && document.issues().empty() && document.rewrite_need() == Document::RewriteNeed::None);
	const std::vector<uint8_t> written = test_io::read_file(dir.file("notes.adm"));
	TEST_EXPECT(std::string(written.begin(), written.end()).find("//") == std::string::npos);

	TEST_EXPECT(editor_test::write_text(dir.file("ten.adm"), "anim_reset\t\"idle.bad\"\r\nanim_idle a b c d e f g h i j\r\n"));
	AnimationMapDocument ten;
	TEST_EXPECT(ten.load(dir.file("ten.adm"), "ten.adm", AssetKind::AnimationMap, "jo", error));
	TEST_EXPECT(ten.blocked() && ten.issues().size() == 1 && ten.issues()[0].blocks && ten.issues()[0].line == 2);
	TEST_EXPECT(!ten.serialize().ok() && !ten.apply(op(EditOperation::Add, {0, kRow, 0}), error));
	std::printf("lines: comments dropped on save and reported, a ten-clip row blocks\n");
	return 0;
}

// A slot a weapon action names and the map lacks (DI-15's Add it there, define_animation_slot): a row of the key as
// the action writes it at the table's end, its clips none yet (the save waits for one); none for a key naming no
// slot, for a slot a row already holds, for another map's scope.
int define_slot() {
	editor_test::TempProjectDir dir("opennova_animation_map_define_test");
	TEST_EXPECT(editor_test::write_text(dir.file("gun.adm"), "anim_reset\t\"idle.bad\"\r\nanim_idle\t\"idle.bad\"\r\n"));
	AnimationMapDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load(dir.file("gun.adm"), "gun.adm", AssetKind::AnimationMap, "jo", error));
	ReferenceSubject missing;
	missing.kind = ReferenceKind::AnimationKey;
	missing.target = "anim_reload";
	missing.scope = "GUN.ADM";
	PlannedFix planned;
	TEST_EXPECT(define_animation_slot(document, missing, planned) && planned.edits.size() == 1);
	TEST_EXPECT(planned.label == "Add the slot anim_reload to gun.adm");
	TEST_EXPECT(document.apply(planned.edits, error) && document.rows().size() == 3);
	Value key;
	TEST_EXPECT(document.get({document.rows()[2]->id, kRow, 0}, "key", key) && std::get<std::string>(key) == "anim_reload");
	TEST_EXPECT(!document.serialize().ok()); // a row names at least one clip
	document.undo();
	TEST_EXPECT(document.rows().size() == 2);
	ReferenceSubject other = missing;
	other.target = "anim_nothing_at_all";
	TEST_EXPECT(!define_animation_slot(document, other, planned));
	other.target = "ANIM_IDLE";
	TEST_EXPECT(!define_animation_slot(document, other, planned));
	other = missing;
	other.scope = "OTHER.ADM";
	TEST_EXPECT(!define_animation_slot(document, other, planned));
	std::printf("define: a missing slot added as a row of no clip; none for no slot, a held one, another map\n");
	return 0;
}

// S12 review: a line's finding stays on the row it was read into while the rows move (the
// locator names the file as loaded: the saved baseline), and goes to the file once that row
// is removed; a row added in its place never takes it.
int findings_follow_rows() {
	editor_test::TempProjectDir dir("opennova_animation_map_follow_test");
	const std::string root = dir.file("Game");
	ProjectDocument project;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "Anim Game", "jo", project, error));
	const ProjectPaths paths = ProjectPaths::for_root(root);
	TEST_EXPECT(editor_test::write_text(root + "/notes.adm", "anim_reset\t\"idle.bad\"\r\nanim_idle\t\"idle.bad\" // old\r\n"));
	TEST_EXPECT(editor_test::write_bytes(root + "/idle.bad", test_io::read_file(anim_dir() + "/idle.bad")));
	auto table = std::make_shared<AnimationMapDocument>();
	TEST_EXPECT(table->load(root + "/notes.adm", "notes.adm", AssetKind::AnimationMap, "jo", error));
	if (table->rows().size() != 2) return 1;
	const NodeId reset = table->rows()[0]->id, idle = table->rows()[1]->id;
	const auto tail_row = [&]() -> NodeId {
		const AssetScan scan = scan_project_assets(paths, project);
		AssetGraph graph;
		ValidationCache cache;
		const std::vector<std::shared_ptr<const DocumentBase>> open{ table };
		for (const Diagnostic &d : validate_project({ paths, project, scan, open }, graph, cache))
			if (d.code() == "animation_map.ignored_input" && d.asset == "notes.adm") return d.row_id;
		return NodeId(-1);
	};
	TEST_EXPECT(tail_row() == idle);
	TEST_EXPECT(table->apply(op(EditOperation::Move, {idle, kRow, 0}, 0), error) && table->rows()[1]->id == reset);
	TEST_EXPECT(tail_row() == idle);
	TEST_EXPECT(table->source_address("1").row == idle && table->source_address("0").row == reset);
	TEST_EXPECT(table->apply(op(EditOperation::Remove, {idle, kRow, 0}), error));
	TEST_EXPECT(table->apply(op(EditOperation::Add, {0, kRow, 0}), error) && table->rows().size() == 2);
	TEST_EXPECT(tail_row() == 0 && table->source_address("1").row == 0);
	std::printf("findings follow rows: moved, removed, a row added in its place\n");
	return 0;
}

// S11a: a clip's frame rate and a frame's step edited are changes and revert; a table's
// key edited is a change and reverts; a clip added to a row is Added and changes the row.
int changes_since_save() {
	using Change = Document::RecordChange;
	AnimationDocument clip;
	Diagnostic error;
	TEST_EXPECT(clip.load(anim_dir() + "/walk.bad", "walk.bad", AssetKind::Animation, "jo", error));
	const ClipRow *row = clip.clip();
	TEST_EXPECT(row && !row->events.empty());
	if (!row || row->events.empty()) return 1;
	const NodeAddress clip_row{row->id, node_kind(AnimationKind::Clip), 0};
	const NodeAddress event{row->id, kEvent, row->collections[1][0]};
	TEST_EXPECT(clip.apply(set(clip_row, "fps", int64_t(25)), error) && clip.apply(set(event, "velocity.x", 0.25), error));
	TEST_EXPECT(clip.field_changed(clip_row, "fps") && clip.field_changed(event, "velocity.x"));
	TEST_EXPECT(clip.record_change(clip_row) == Change::Changed && clip.record_change(event) == Change::Changed);
	TEST_EXPECT(clip.apply(clip.revert_edits(clip_row, "fps"), error) && clip.apply(clip.revert_edits(event, "velocity.x"), error));
	TEST_EXPECT(!clip.field_changed(clip_row, "fps") && !clip.field_changed(event, "velocity.x"));
	TEST_EXPECT(clip.record_change(event) == Change::Unchanged && serialized(clip) == test_io::read_file(anim_dir() + "/walk.bad"));

	AnimationMapDocument table;
	TEST_EXPECT(table.load(anim_dir() + "/soldier.adm", "soldier.adm", AssetKind::AnimationMap, "jo", error));
	const NodeAddress first{table.rows()[1]->id, kRow, 0};
	TEST_EXPECT(table.apply(set(first, "key", std::string("anim_crouch_idle")), error) && table.field_changed(first, "key"));
	TEST_EXPECT(table.apply(table.revert_edits(first, "key"), error) && !table.field_changed(first, "key"));
	TEST_EXPECT(table.record_change(first) == Change::Unchanged);
	TEST_EXPECT(table.apply(op(EditOperation::Add, {first.row, kMapClip, 0}), error));
	TEST_EXPECT(table.record_change({first.row, kMapClip, table.last_added()}) == Change::Added &&
	            table.record_change(first) == Change::Changed);
	std::printf("changes: a clip's fields and a table's key changed and reverted, a clip added\n");
	return 0;
}

int validation_and_graph() {
	editor_test::TempProjectDir dir("opennova_animation_validation_test");
	const std::string root = dir.file("Game");
	ProjectDocument project;
	Diagnostic created;
	TEST_EXPECT(create_project(root, "Anim Game", "jo", project, created));
	const ProjectPaths paths = ProjectPaths::for_root(root);
	TEST_EXPECT(editor_test::write_text(root + "/soldier.adm",
	                                    "anim_reset\t\"idle.bad\"\r\nanim_nosuchslot\t\"idle.bad\"\r\nanim_idle\t\"gone.bad\"\r\n"));
	TEST_EXPECT(editor_test::write_bytes(root + "/idle.bad", test_io::read_file(anim_dir() + "/idle.bad")));
	TEST_EXPECT(editor_test::write_text(root + "/noreset.adm", "anim_idle\t\"idle.bad\"\r\n"));
	// S12: a comment line, a comment after a row, a slot two rows name, a second reset row; a
	// row of ten clips.
	TEST_EXPECT(editor_test::write_text(root + "/twice.adm", "; the rig\r\nanim_reset\t\"idle.bad\"\r\nanim_idle\t\"idle.bad\"\r\n"
	                                                         "ANIM_IDLE\t\"idle.bad\" // again\r\nanim_reset\t\"idle.bad\"\r\n"));
	TEST_EXPECT(editor_test::write_text(root + "/ten.adm", "anim_reset\t\"idle.bad\"\r\nanim_idle a b c d e f g h i j\r\n"));
	// S12: the chain with its second bone hung off its last, which comes after it.
	{
		const std::vector<uint8_t> source = chain_clip();
		bad::BadFile clip{};
		TEST_EXPECT(bad::bad_parse_buffer(source.data(), source.size(), &clip) == 0 && clip.num_bones == 4);
		if (clip.num_bones != 4) return 1;
		clip.bones[1].parent_index = 3;
		std::vector<uint8_t> bytes;
		TEST_EXPECT(bad::bad_write_buffer(&clip, bytes) == 0 && editor_test::write_bytes(root + "/late.bad", bytes));
		bad::bad_free(&clip);
	}
	const AssetScan scan = scan_project_assets(paths, project);
	AssetGraph graph;
	ValidationCache cache;
	const std::vector<std::shared_ptr<const DocumentBase>> open;
	const std::vector<Diagnostic> findings =
			validate_project({ paths, project, scan, open }, graph, cache);
	const auto find = [&](const char *code, const char *asset, size_t nth = 0) -> const Diagnostic * {
		for (const Diagnostic &d : findings)
			if (d.code() == code && d.asset == asset && nth-- == 0) return &d;
		return nullptr;
	};
	const auto has = [&](const char *code, const char *asset) { return find(code, asset) != nullptr; };
	TEST_EXPECT(has("animation_map.key_unknown", "soldier.adm"));
	TEST_EXPECT(has("animation_map.no_reset", "noreset.adm"));
	TEST_EXPECT(has("reference.missing", "soldier.adm")); // gone.bad
	// The lines the table leaves out: on the file (line 1), on the row that keeps the line (line 4).
	const Diagnostic *note = find("animation_map.ignored_input", "twice.adm");
	const Diagnostic *again = find("animation_map.ignored_input", "twice.adm", 1);
	TEST_EXPECT(note && note->line == 1 && note->row_id == 0 && note->severity == DiagnosticSeverity::Warning);
	TEST_EXPECT(again && again->line == 4 && again->row_id != 0 && again->record == "ANIM_IDLE" && again->field == "key");
	TEST_EXPECT(!find("animation_map.ignored_input", "twice.adm", 2));
	// A slot an earlier row names (in any case): one ring; a second reset row: the last reset.
	const Diagnostic *ring = find("animation_map.slot_repeated", "twice.adm");
	const Diagnostic *reset = find("animation_map.slot_repeated", "twice.adm", 1);
	TEST_EXPECT(ring && ring->severity == DiagnosticSeverity::Info && ring->record == "ANIM_IDLE" &&
	            ring->message.find("row 2") != std::string::npos && ring->message.find("one ring") != std::string::npos);
	TEST_EXPECT(reset && reset->record == "anim_reset" && reset->message.find("last reset clip") != std::string::npos);
	TEST_EXPECT(!find("animation_map.slot_repeated", "twice.adm", 2) && !has("animation_map.slot_repeated", "soldier.adm"));
	const Diagnostic *ten = find("animation_map.invalid_input", "ten.adm");
	TEST_EXPECT(ten && ten->severity == DiagnosticSeverity::Error && ten->line == 2 && ten->row_id != 0);
	// A bone whose parent comes after it: on that bone's parent (the rule the runtime's rig is
	// FK-safe by); a retail clip has none.
	const Diagnostic *order = find("animation.parent_order", "late.bad");
	TEST_EXPECT(order && order->severity == DiagnosticSeverity::Warning && order->field == "parent" &&
	            order->record_kind == kBone && order->child_id != 0 && order->message.find("BN04") != std::string::npos);
	TEST_EXPECT(!find("animation.parent_order", "late.bad", 1) && !has("animation.parent_order", "idle.bad"));
	TEST_EXPECT(!bad::bad_parent_in_order(3, 1) && bad::bad_parent_in_order(-1, 0) && bad::bad_parent_in_order(-1, 2) &&
	            bad::bad_parent_in_order(1, 2) && !bad::bad_parent_in_order(-2, 3) && !bad::bad_parent_in_order(0, 0));
	TEST_EXPECT(graph.resolve(ReferenceKind::Animation, "idle.bad") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::Animation, "idle") == ReferenceStatus::Present);
	TEST_EXPECT(graph.resolve(ReferenceKind::Animation, "gone.bad") == ReferenceStatus::Missing);
	std::printf("validation: an unknown slot, a table with no reset row, a missing clip\n");
	return 0;
}

int retail_files() {
	const std::string root = retail::assets();
	if (!retail::dir_exists(root)) return retail::skip_leg("OPENNOVA_JO_ASSETS (the retail clips and tables at its root)");
	size_t clips = 0, tables = 0;
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(root, ec)) {
		if (!entry.is_regular_file(ec)) continue;
		const std::string extension = retail::lower_ascii(entry.path().extension().string());
		const std::string name = entry.path().filename().generic_string();
		Diagnostic error;
		if (extension == ".bad") {
			AnimationDocument document;
			TEST_EXPECT(document.load(entry.path().generic_string(), name, AssetKind::Animation, "jo", error));
			const std::vector<uint8_t> bytes = serialized(document);
			const std::vector<uint8_t> source = test_io::read_file(entry.path().string());
			bad::BadFile a{}, b{};
			TEST_EXPECT(bad::bad_parse_buffer(source.data(), source.size(), &a) == 0);
			TEST_EXPECT(bad::bad_parse_buffer(bytes.data(), bytes.size(), &b) == 0);
			TEST_EXPECT(bad_equal::field_equal(a, b));
			bad::bad_free(&a);
			bad::bad_free(&b);
			++clips;
		} else if (extension == ".adm") {
			AnimationMapDocument document;
			TEST_EXPECT(document.load(entry.path().generic_string(), name, AssetKind::AnimationMap, "jo", error));
			const SerializeResult saved = document.serialize();
			if (!saved.ok()) std::printf("%s: %s\n", name.c_str(), saved.issues.front().message.c_str());
			TEST_EXPECT(saved.ok());
			++tables;
		}
	}
	if (clips + tables == 0) return retail::skip_leg("OPENNOVA_JO_ASSETS with the retail .bad/.adm files at its root");
	std::printf("retail: %zu clips and %zu tables open and save untouched\n", clips, tables);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (clips() != 0) return 1;
	if (bone_parents() != 0) return 1;
	if (tables() != 0) return 1;
	if (maps_by_kind() != 0) return 1;
	if (unused_files_and_letters() != 0) return 1;
	if (table_lines() != 0) return 1;
	if (define_slot() != 0) return 1;
	if (findings_follow_rows() != 0) return 1;
	if (changes_since_save() != 0) return 1;
	if (validation_and_graph() != 0) return 1;
	return retail_files();
}
