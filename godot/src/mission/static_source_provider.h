#pragma once

#include <runtime/mission/static_sources.h>
#include <godot_cpp/classes/ref.hpp>
#include "object/item_database.h"
#include "object/object_data.h"

namespace godot {

// The native placer facts the effect directors read. The device resolves the
// engine's stable asset ids to retained ObjectData resources at the boundary.
class StaticSourceProvider {
public:
	virtual ~StaticSourceProvider() = default;
	virtual std::vector<opennova::mission::StaticEffectSource> static_item_effect_sources() = 0;
	virtual std::vector<opennova::mission::StaticLightDrawSource> static_light_draw_sources() = 0;
	virtual uint64_t static_light_draw_source_revision() = 0;
	virtual Ref<ItemDatabase> static_source_item_db() = 0;
	virtual Ref<ObjectData> static_source_object_data(uint64_t asset_id) const = 0;
};

} // namespace godot
