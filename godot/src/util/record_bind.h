#pragma once

// The one read-only property binding the value-wrapper records share (ADR
// 0043 d10: a record crosses to GDScript as `engine::X value_` plus forwarding
// getters; a property is bound so `row.field` reads work, never a setter).

#include <godot_cpp/core/class_db.hpp>

#include "util/variant_type_of.h"

#define OPENNOVA_RECORD_READ_ONLY(m_class, m_variant, m_name)                             \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &m_class::get_##m_name);                \
	ADD_PROPERTY(PropertyInfo(m_variant, #m_name, PROPERTY_HINT_NONE, "",                  \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),               \
			"", "get_" #m_name);

// The same for a bool read through an `is_` getter (the record keeps its
// property name; the getter name is the class's own).
#define OPENNOVA_RECORD_READ_ONLY_IS(m_class, m_name)                                     \
	ClassDB::bind_method(D_METHOD("is_" #m_name), &m_class::is_##m_name);                  \
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, #m_name, PROPERTY_HINT_NONE, "",              \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),               \
			"", "is_" #m_name);

// A typed-array property of records (PROPERTY_HINT_ARRAY_TYPE names the row class).
#define OPENNOVA_RECORD_READ_ONLY_ROWS(m_class, m_name, m_row_class)                      \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &m_class::get_##m_name);                \
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, #m_name, PROPERTY_HINT_ARRAY_TYPE,            \
						 #m_row_class, PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), \
			"", "get_" #m_name);

// An object-valued property (a nested record).
#define OPENNOVA_RECORD_READ_ONLY_OBJECT(m_class, m_name, m_row_class)                    \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &m_class::get_##m_name);                \
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, #m_name, PROPERTY_HINT_NONE, "",            \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY, #m_row_class), \
			"", "get_" #m_name);

// The read-write binding the field-list records share (a record whose X-macro
// field list declares get_/set_ pairs over its own members): both accessors and
// the property, typed by variant_type_of. Expanded inside the record's own
// _bind_methods (self_type is the GDCLASS typedef); a field list's trailing
// columns (the default) are ignored.
#define OPENNOVA_RECORD_FIELD(m_type, m_name, ...)                                        \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &self_type::get_##m_name);              \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &self_type::set_##m_name);     \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);
