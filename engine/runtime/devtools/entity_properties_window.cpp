#include <runtime/devtools/entity_properties_window.h>

#include <runtime/devtools/debug_control_ids.h>
#include <runtime/devtools/entities_window.h>

#include <formats/def/def.h> // the items.def attrib keyword + type-name tables (the parser's own)
#include <formats/mission/mission.h> // kItemIdOffset: wire type id -> items.def id

#include <imgui.h>

#include <utility>

using namespace opennova::def;

namespace opennova::devtools {

namespace {

constexpr uint16_t kNoHandle = world::EntityHandle::kInvalid;

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
		// Drop the card and the seeds so a closed window holds nothing; the
		// embedder's needs_entity_detail gate stops the pushes on the same edge.
		clear();
	}
}

void EntityPropertiesWindow::clear() {
	detail_ = EntityDetailSnapshot{};
	attrib_edit_ = 0;
	attrib2_edit_ = 0;
	seeded_handle_ = kNoHandle;
}

bool EntityPropertiesWindow::detail_valid() const {
	// A card for a selection that moved is stale, whatever it says.
	return detail_.card.valid && detail_.card.handle == entities_.selected_handle();
}

bool EntityPropertiesWindow::edits_enabled() const {
	return detail_valid() && entities_.authority();
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

// The AIData gate also decides what the 0x0D wire record carries: with a
// wire session live it stays as the def authored it, for a stock client's sake.
bool EntityPropertiesWindow::bit_locked(bool second_word, uint32_t bit) const {
	return !second_word && bit == DEF_ITEM_ATTRIB_AIDATA && entities_.session_live();
}

void EntityPropertiesWindow::toggle_bit(bool second_word, uint32_t bit) {
	if (!edits_enabled() || !detail_.card.has_world || bit_locked(second_word, bit)) {
		return;
	}
	if (second_word) {
		attrib2_edit_ ^= bit;
		detail_.card.world.item_attrib2 = static_cast<int64_t>(attrib2_edit_);
	} else {
		attrib_edit_ ^= bit;
		detail_.card.world.item_attrib = static_cast<int64_t>(attrib_edit_);
	}
	// The handle-keyed row (a brainless row takes overrides too): the table's
	// set_entity_item_attrib takes the wire handle and both full words.
	entities_.enqueue_request({control_id::kSetEntityItemAttrib,
			{ControlArg::integer(detail_.card.handle), ControlArg::integer(attrib_edit_),
					ControlArg::integer(attrib2_edit_)}});
}

void EntityPropertiesWindow::toggle_item_attrib(uint32_t bit) {
	toggle_bit(false, bit);
}

void EntityPropertiesWindow::toggle_item_attrib2(uint32_t bit) {
	toggle_bit(true, bit);
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
	// slot): the table's set_entity_health / set_entity_position rows take
	// the row's ai_index, the same address MCP's game_entities publishes. A
	// brainless row still offers the local-player teleport below.
	ImGui::BeginDisabled(!row->editable);
	ImGui::SetNextItemWidth(96.0f);
	ImGui::InputInt("##entity_health", &health_edit_);
	ImGui::SameLine();
	if (ImGui::Button("Set health")) {
		entities_.enqueue_request({control_id::kSetEntityHealth,
				{ControlArg::integer(row->ai_index), ControlArg::integer(health_edit_)}});
	}
	ImGui::SetNextItemWidth(240.0f);
	ImGui::InputFloat3("##entity_pos", pos_edit_, "%.1f");
	ImGui::SameLine();
	if (ImGui::Button("Set position")) {
		entities_.enqueue_request({control_id::kSetEntityPosition,
				{ControlArg::integer(row->ai_index),
						ControlArg::vector3(pos_edit_[0], pos_edit_[1], pos_edit_[2])}});
	}
	ImGui::EndDisabled();
	ImGui::SetNextItemWidth(64.0f);
	ImGui::InputFloat("yaw", &yaw_edit_, 0.0f, 0.0f, "%.0f");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(64.0f);
	ImGui::InputFloat("pitch", &pitch_edit_, 0.0f, 0.0f, "%.0f");
	ImGui::SameLine();
	if (ImGui::Button("Teleport player here")) {
		entities_.enqueue_request({control_id::kTeleportLocalPlayer,
				{ControlArg::vector3(pos_edit_[0], pos_edit_[1], pos_edit_[2]),
						ControlArg::number(yaw_edit_), ControlArg::number(pitch_edit_)}});
	}
	// Crewing: the selected row is the vehicle (its SSN is the wire net id);
	// the automation rows seat an occupant by SSN or the local player.
	if (row->net_id > 0) {
		ImGui::BeginDisabled(!entities_.authority());
		ImGui::SetNextItemWidth(96.0f);
		ImGui::InputInt("##occupant_ssn", &occupant_ssn_edit_);
		ImGui::SameLine();
		if (ImGui::Button("Crew with SSN") && occupant_ssn_edit_ > 0) request_crew_vehicle(occupant_ssn_edit_, row->net_id);
		ImGui::SameLine();
		if (ImGui::Button("Board as player")) {
			entities_.enqueue_request({control_id::kCrewLocalPlayer, {ControlArg::integer(row->net_id)}});
		}
		ImGui::EndDisabled();
	}
}

void EntityPropertiesWindow::request_crew_vehicle(int32_t occupant_ssn, int32_t vehicle_ssn) {
	entities_.enqueue_request({control_id::kCrewVehicle,
			{ControlArg::integer(occupant_ssn), ControlArg::integer(vehicle_ssn)}});
}

void EntityPropertiesWindow::draw_attrib_grid(const char *label, bool second_word) {
	ImGui::SeparatorText(label);
	ImGui::PushID(label);
	const int count = keyword_count(second_word);
	if (ImGui::BeginTable("bits", 4, ImGuiTableFlags_SizingStretchSame)) {
		for (int i = 0; i < count; ++i) {
			ImGui::TableNextColumn();
			const uint32_t bit = keyword_bit(second_word, i);
			// The live word: a toggle earlier in this pass already moved it.
			const uint32_t word = second_word ? attrib2_edit_ : attrib_edit_;
			bool checked = (word & bit) != 0;
			const bool locked = bit_locked(second_word, bit);
			ImGui::BeginDisabled(locked);
			if (ImGui::Checkbox(keyword(second_word, i), &checked)) {
				toggle_bit(second_word, bit);
			}
			ImGui::EndDisabled();
			if (!second_word && bit == DEF_ITEM_ATTRIB_AIDATA &&
					ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip(locked
						? "Locked while a wire session is live: the AI-class gate also decides\n"
						  "what the 0x0D record carries, and a stock client reads its own def."
						: "The AI-class gate: also changes what the 0x0D wire record carries.\n"
						  "Overrides are per entity, never replicated (joiners keep their own\n"
						  "items.def), and re-stamped by the next items.def sweep\n"
						  "(mission load, net topology sync).");
			}
		}
		ImGui::EndTable();
	}
	const uint32_t word = second_word ? attrib2_edit_ : attrib_edit_;
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
	const world::EntityHandle handle{card.handle};
	if (card.has_world) {
		const world::inspect::WorldDetail &w = card.world;
		ImGui::Text("Item: %s  (type %d / def %d, %s)",
				w.item_name.empty() ? "(no def)" : w.item_name.c_str(), w.item_id,
				w.item_id + mission::kItemIdOffset, def_item_type_name(w.item_type));
		ImGui::Text("Handle %d:%d  ssn %d  bms %d  team %d  %s%s",
				handle.pool(), handle.slot(), w.net_id, w.bms_id, w.team,
				w.alive ? "alive" : "dead", w.hidden ? ", hidden" : "");
		// Entity::yaw is whole mission degrees (inspect.h WorldDetail).
		ImGui::Text("Health %d / %d  at %.1f %.1f %.1f  yaw %d",
				w.health, w.health_max, w.mission_position.x, w.mission_position.y,
				w.mission_position.z, w.yaw);
	}
	if (card.has_ai) {
		const world::inspect::AiDetail &a = card.ai;
		ImGui::Text("AI #%d: %s  alert %d  waypoint %d/%d  ai-health %d  heading %.0f",
				card.ai_index, a.state_name.c_str(), a.alert, a.waypoint_id, a.wp_number,
				a.ai_health, a.yaw_deg);
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
		const world::EntityHandle pending{handle};
		ImGui::Text("Selected entity %d:%d awaits the next directory push.",
				pending.pool(), pending.slot());
		return;
	}
	seed_edits_from_selection();
	ImGui::Text("%s  (ssn %d)", row->name.empty() ? "(unnamed)" : row->name.c_str(), row->net_id);
	const bool authority = entities_.authority();
	if (!authority) {
		ImGui::TextUnformatted("Read-only: this peer is a joiner; the session authority owns entity state.");
	}
	// Every edit sits under the authority gate: the actions and the grids.
	ImGui::BeginDisabled(!authority);
	draw_actions();
	ImGui::Separator();
	draw_card();
	ImGui::EndDisabled();
}

}  // namespace opennova::devtools
