#include "object/model_inspection_records.h"

using namespace godot;

namespace {

template <typename T>
constexpr Variant::Type variant_type_of();
template <>
constexpr Variant::Type variant_type_of<bool>() { return Variant::BOOL; }
template <>
constexpr Variant::Type variant_type_of<int>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<int64_t>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<float>() { return Variant::FLOAT; }
template <>
constexpr Variant::Type variant_type_of<String>() { return Variant::STRING; }
template <>
constexpr Variant::Type variant_type_of<Color>() { return Variant::COLOR; }

} // namespace

#define INSPECTION_BIND_FIELD(m_type, m_name, m_default)                                           \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &self_type::get_##m_name);                      \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &self_type::set_##m_name);             \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);

void MaterialInfo::_bind_methods() { MATERIAL_INFO_FIELDS(INSPECTION_BIND_FIELD) }
void PartAnimTrack::_bind_methods() { PART_ANIM_TRACK_FIELDS(INSPECTION_BIND_FIELD) }
void RenderLodInfo::_bind_methods() { RENDER_LOD_INFO_FIELDS(INSPECTION_BIND_FIELD) }
void BodyBlendState::_bind_methods() { BODY_BLEND_STATE_FIELDS(INSPECTION_BIND_FIELD) }
void WeaponChannelState::_bind_methods() { WEAPON_CHANNEL_STATE_FIELDS(INSPECTION_BIND_FIELD) }
void PartAnimChannelState::_bind_methods() { PART_ANIM_CHANNEL_STATE_FIELDS(INSPECTION_BIND_FIELD) }

Ref<PartAnimTrack> PartAnimInfo::get_track(const String &p_name) const {
#define PART_ANIM_INFO_TRACK_LOOKUP(m_name) \
	if (p_name == #m_name) return m_name##_;
	PART_ANIM_INFO_TRACKS(PART_ANIM_INFO_TRACK_LOOKUP)
#undef PART_ANIM_INFO_TRACK_LOOKUP
	return Ref<PartAnimTrack>();
}

void PartAnimInfo::_bind_methods() {
	PART_ANIM_INFO_FIELDS(INSPECTION_BIND_FIELD)
#define PART_ANIM_INFO_TRACK_BIND(m_name)                                                          \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &PartAnimInfo::get_##m_name);                   \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &PartAnimInfo::set_##m_name);          \
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, #m_name, PROPERTY_HINT_NONE, "",                    \
						 PROPERTY_USAGE_DEFAULT, "PartAnimTrack"),                                 \
			"set_" #m_name, "get_" #m_name);
	PART_ANIM_INFO_TRACKS(PART_ANIM_INFO_TRACK_BIND)
#undef PART_ANIM_INFO_TRACK_BIND
	ClassDB::bind_method(D_METHOD("get_track", "name"), &PartAnimInfo::get_track);
}

#undef INSPECTION_BIND_FIELD
