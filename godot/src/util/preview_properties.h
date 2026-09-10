#pragma once

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/property_info.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <initializer_list>
#include <utility>
#include <vector>

namespace godot {

// Source-derived preview properties remain inspectable on the authored nodes.
// Their original scene values are restored on unload; decoded native documents
// and per-frame device state never become a second saved copy of game data.
class PreviewProperties {
	std::vector<StringName> names_;
	std::vector<std::pair<StringName, Variant>> saved_;

public:
	void begin(Object *owner, std::initializer_list<const char *> names) {
		if (!names_.empty()) return;
		const Array properties = owner->get_property_list();
		for (const char *name : names) names_.emplace_back(name);
		for (int i = 0; i < properties.size(); ++i) {
			const Dictionary property = properties[i];
			const StringName name = property["name"];
			if ((int(property["usage"]) & PROPERTY_USAGE_STORAGE) == 0) continue;
			for (const StringName &candidate : names_) {
				if (name == candidate) {
					saved_.emplace_back(name, owner->get(name));
					break;
				}
			}
		}
		owner->notify_property_list_changed();
	}

	void end(Object *owner) {
		if (names_.empty()) return;
		for (const auto &property : saved_) owner->set(property.first, property.second);
		saved_.clear();
		names_.clear();
		owner->notify_property_list_changed();
	}

	void validate(PropertyInfo &property) const {
		for (const StringName &name : names_) {
			if (property.name == name) {
				property.usage = (property.usage & ~PROPERTY_USAGE_STORAGE) | PROPERTY_USAGE_READ_ONLY;
				return;
			}
		}
	}
};

} // namespace godot
