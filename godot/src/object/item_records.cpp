#include "object/item_records.h"

#include <formats/mission/mission.h> // kItemIdOffset

using namespace godot;

#define ITEM_RECORD_READ_ONLY(m_variant, m_name, m_getter)                          \
	ADD_PROPERTY(PropertyInfo(m_variant, m_name, PROPERTY_HINT_NONE, "",            \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),        \
			"", m_getter)

// --- ItemEmplacementAttachment ---------------------------------------------------

void ItemEmplacementAttachment::assign(int p_kind, const String &p_userpoint, int p_item_id,
		int p_stored_slot, int p_angle_count, int p_down, int p_up,
		bool p_designated_g, bool p_designated_c) {
	kind_ = p_kind;
	userpoint_ = p_userpoint;
	item_id_ = p_item_id;
	stored_slot_ = p_stored_slot;
	angle_count_ = p_angle_count;
	down_limit_bam_ = p_down;
	up_limit_bam_ = p_up;
	designated_g_ = p_designated_g;
	designated_c_ = p_designated_c;
}

void ItemEmplacementAttachment::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_kind"), &ItemEmplacementAttachment::get_kind);
	ClassDB::bind_method(D_METHOD("get_userpoint"), &ItemEmplacementAttachment::get_userpoint);
	ClassDB::bind_method(D_METHOD("get_item_id"), &ItemEmplacementAttachment::get_item_id);
	ClassDB::bind_method(D_METHOD("get_stored_slot"), &ItemEmplacementAttachment::get_stored_slot);
	ClassDB::bind_method(D_METHOD("has_explicit_limits"), &ItemEmplacementAttachment::has_explicit_limits);
	ClassDB::bind_method(D_METHOD("get_down_limit_bam"), &ItemEmplacementAttachment::get_down_limit_bam);
	ClassDB::bind_method(D_METHOD("get_up_limit_bam"), &ItemEmplacementAttachment::get_up_limit_bam);
	ClassDB::bind_method(D_METHOD("is_designated_g"), &ItemEmplacementAttachment::is_designated_g);
	ClassDB::bind_method(D_METHOD("is_designated_c"), &ItemEmplacementAttachment::is_designated_c);
	ITEM_RECORD_READ_ONLY(Variant::INT, "kind", "get_kind");
	ITEM_RECORD_READ_ONLY(Variant::STRING, "userpoint", "get_userpoint");
	ITEM_RECORD_READ_ONLY(Variant::INT, "item_id", "get_item_id");
	ITEM_RECORD_READ_ONLY(Variant::INT, "stored_slot", "get_stored_slot");
	ITEM_RECORD_READ_ONLY(Variant::INT, "down_limit_bam", "get_down_limit_bam");
	ITEM_RECORD_READ_ONLY(Variant::INT, "up_limit_bam", "get_up_limit_bam");
}

// --- EnvsMarkerRow ---------------------------------------------------------------

void EnvsMarkerRow::assign(const opennova::audio::EnvsMarker &p_marker) {
	position_ = Vector3(p_marker.x, p_marker.y, p_marker.z);
	bms_id_ = p_marker.bms_id;
	slot_sets_.resize(4);
	for (int slot = 0; slot < 4; ++slot) {
		slot_sets_.set(slot, String(p_marker.slot_sets[static_cast<size_t>(slot)].c_str()));
	}
}

void EnvsMarkerRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_position"), &EnvsMarkerRow::get_position);
	ClassDB::bind_method(D_METHOD("get_bms_id"), &EnvsMarkerRow::get_bms_id);
	ClassDB::bind_method(D_METHOD("get_slot_sets"), &EnvsMarkerRow::get_slot_sets);
	ITEM_RECORD_READ_ONLY(Variant::VECTOR3, "position", "get_position");
	ITEM_RECORD_READ_ONLY(Variant::INT, "bms_id", "get_bms_id");
	ITEM_RECORD_READ_ONLY(Variant::PACKED_STRING_ARRAY, "slot_sets", "get_slot_sets");
}

// --- ItemSeatAttachmentRow ---------------------------------------------------------

int ItemSeatAttachmentRow::get_item_id() const {
	return value_.child_type_id + static_cast<int>(opennova::mission::kItemIdOffset);
}

void ItemSeatAttachmentRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_item_id"), &ItemSeatAttachmentRow::get_item_id);
	ClassDB::bind_method(D_METHOD("get_kind"), &ItemSeatAttachmentRow::get_kind);
	ClassDB::bind_method(D_METHOD("get_stored_slot"), &ItemSeatAttachmentRow::get_stored_slot);
	ClassDB::bind_method(D_METHOD("get_down_limit_bam"), &ItemSeatAttachmentRow::get_down_limit_bam);
	ClassDB::bind_method(D_METHOD("get_up_limit_bam"), &ItemSeatAttachmentRow::get_up_limit_bam);
	ITEM_RECORD_READ_ONLY(Variant::INT, "item_id", "get_item_id");
	ITEM_RECORD_READ_ONLY(Variant::INT, "kind", "get_kind");
	ITEM_RECORD_READ_ONLY(Variant::INT, "stored_slot", "get_stored_slot");
}

// --- ItemSeatCard --------------------------------------------------------------------

void ItemSeatCard::set_identity(int p_item_id, int p_type_id) {
	item_id_ = p_item_id;
	type_id_ = p_type_id;
}

void ItemSeatCard::set_model(const String &p_display_name, const String &p_graphic,
		const String &p_model) {
	display_name_ = p_display_name;
	graphic_ = p_graphic;
	model_ = p_model;
}

void ItemSeatCard::assign_spec(const opennova::mission::ItemSeatSpec &p_spec) {
	seats_.clear();
	for (const opennova::world::Seat &seat : p_spec.seats) {
		Ref<EntityCardSeat> row;
		row.instantiate();
		row->assign(opennova::world::inspect::seat_row(seat, -1));
		seats_.push_back(row);
	}
	emplacement_attachments_.clear();
	for (const opennova::mission::ItemEmplacementAttachmentSpec &attachment :
			p_spec.emplacement_attachments) {
		Ref<ItemSeatAttachmentRow> row;
		row.instantiate();
		row->assign(attachment);
		emplacement_attachments_.push_back(row);
	}
	primary_weapon_ = String(p_spec.primary_weapon.c_str());
	mount_config_valid_ = p_spec.mount_config_valid;
	mount_config_ = p_spec.mount_config;
}

void ItemSeatCard::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_item_id"), &ItemSeatCard::get_item_id);
	ClassDB::bind_method(D_METHOD("get_type_id"), &ItemSeatCard::get_type_id);
	ClassDB::bind_method(D_METHOD("get_display_name"), &ItemSeatCard::get_display_name);
	ClassDB::bind_method(D_METHOD("get_graphic"), &ItemSeatCard::get_graphic);
	ClassDB::bind_method(D_METHOD("get_model"), &ItemSeatCard::get_model);
	ClassDB::bind_method(D_METHOD("get_error"), &ItemSeatCard::get_error);
	ClassDB::bind_method(D_METHOD("get_primary_weapon"), &ItemSeatCard::get_primary_weapon);
	ClassDB::bind_method(D_METHOD("is_mount_config_valid"), &ItemSeatCard::is_mount_config_valid);
	ClassDB::bind_method(D_METHOD("get_mount_config"), &ItemSeatCard::get_mount_config);
	ClassDB::bind_method(D_METHOD("get_seats"), &ItemSeatCard::get_seats);
	ClassDB::bind_method(D_METHOD("get_emplacement_attachments"), &ItemSeatCard::get_emplacement_attachments);
	ITEM_RECORD_READ_ONLY(Variant::INT, "item_id", "get_item_id");
	ITEM_RECORD_READ_ONLY(Variant::INT, "type_id", "get_type_id");
	ITEM_RECORD_READ_ONLY(Variant::STRING, "display_name", "get_display_name");
	ITEM_RECORD_READ_ONLY(Variant::STRING, "graphic", "get_graphic");
	ITEM_RECORD_READ_ONLY(Variant::STRING, "model", "get_model");
	ITEM_RECORD_READ_ONLY(Variant::STRING, "error", "get_error");
	ITEM_RECORD_READ_ONLY(Variant::STRING, "primary_weapon", "get_primary_weapon");
	ITEM_RECORD_READ_ONLY(Variant::INT, "mount_config", "get_mount_config");
}
