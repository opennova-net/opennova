#include "object/item_records.h"

using namespace godot;

#define ITEM_RECORD_READ_ONLY(m_variant, m_name, m_getter)                          \
	ADD_PROPERTY(PropertyInfo(m_variant, m_name, PROPERTY_HINT_NONE, "",            \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),        \
			"", m_getter)

// --- ItemParticleFx ------------------------------------------------------------

void ItemParticleFx::assign(const String &p_effect, const String &p_userpoint,
		const String &p_secondary_effect) {
	effect_ = p_effect;
	userpoint_ = p_userpoint;
	secondary_effect_ = p_secondary_effect;
}

void ItemParticleFx::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_effect"), &ItemParticleFx::get_effect);
	ClassDB::bind_method(D_METHOD("get_userpoint"), &ItemParticleFx::get_userpoint);
	ClassDB::bind_method(D_METHOD("get_secondary_effect"), &ItemParticleFx::get_secondary_effect);
	ITEM_RECORD_READ_ONLY(Variant::STRING, "effect", "get_effect");
	ITEM_RECORD_READ_ONLY(Variant::STRING, "userpoint", "get_userpoint");
	ITEM_RECORD_READ_ONLY(Variant::STRING, "secondary_effect", "get_secondary_effect");
}

// --- ItemEmplacementAttachment ---------------------------------------------------

void ItemEmplacementAttachment::assign(int p_kind, const String &p_userpoint, int p_item_id,
		int p_stored_slot, int p_angle_count, int p_down, int p_up, int p_right, int p_left,
		bool p_designated_g, bool p_designated_c) {
	kind_ = p_kind;
	userpoint_ = p_userpoint;
	item_id_ = p_item_id;
	stored_slot_ = p_stored_slot;
	angle_count_ = p_angle_count;
	down_limit_bam_ = p_down;
	up_limit_bam_ = p_up;
	right_limit_bam_ = p_right;
	left_limit_bam_ = p_left;
	designated_g_ = p_designated_g;
	designated_c_ = p_designated_c;
}

void ItemEmplacementAttachment::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_kind"), &ItemEmplacementAttachment::get_kind);
	ClassDB::bind_method(D_METHOD("get_userpoint"), &ItemEmplacementAttachment::get_userpoint);
	ClassDB::bind_method(D_METHOD("get_item_id"), &ItemEmplacementAttachment::get_item_id);
	ClassDB::bind_method(D_METHOD("get_stored_slot"), &ItemEmplacementAttachment::get_stored_slot);
	ClassDB::bind_method(D_METHOD("get_angle_count"), &ItemEmplacementAttachment::get_angle_count);
	ClassDB::bind_method(D_METHOD("has_explicit_limits"), &ItemEmplacementAttachment::has_explicit_limits);
	ClassDB::bind_method(D_METHOD("get_down_limit_bam"), &ItemEmplacementAttachment::get_down_limit_bam);
	ClassDB::bind_method(D_METHOD("get_up_limit_bam"), &ItemEmplacementAttachment::get_up_limit_bam);
	ClassDB::bind_method(D_METHOD("get_right_limit_bam"), &ItemEmplacementAttachment::get_right_limit_bam);
	ClassDB::bind_method(D_METHOD("get_left_limit_bam"), &ItemEmplacementAttachment::get_left_limit_bam);
	ClassDB::bind_method(D_METHOD("is_designated_g"), &ItemEmplacementAttachment::is_designated_g);
	ClassDB::bind_method(D_METHOD("is_designated_c"), &ItemEmplacementAttachment::is_designated_c);
	ITEM_RECORD_READ_ONLY(Variant::INT, "kind", "get_kind");
	ITEM_RECORD_READ_ONLY(Variant::STRING, "userpoint", "get_userpoint");
	ITEM_RECORD_READ_ONLY(Variant::INT, "item_id", "get_item_id");
	ITEM_RECORD_READ_ONLY(Variant::INT, "stored_slot", "get_stored_slot");
	ITEM_RECORD_READ_ONLY(Variant::INT, "angle_count", "get_angle_count");
	ITEM_RECORD_READ_ONLY(Variant::INT, "down_limit_bam", "get_down_limit_bam");
	ITEM_RECORD_READ_ONLY(Variant::INT, "up_limit_bam", "get_up_limit_bam");
	ITEM_RECORD_READ_ONLY(Variant::INT, "right_limit_bam", "get_right_limit_bam");
	ITEM_RECORD_READ_ONLY(Variant::INT, "left_limit_bam", "get_left_limit_bam");
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
