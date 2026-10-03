#include <editor/ui/model_inspector_view.h>

#include <string>
#include <vector>

#include <imgui.h>

#include <editor/documents/model_collision_words.h>
#include <editor/documents/model_document.h>
#include <editor/documents/model_labels.h>
#include <editor/documents/model_surfaces.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

namespace {

constexpr NodeKind k(ModelKind kind) { return node_kind(kind); }

const ImVec4 kMuted(0.70f, 0.70f, 0.70f, 1.0f);
const ImVec4 kMixed(0.95f, 0.75f, 0.35f, 1.0f);

// A label column as the generic form draws one, then the control on the rest of the line.
void label(const char *text, float column) {
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(text);
	ImGui::SameLine(column);
}

void raise(Workspace &workspace, const Document &document, bool ok, std::vector<Edit> edits) {
	if (ok && !edits.empty()) window_requests::edits(workspace, document, std::move(edits));
}

// The surface of a surface picker's item: its words, the effects row it plays in its tooltip, with what
// else the game does with it.
std::string surface_tip(int64_t surface) {
	const ModelSurfaceWords words = model_surface_words(surface);
	std::string tip = words.name + ": a round that hits it plays its ammo's `" + words.tag + "` effects row.";
	if (!words.note.empty()) tip += "\n" + words.note;
	return tip;
}

// A material's bullet faces: their surface by name and their flags, each set on every face made from it.
void material_faces(Workspace &workspace, const ModelDocument &model, const NodeAddress &record, bool editable) {
	ModelMaterialSurface surface;
	if (!model_material_surface(model, record.child, surface)) return;
	const float em = ImGui::GetFontSize();
	const float column = em * 7.0f;
	ImGui::SeparatorText("Bullet faces");
	if (surface.faces == 0) {
		ImGui::TextDisabled("No bullet face is made from this material.");
		ui_kit::tooltip("Bullets hit the faces of the model's collision LOD, each made from a material's triangle. "
		                "This material draws none of them (or was added here): export the model from Blender to "
		                "give it faces.");
		return;
	}
	const std::string count = std::to_string(surface.faces) + (surface.faces == 1 ? " face" : " faces") +
	                          " made from this material";
	ImGui::TextColored(kMuted, "%s", count.c_str());
	ui_kit::tooltip("Bullets hit the faces of the model's collision LOD, each made from a material's triangle: "
	                "what is set here is set on every one of them, in one undo step. A face is still set alone "
	                "under Collision > Bullet faces.");

	// The surface, picked by name.
	label("Surface", column);
	const std::string shown = model_material_surface_words(surface);
	ImGui::SetNextItemWidth(-1.0f);
	if (surface.mixed()) ImGui::PushStyleColor(ImGuiCol_Text, kMixed);
	const bool open = ImGui::BeginCombo("##surface", shown.c_str());
	if (surface.mixed()) ImGui::PopStyleColor();
	if (!open) ui_kit::tooltip(surface.mixed() ? shown + "\nThe faces made from this material disagree: pick one "
	                                                     "surface to give it to all of them."
	                                           : surface_tip(surface.common()));
	if (open) {
		for (const FieldChoice &choice : model_surface_choices()) {
			const bool selected = !surface.mixed() && surface.common() == choice.value;
			ImGui::PushID(static_cast<int>(choice.value));
			if (ImGui::Selectable(choice.label.c_str(), selected) && editable) {
				std::vector<Edit> edits;
				std::string refusal;
				const bool ok = model_surface_edits(model, record.child, choice.value, edits, refusal);
				raise(workspace, model, ok, std::move(edits));
			}
			ui_kit::tooltip(surface_tip(choice.value));
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	if (surface.mixed()) {
		const std::string common = model_surface_words(surface.common()).name;
		ui_kit::WrapRow row;
		if (ui_kit::tool(row, ("Make all " + common).c_str(), editable,
		                 "Gives the " + std::to_string(surface.differing.size()) + " faces that differ the surface " +
		                         std::to_string(surface.surfaces.front().faces) + " of them carry (one undo step).",
		                 true)) {
			std::vector<Edit> edits;
			std::string refusal;
			const bool ok = model_surface_edits(model, record.child, surface.common(), edits, refusal);
			raise(workspace, model, ok, std::move(edits));
		}
		if (ui_kit::tool(row, "Select the faces that differ", true,
		                 "Selects the " + std::to_string(surface.differing.size()) +
		                         " bullet faces whose surface is not " + common + ", under Collision > Bullet faces.",
		                 true)) {
			const std::vector<NodeAddress> faces = model_differing_faces(model, record.child);
			if (!faces.empty())
				workspace.request(request::select_record(model.path(), faces.front(), SelectMode::Replace, faces));
		}
	}

	// The flags, each on every face or none; a count where they disagree.
	const std::vector<ModelFaceFlag> &flags = model_face_flags();
	for (size_t i = 0; i < flags.size() && i < surface.flags.size(); ++i) {
		const ModelFlagCount &count_of = surface.flags[i];
		bool on = count_of.on == surface.faces;
		const bool mixed = count_of.on != 0 && count_of.on != surface.faces;
		ImGui::PushID(flags[i].token);
		if (ImGui::Checkbox(flags[i].label, &on) && editable) {
			std::vector<Edit> edits;
			std::string refusal;
			const bool ok = model_face_flag_edits(model, record.child, flags[i].bit, on || mixed, edits, refusal);
			raise(workspace, model, ok, std::move(edits));
		}
		ui_kit::tooltip(std::string(flags[i].tip) + (mixed ? "\nOn " + std::to_string(count_of.on) + " of the " +
		                                                             std::to_string(surface.faces) +
		                                                             " faces: a click sets it on all of them."
		                                                   : std::string()));
		if (mixed) {
			ImGui::SameLine();
			ImGui::TextColored(kMixed, "%zu of %zu", count_of.on, surface.faces);
		}
		ImGui::PopID();
	}
}

// A bullet face: the material it was made from, a click away.
void face_material(Workspace &workspace, const ModelDocument &model, const NodeAddress &record) {
	const NodeAddress material = model_face_material(model, record.child);
	ImGui::SeparatorText("Made from");
	if (!material.child) {
		ImGui::TextDisabled("No material's triangle meets this face.");
		ui_kit::tooltip("A face made from a collision mesh of its own (a first-person gun's), which the file does not "
		                "keep: its surface is set here alone.");
		return;
	}
	const std::string words = model_record_label(model, material, nullptr);
	if (ImGui::Selectable((ui_kit::fit(words, ImGui::GetContentRegionAvail().x) + "###material").c_str()))
		window_requests::select(workspace, model, material);
	ui_kit::tooltip(words + "\nIts surface and flags are set on every face made from it: select it to set them "
	                        "all at once.");
}

// Words without their [orig: ...] citations: what the Inspector shows, the cited words in its tooltip.
std::string plain(const std::string &words) {
	std::string out;
	size_t at = 0;
	while (at < words.size()) {
		const size_t open = words.find(" [orig:", at);
		if (open == std::string::npos) {
			out += words.substr(at);
			break;
		}
		out += words.substr(at, open - at);
		const size_t close = words.find(']', open);
		if (close == std::string::npos) break;
		at = close + 1;
	}
	return out;
}

// What a collision record is to the game (documents/model_collision_words.h): the words wrapped, muted,
// their citations in the tooltip, and a second paragraph where the record's own kind says more.
void game_words(const std::string &words, const std::string &more = std::string()) {
	ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
	ImGui::TextWrapped("%s", plain(words).c_str());
	ImGui::PopStyleColor();
	ui_kit::tooltip(words);
	if (more.empty()) return;
	ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
	ImGui::TextWrapped("%s", plain(more).c_str());
	ImGui::PopStyleColor();
	ui_kit::tooltip(more);
}

// A collision record's words: the section (a person's hit sphere), the volume by its type, the bullet face,
// the occlusion record by its type; false for any other record.
bool collision_words(const ModelDocument &model, const NodeAddress &record) {
	const CollisionRow *collision = model.collision_row();
	const ModelRow *row = model.model_row();
	Document::Placement at;
	if (!collision || !row || !model.placement(record, at)) return false;
	const size_t i = at.index;
	ImGui::SeparatorText("In the game");
	if (record.kind == k(ModelKind::Section) && i < collision->sections.size()) {
		const threedi::ThreediCollisionObject &s = collision->sections[i];
		const bool person =
				row->header.mesh_type == threedi::THREEDI_MESH_SKINNED && s.num_faces == 0 && s.num_bounding_volumes == 0;
		game_words(person ? kModelHitSphereWords : kModelSectionWords);
		if (s.unk0 & 2) ImGui::TextColored(kMuted, "A blast breaks this section off.");
		return true;
	}
	if (record.kind == k(ModelKind::Volume) && i < collision->volumes.size()) {
		const ModelVolumeType &type = model_volume_type(collision->volumes[i].collidable_type);
		game_words(std::string(type.code) + ", " + type.words + ": " + type.what, kModelVolumeWords);
		return true;
	}
	if (record.kind == k(ModelKind::Face)) {
		const bool person = row->header.mesh_type == threedi::THREEDI_MESH_SKINNED;
		game_words(kModelBulletFaceWords,
		           person ? "On a person a round meets the hit spheres instead: these faces serve the knife, the "
		                    "laser and the other rays [orig: Physics_RaycastAgainstBoneSections @ 0x4e4670]."
		                  : std::string());
		return true;
	}
	if (record.kind == k(ModelKind::Occlusion) && i < collision->occlusion.size()) {
		const int64_t type = collision->occlusion[i].type;
		game_words(std::string(model_occlusion_type_words(type)) + ": " + model_occlusion_type_what(type),
		           kModelOcclusionWords);
		return true;
	}
	return false;
}

} // namespace

bool draw_model_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
                          InspectorTaken &taken) {
	(void)taken;
	const auto *model = dynamic_cast<const ModelDocument *>(&document);
	if (!model) return false;
	// The collision row: what it holds and what the game tests with it (S17).
	if (record.kind == k(ModelKind::Collision) && !record.child) {
		ImGui::SeparatorText("In the game");
		game_words(kModelCollisionWords, "Show draws it over the picture: the Collision part of its menu.");
		return true;
	}
	if (!record.child) return false;
	if (record.kind == k(ModelKind::Section) || record.kind == k(ModelKind::Volume) ||
	    record.kind == k(ModelKind::Occlusion)) {
		ImGui::PushID("model_collision");
		const bool drawn = collision_words(*model, record);
		ImGui::PopID();
		if (drawn) ImGui::Spacing();
		return drawn;
	}
	const bool editable = workspace.view().allows(EditorRequestKind::EditRecord);
	if (record.kind == k(ModelKind::Material)) {
		ImGui::PushID("model_material");
		ImGui::BeginDisabled(!editable);
		material_faces(workspace, *model, record, editable);
		ImGui::EndDisabled();
		ImGui::PopID();
		ImGui::Spacing();
		return true;
	}
	if (record.kind == k(ModelKind::Face)) {
		ImGui::PushID("model_face");
		face_material(workspace, *model, record);
		collision_words(*model, record);
		ImGui::PopID();
		ImGui::Spacing();
		return true;
	}
	if (record.kind == k(ModelKind::UserPoint)) {
		Value name;
		if (!model->get(record, "name", name) || !std::holds_alternative<std::string>(name)) return false;
		const std::string role = model_user_point_role(std::get<std::string>(name));
		if (role.empty()) {
			ImGui::TextColored(kMuted, "No name the game looks up itself.");
			ui_kit::tooltip("An item may still name it (its particle effects, an attached weapon): see Referenced by "
			                "below.");
		} else {
			ImGui::TextColored(kMuted, "The game reads it as the %s.", role.c_str());
			ui_kit::tooltip("By its name: the game finds its seats, flare launch points, weapon muzzles, aim and "
			                "sight origins, a gun's camera and its ground anchor by the names of its user points.");
		}
		return true;
	}
	if (record.kind == k(ModelKind::Lod)) {
		Document::Placement at;
		const ModelRow *row = model->model_row();
		if (!row || !model->placement(record, at)) return false;
		const std::string range = model_lod_range(*row, at.index);
		if (model_lod_drawn(*row, at.index)) ImGui::TextColored(kMuted, "Drawn %s.", range.c_str());
		else ImGui::TextColored(kMixed, "Drawn %s.", range.c_str());
		ui_kit::tooltip("The game draws the first LOD whose threshold the model's projected radius is above, LOD 0 "
		                "first, and stops at the first 0 [Model_SelectRlodLevel].");
		return true;
	}
	return false;
}

} // namespace opennova::editor
