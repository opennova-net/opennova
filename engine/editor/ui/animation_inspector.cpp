#include "animation_inspector.h"

#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include <base/io/tick_rate.h>
#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/animation_slots.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/animation_uses.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <formats/bad/bad_build.h>
#include <runtime/anim/clip_timeline.h>

namespace opennova::editor {

namespace {

// A sentence of the game's words, muted and wrapped.
void note(const std::string &text) {
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", text.c_str());
	ImGui::PopStyleColor();
}

// A line that goes to a use (a Selectable cut to the room, whole in its tooltip).
void use_line(Workspace &workspace, const GraphEdge *edge, const std::string &line, int id) {
	const SessionView &view = workspace.view();
	ImGui::PushID(id);
	const std::string shown = ui_kit::fit(line, ImGui::GetContentRegionAvail().x);
	if (ImGui::Selectable((shown + "###use").c_str()) && edge && view.project.scan)
		window_requests::go_to(workspace, usage_target(*view.project.scan, *edge));
	ui_kit::tooltip(line + "\nClick to go there.");
	ImGui::PopID();
}

} // namespace

bool draw_animation_map_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                                  InspectorTaken &) {
	const auto *map = dynamic_cast<const AnimationMapDocument *>(&document);
	if (!map) return false;
	const SessionView &view = workspace.view();
	const ClipLoads loads = view.project.scan ? project_clip_loads(*view.project.scan) : ClipLoads();
	// What the game does with the row (or the row of the clip selected).
	const std::vector<std::string> notes = map_row_notes(*map, record, loads);
	for (const std::string &line : notes) note(line);
	// The reset row: the slots the map leaves out, by what the game does with each.
	const Node *row = document.row(record.row);
	if (row && !record.child &&
			animation_key_slot(static_cast<const AnimationMapRow &>(*row).key) == 0) {
		const std::vector<int> left_out = map_unauthored_slots(*map, loads);
		const std::string label = "Slots it leaves out (" + std::to_string(left_out.size()) + ")";
		const bool open = !left_out.empty() &&
		                  ImGui::TreeNode("left_out", "%s",
		                                  ui_kit::fit(label, ImGui::GetContentRegionAvail().x -
		                                                             ImGui::GetTreeNodeToLabelSpacing())
		                                          .c_str());
		if (!left_out.empty())
			ui_kit::tooltip("The slots no row of this map authors: each serves the reset row's first clip, and what the "
			                "game does with it is the slot's own rule.");
		if (open) {
			const struct {
				AnimSlotAbsence absence;
				const char *heading;
			} groups[] = {{AnimSlotAbsence::PlaysReset, "The game plays the reset clip in:"},
			              {AnimSlotAbsence::NotPicked, "The game never picks these without a clip:"},
			              {AnimSlotAbsence::Untraced, "Whether the game picks these without a clip is not traced:"}};
			for (const auto &group : groups) {
				std::string words;
				for (const int slot : left_out)
					if (animation_slot_absence(slot) == group.absence)
						words += (words.empty() ? "" : ", ") + animation_slot_words(slot);
				if (words.empty()) continue;
				ImGui::TextWrapped("%s", group.heading);
				note(words);
			}
			ImGui::TreePop();
		}
	}
	// Who plays the map: the items (on their models) and the weapons naming it.
	if (view.findings.graph && view.project.scan) {
		const std::vector<MapPlayer> players = map_players(*view.findings.graph, *view.project.scan, map->path());
		if (players.empty()) {
			note(map_unused_words(*view.findings.graph, map->path()));
		} else {
			ImGui::Text("Played by (%zu):", players.size());
			int id = 0;
			for (const MapPlayer &player : players) {
				std::string line = player.record + (player.first_person ? " (its first-person view)" : "");
				if (!player.model.empty()) line += " on " + player.model;
				if (!player.enemy_model.empty()) line += " (" + player.enemy_model + " as an enemy)";
				use_line(workspace, player.edge, line + " (" + player.file.substr(player.file.find_last_of('/') + 1) + ")",
				         id++);
			}
		}
	}
	ImGui::Separator();
	return true;
}

bool draw_clip_inspector(Workspace &workspace, const Document &document, const NodeAddress &, InspectorTaken &) {
	const auto *clip_document = dynamic_cast<const AnimationDocument *>(&document);
	const ClipRow *clip = clip_document ? clip_document->clip() : nullptr;
	if (!clip || !clip->base) return false;
	const SessionView &view = workspace.view();
	// Its length as the game plays it: the clip's own clock in game ticks [orig: AnimChannel_InitFromData
	// @ 0x410560, the step @ 0x4105BA; AnimChannel_AdvancePlayback @ 0x40B140].
	const uint32_t frames = clip->base->frame_count;
	const bool loops = (clip->flags & bad::BAD_FLAG_LOOP) != 0;
	const anim::ClipTimeline timeline(clip->fps, frames, loops);
	const int32_t ticks = timeline.length_ticks();
	char length[160];
	std::snprintf(length, sizeof(length), "%u frames at %u a second: %.2f s in the game; %s.", frames, clip->fps,
	              ticks > 0 ? ticks / io::kTickHz : 0.0,
	              loops ? "it loops" : "it plays once and holds its last frame");
	note(length);
	// Its frame events: one a frame and the end pose, whose event never fires (animation_end_pose).
	if (clip->events.size() == size_t(frames) + 1 && frames > 0) {
		std::snprintf(length, sizeof(length),
		              "Its %zu frame events are its %u frames (0 to %u) and the end pose (%u), whose event never fires.",
		              clip->events.size(), frames, frames - 1, frames);
		note(length);
	}
	if (view.findings.graph && view.project.scan) {
		const std::vector<ClipUse> uses = clip_uses(*view.findings.graph, *view.project.scan, clip_document->path());
		if (uses.empty()) {
			note(clip_unused_words(*view.findings.graph, clip_document->path()));
		} else {
			ImGui::Text("Played by these map rows (%zu):", uses.size());
			int id = 0;
			for (const ClipUse &use : uses) use_line(workspace, use.edge, use.map + ": " + use.words, id++);
		}
	}
	ImGui::Separator();
	return true;
}

} // namespace opennova::editor
