#include <runtime/devtools/entity_properties_window.h>

#include <runtime/devtools/entities_window.h>

#include <formats/def/def.h> // the items.def attrib keyword tables (the parser's own)
#include <formats/mission/mission.h> // kItemIdOffset: wire type id -> items.def id

#include <imgui.h>

#include <utility>

namespace opennova::devtools {

namespace {

constexpr uint16_t kNoHandle = world::EntityHandle::kInvalid;

// The items.def `type` values as the def parser documents them (DefItemDef.type).
const char *item_type_name(int type) {
	switch (type) {
		case 1: return "vehicle";
		case 2: return "decoration";
		case 3: return "person";
		case 4: return "marker";
		case 5: return "building";
		case 6: return "object";
		case 8: return "effect";
		case 0: return "unset";
		default: return "?";
	}
}

int keyword_count(bool second_word) {
	return second_word ? def_item_attrib2_keyword_count() : def_item_attrib_keyword_count();
}

const char *keyword(bool second_word, int index) {
	return second_word ? def_item_attrib2_keyword(index) : def_item_attrib_keyword(index);
}

uint32_t keyword_bit(bool second_word, int index) {
	return second_word ? def_item_attrib2_keyword_bit(index) : def_item_attrib_keyword_bit(index);
}

uint32_t known_mask(bool second_word) {
	uint32_t mask = 0;
	for (int i = 0; i < keyword_count(second_word); ++i) {
		mask |= keyword_bit(second_word, i);
	}
	return mask;
}

}  // namespace

void EntityPropertiesWindow::on_visibility(bool visible) {
	shown_ = visible;
	if (!visible) {
		// Drop the card so a closed window holds nothing; the embedder's
		// needs_entity_detail gate stops the pushes on the same edge.
		detail_ = EntityDetailSnapshot{};
	}
}

bool EntityPropertiesWindow::wants_detail() const {
	return shown_ && entities_.selected_handle() != kNoHandle;
}

bool EntityPropertiesWindow::detail_valid() const {
	// A card for a selection that moved is stale, whatever it says.
	return detail_.card.valid && detail_.card.handle == entities_.selected_handle();
}

void EntityPropertiesWindow::set_detail(EntityDetailSnapshot detail) {
	if (!detail.card.valid) {
		detail_ = EntityDetailSnapshot{};
		return;
	}
	if (detail.card.handle != entities_.selected_handle()) {
		return;
	}
	detail_ = std::move(detail);
	attrib_edit_ = static_cast<uint32_t>(detail_.card.world.item_attrib);
	attrib2_edit_ = static_cast<uint32_t>(detail_.card.world.item_attrib2);
}

void EntityPropertiesWindow::toggle_item_attrib(uint32_t bit) {
	if (!detail_valid()) {
		return;
	}
	attrib_edit_ ^= bit;
	detail_.card.world.item_attrib = static_cast<int64_t>(attrib_edit_);
	DebugRequest request;
	request.kind = DebugRequest::Kind::SetEntityItemAttrib;
	request.target.packed = detail_.card.handle;
	request.attrib = attrib_edit_;
	request.attrib2 = attrib2_edit_;
	entities_.enqueue_request(request);
}

void EntityPropertiesWindow::toggle_item_attrib2(uint32_t bit) {
	if (!detail_valid()) {
		return;
	}
	attrib2_edit_ ^= bit;
	detail_.card.world.item_attrib2 = static_cast<int64_t>(attrib2_edit_);
	DebugRequest request;
	request.kind = DebugRequest::Kind::SetEntityItemAttrib;
	request.target.packed = detail_.card.handle;
	request.attrib = attrib_edit_;
	request.attrib2 = attrib2_edit_;
	entities_.enqueue_request(request);
}

// Seed the action edits from the selected row once per selection, so
// "apply" without a touch is a no-op-shaped write, not a zero.
void EntityPropertiesWindow::seed_edits_from_selection() {
	const world::inspect::EntityRow *row = entities_.selected_row();
	if (row == nullptr || row->wire_handle == seeded_handle_) {
		return;
	}
	seeded_handle_ = row->wire_handle;
	health_edit_ = row->health;
	pos_edit_[0] = row->mission_position.x;
	pos_edit_[1] = row->mission_position.y;
	pos_edit_[2] = row->mission_position.z;
}

void EntityPropertiesWindow::draw_actions() {
	const world::inspect::EntityRow *row = entities_.selected_row();
	if (row == nullptr) {
		return;
	}
	// The edit seams key on the AI brain (editable = brain + live registry
	// slot); a brainless row still offers the local-player teleport below.
	ImGui::BeginDisabled(!row->editable);
	ImGui::SetNextItemWidth(96.0f);
	ImGui::InputInt("##entity_health", &health_edit_);
	ImGui::SameLine();
	if (ImGui::Button("Set health")) {
		DebugRequest request;
		request.kind = DebugRequest::Kind::SetEntityHealth;
		request.target.packed = row->wire_handle;
		request.health = health_edit_;
		entities_.enqueue_request(request);
	}
	ImGui::SetNextItemWidth(240.0f);
	ImGui::InputFloat3("##entity_pos", pos_edit_, "%.1f");
	ImGui::SameLine();
	if (ImGui::Button("Set position")) {
		DebugRequest request;
		request.kind = DebugRequest::Kind::SetEntityPosition;
		request.target.packed = row->wire_handle;
		request.pos[0] = pos_edit_[0];
		request.pos[1] = pos_edit_[1];
		request.pos[2] = pos_edit_[2];
		entities_.enqueue_request(request);
	}
	ImGui::EndDisabled();
	ImGui::SetNextItemWidth(64.0f);
	ImGui::InputFloat("yaw", &yaw_edit_, 0.0f, 0.0f, "%.0f");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(64.0f);
	ImGui::InputFloat("pitch", &pitch_edit_, 0.0f, 0.0f, "%.0f");
	ImGui::SameLine();
	if (ImGui::Button("Teleport player here")) {
		DebugRequest request;
		request.kind = DebugRequest::Kind::TeleportLocalPlayer;
		request.pos[0] = pos_edit_[0];
		request.pos[1] = pos_edit_[1];
		request.pos[2] = pos_edit_[2];
		request.yaw = yaw_edit_;
		request.pitch = pitch_edit_;
		entities_.enqueue_request(request);
	}
}

void EntityPropertiesWindow::draw_attrib_grid(const char *label, bool second_word) {
	const uint32_t word = second_word ? attrib2_edit_ : attrib_edit_;
	ImGui::SeparatorText(label);
	ImGui::PushID(label);
	const int count = keyword_count(second_word);
	if (ImGui::BeginTable("bits", 4, ImGuiTableFlags_SizingStretchSame)) {
		for (int i = 0; i < count; ++i) {
			ImGui::TableNextColumn();
			const uint32_t bit = keyword_bit(second_word, i);
			bool checked = (word & bit) != 0;
			if (ImGui::Checkbox(keyword(second_word, i), &checked)) {
				if (second_word) {
					toggle_item_attrib2(bit);
				} else {
					toggle_item_attrib(bit);
				}
			}
			if (!second_word && bit == DEF_ITEM_ATTRIB_AIDATA && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("The AI-class gate: also changes what the 0x0D wire record carries.\n"
								  "Overrides are per entity, never replicated (joiners keep their own\n"
								  "items.def), and re-stamped by the next items.def sweep\n"
								  "(mission load, net topology sync).");
			}
		}
		ImGui::EndTable();
	}
	const uint32_t other = word & ~known_mask(second_word);
	if (other != 0) {
		ImGui::Text("other bits: 0x%08X", other);
	}
	ImGui::PopID();
}

void EntityPropertiesWindow::draw_card() {
	if (!detail_valid()) {
		ImGui::TextUnformatted("Detail card pending...");
		return;
	}
	const world::inspect::EntityCard &card = detail_.card;
	if (card.has_world) {
		const world::inspect::WorldDetail &w = card.world;
		ImGui::Text("Item: %s  (type %d / def %d, %s)",
				w.item_name.empty() ? "(no def)" : w.item_name.c_str(), w.item_id,
				w.item_id + mission::kItemIdOffset, item_type_name(w.item_type));
		ImGui::Text("Handle %d:%d  ssn %d  bms %d  team %d  %s%s",
				(card.handle >> 12) & 0xF, card.handle & 0xFFF, w.net_id, w.bms_id, w.team,
				w.alive ? "alive" : "dead", w.hidden ? ", hidden" : "");
		ImGui::Text("Health %d / %d  at %.1f %.1f %.1f  yaw %.0f",
				w.health, w.health_max, w.mission_position.x, w.mission_position.y,
				w.mission_position.z,
				static_cast<double>(w.yaw) * (360.0 / 4294967296.0));
	}
	if (card.has_ai) {
		const world::inspect::AiDetail &a = card.ai;
		ImGui::Text("AI #%d: %s  alert %d  waypoint %d/%d  ai-health %d",
				card.ai_index, a.state_name.c_str(), a.alert, a.waypoint_id, a.wp_number,
				a.ai_health);
	}
	ImGui::Text("logic tick %llu", static_cast<unsigned long long>(detail_.logic_tick));
	// The attrib words live on the registry row; a brain without a row has
	// nothing to override.
	ImGui::BeginDisabled(!card.has_world);
	draw_attrib_grid("Item attributes", false);
	draw_attrib_grid("Item attributes 2", true);
	ImGui::EndDisabled();
}

void EntityPropertiesWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	const uint16_t handle = entities_.selected_handle();
	if (handle == kNoHandle) {
		ImGui::TextUnformatted("Select a row in Entities, or pick an entity in the Game view.");
		return;
	}
	const world::inspect::EntityRow *row = entities_.selected_row();
	if (row == nullptr) {
		ImGui::Text("Selected entity %d:%d awaits the next directory push.",
				(handle >> 12) & 0xF, handle & 0xFFF);
		return;
	}
	seed_edits_from_selection();
	ImGui::Text("%s  (ssn %d)", row->name.empty() ? "(unnamed)" : row->name.c_str(), row->net_id);
	draw_actions();
	ImGui::Separator();
	draw_card();
}

}  // namespace opennova::devtools
