#include "mission/static_source_records.h"

using namespace godot;

#define STATIC_SOURCE_BIND_INT(m_class, m_name, m_default)                                     \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &m_class::get_##m_name);                    \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &m_class::set_##m_name);           \
	ADD_PROPERTY(PropertyInfo(Variant::INT, #m_name), "set_" #m_name, "get_" #m_name);

#define STATIC_SOURCE_BIND_GRAPHIC(m_class)                                                    \
	ClassDB::bind_method(D_METHOD("get_graphic"), &m_class::get_graphic);                      \
	ClassDB::bind_method(D_METHOD("set_graphic", "graphic"), &m_class::set_graphic);           \
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "graphic"), "set_graphic", "get_graphic");

#define STATIC_SOURCE_BIND_OBJECT_DATA(m_class)                                                \
	ClassDB::bind_method(D_METHOD("get_object_data"), &m_class::get_object_data);              \
	ClassDB::bind_method(D_METHOD("set_object_data", "data"), &m_class::set_object_data);      \
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "object_data", PROPERTY_HINT_NONE, "",          \
						 PROPERTY_USAGE_DEFAULT, "ObjectData"),                                \
			"set_object_data", "get_object_data");

#define STATIC_SOURCE_BIND_WORLD_TRANSFORM(m_class)                                            \
	ClassDB::bind_method(D_METHOD("get_world_transform"), &m_class::get_world_transform);      \
	ClassDB::bind_method(D_METHOD("set_world_transform", "transform"),                         \
			&m_class::set_world_transform);                                                    \
	ADD_PROPERTY(PropertyInfo(Variant::TRANSFORM3D, "world_transform"),                        \
			"set_world_transform", "get_world_transform");

#define STATIC_SOURCE_BIND_ACTIVE(m_class)                                                     \
	ClassDB::bind_method(D_METHOD("is_active"), &m_class::is_active);                          \
	ClassDB::bind_method(D_METHOD("set_active", "active"), &m_class::set_active);              \
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "active"), "set_active", "is_active");

void StaticUserPointSource::_bind_methods() {
	STATIC_SOURCE_BIND_GRAPHIC(StaticUserPointSource)
	STATIC_SOURCE_BIND_OBJECT_DATA(StaticUserPointSource)
	ClassDB::bind_method(D_METHOD("get_transforms"), &StaticUserPointSource::get_transforms);
	ClassDB::bind_method(D_METHOD("set_transforms", "transforms"),
			&StaticUserPointSource::set_transforms);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "transforms", PROPERTY_HINT_ARRAY_TYPE,
						 "Transform3D"),
			"set_transforms", "get_transforms");
}

void StaticEffectSource::_bind_methods() {
#define STATIC_EFFECT_SOURCE_BIND(m_name, m_default) \
	STATIC_SOURCE_BIND_INT(StaticEffectSource, m_name, m_default)
	STATIC_EFFECT_SOURCE_INT_FIELDS(STATIC_EFFECT_SOURCE_BIND)
#undef STATIC_EFFECT_SOURCE_BIND
	STATIC_SOURCE_BIND_GRAPHIC(StaticEffectSource)
	STATIC_SOURCE_BIND_WORLD_TRANSFORM(StaticEffectSource)
	STATIC_SOURCE_BIND_OBJECT_DATA(StaticEffectSource)
}

void StaticLightDrawSource::_bind_methods() {
#define STATIC_LIGHT_DRAW_SOURCE_BIND(m_name, m_default) \
	STATIC_SOURCE_BIND_INT(StaticLightDrawSource, m_name, m_default)
	STATIC_LIGHT_DRAW_SOURCE_INT_FIELDS(STATIC_LIGHT_DRAW_SOURCE_BIND)
#undef STATIC_LIGHT_DRAW_SOURCE_BIND
	ClassDB::bind_method(D_METHOD("get_world_bounds"), &StaticLightDrawSource::get_world_bounds);
	ClassDB::bind_method(D_METHOD("set_world_bounds", "bounds"),
			&StaticLightDrawSource::set_world_bounds);
	ADD_PROPERTY(PropertyInfo(Variant::AABB, "world_bounds"), "set_world_bounds",
			"get_world_bounds");
	STATIC_SOURCE_BIND_ACTIVE(StaticLightDrawSource)
}

void StaticTerrainShadowSourceRow::_bind_methods() {
#define STATIC_TERRAIN_SHADOW_SOURCE_BIND(m_name, m_default) \
	STATIC_SOURCE_BIND_INT(StaticTerrainShadowSourceRow, m_name, m_default)
	STATIC_TERRAIN_SHADOW_SOURCE_INT_FIELDS(STATIC_TERRAIN_SHADOW_SOURCE_BIND)
#undef STATIC_TERRAIN_SHADOW_SOURCE_BIND
	ClassDB::bind_method(D_METHOD("get_entity_attrib"),
			&StaticTerrainShadowSourceRow::get_entity_attrib);
	ClassDB::bind_method(D_METHOD("set_entity_attrib", "value"),
			&StaticTerrainShadowSourceRow::set_entity_attrib);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "entity_attrib"), "set_entity_attrib",
			"get_entity_attrib");
	ClassDB::bind_method(D_METHOD("get_item_attrib"),
			&StaticTerrainShadowSourceRow::get_item_attrib);
	ClassDB::bind_method(D_METHOD("set_item_attrib", "value"),
			&StaticTerrainShadowSourceRow::set_item_attrib);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "item_attrib"), "set_item_attrib",
			"get_item_attrib");
	ClassDB::bind_method(D_METHOD("get_item_attrib2"),
			&StaticTerrainShadowSourceRow::get_item_attrib2);
	ClassDB::bind_method(D_METHOD("set_item_attrib2", "value"),
			&StaticTerrainShadowSourceRow::set_item_attrib2);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "item_attrib2"), "set_item_attrib2",
			"get_item_attrib2");
	STATIC_SOURCE_BIND_GRAPHIC(StaticTerrainShadowSourceRow)
	STATIC_SOURCE_BIND_WORLD_TRANSFORM(StaticTerrainShadowSourceRow)
	STATIC_SOURCE_BIND_OBJECT_DATA(StaticTerrainShadowSourceRow)
	STATIC_SOURCE_BIND_ACTIVE(StaticTerrainShadowSourceRow)
}
