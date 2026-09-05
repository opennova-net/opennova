#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstdint>

#include "mission/static_source_records.h"
#include "object/item_database.h"

namespace godot {

// The placer-backed reads the two effect directors take from the world (ADR
// 0043 slice G10): the static item-effect sources and the static light draw
// sources with their revision, and the placer's item database, each resolved
// lazily (a placer exists only once a mission is placed, and dies with it).
// GameWorld implements it over its placer; the bound Callable `setup` of each
// director stays for the isolated GUT harnesses that lend their own sources.
class StaticSourceProvider {
public:
	virtual ~StaticSourceProvider() = default;
	virtual TypedArray<StaticEffectSource> static_item_effect_sources() = 0;
	virtual TypedArray<StaticLightDrawSource> static_light_draw_sources() = 0;
	virtual uint64_t static_light_draw_source_revision() = 0;
	virtual Ref<ItemDatabase> static_source_item_db() = 0;
};

} // namespace godot
