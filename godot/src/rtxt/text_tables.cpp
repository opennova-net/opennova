#include "rtxt/text_tables.h"

#include "resource_index/resource_root.h"
#include "util/string_convert.h"

#include <formats/rtxt/text_tables.h>

#include <godot_cpp/core/object.hpp>

#include <string>
#include <unordered_map>

namespace godot {

namespace {

// The bindings handed out per registered name (weak: a binding nobody holds is made again over
// the registry's table), so a registered table reads back as the object registered.
std::unordered_map<std::string, ObjectID> &handles() {
	static std::unordered_map<std::string, ObjectID> ids;
	return ids;
}

// The root whose override table is installed (track_expansion_override), or none.
ObjectID &override_source() {
	static ObjectID id;
	return id;
}

std::string table_name(const String &p_name) { return opennova::to_std(p_name.to_lower()); }

// The engine's texts are the tables' bytes; the shell's strings take the table converter's
// cp1252-aware form (RtxtStringFile's own, the same text a table read gives).
String text(const std::string &p_text) { return RtxtStringFile::text_to_gd(p_text); }
std::string key(const String &p_text) { return RtxtStringFile::text_from_gd(p_text); }

void install_override_from(const ResourceRoot *p_root) {
	const PackedByteArray bytes =
			p_root != nullptr ? p_root->get_expansion_override_table() : PackedByteArray();
	opennova::rtxt::install_text_override_bytes(bytes.ptr(), static_cast<size_t>(bytes.size()));
}

} // namespace

void TextTables::register_table(const String &p_name, const Ref<RtxtStringFile> &p_table) {
	const std::string name = table_name(p_name);
	opennova::rtxt::register_table(name, p_table.is_valid() ? p_table->shared_native() : nullptr);
	if (p_table.is_valid())
		handles()[name] = p_table->get_instance_id();
	else
		handles().erase(name);
	if (name == opennova::rtxt::kTableKeyHelp) {
		if (p_table.is_valid())
			p_table->install_key_strings();
		else
			RtxtStringFile::clear_key_strings();
	}
}

Ref<RtxtStringFile> TextTables::get_table(const String &p_name) {
	const std::string name = table_name(p_name);
	const std::shared_ptr<const opennova::rtxt::File> table = opennova::rtxt::registered_table(name);
	if (table == nullptr) return Ref<RtxtStringFile>();
	const auto it = handles().find(name);
	if (it != handles().end()) {
		RtxtStringFile *held = Object::cast_to<RtxtStringFile>(ObjectDB::get_instance(it->second));
		if (held != nullptr && held->shared_native() == table) return Ref<RtxtStringFile>(held);
	}
	const Ref<RtxtStringFile> binding = RtxtStringFile::over(table);
	handles()[name] = binding->get_instance_id();
	return binding;
}

void TextTables::clear() {
	opennova::rtxt::clear_tables();
	handles().clear();
	override_source() = ObjectID();
	RtxtStringFile::clear_text_override();
	RtxtStringFile::clear_key_strings();
}

Ref<RtxtStringFile> TextTables::load_rtxt(const Ref<ResourceRoot> &p_root, const String &p_file) {
	if (p_root.is_null()) return Ref<RtxtStringFile>();
	const PackedByteArray bytes = p_root->read_file(p_file);
	if (bytes.is_empty()) return Ref<RtxtStringFile>();
	Ref<RtxtStringFile> table;
	table.instantiate();
	if (table->load_from_byte_array(bytes) != OK) return Ref<RtxtStringFile>();
	return table;
}

String TextTables::lookup(const String &p_table, const String &p_section, const String &p_key) {
	return text(opennova::rtxt::table_lookup(table_name(p_table), key(p_section), key(p_key)));
}

String TextTables::lookup_or(const String &p_table, const String &p_section, const String &p_key,
		const String &p_fallback) {
	std::string out;
	return opennova::rtxt::table_text(table_name(p_table), key(p_section), key(p_key), out)
			? text(out)
			: p_fallback;
}

bool TextTables::has_text(const String &p_table, const String &p_section, const String &p_key) {
	std::string out;
	return opennova::rtxt::table_text(table_name(p_table), key(p_section), key(p_key), out);
}

String TextTables::lookup_display(const String &p_table, const String &p_section,
		const String &p_key) {
	return text(opennova::rtxt::table_lookup_display(table_name(p_table), key(p_section),
			key(p_key)));
}

String TextTables::menu_text(const String &p_key, const String &p_fallback) {
	std::string out;
	return opennova::rtxt::find_menu_text(key(p_key), out) ? text(out) : p_fallback;
}

void TextTables::track_expansion_override(const Ref<ResourceRoot> &p_root) {
	override_source() = p_root.is_valid() ? p_root->get_instance_id() : ObjectID();
	install_override_from(p_root.ptr());
}

void TextTables::follow_mount(const ResourceRoot &p_root) {
	if (!override_source().is_valid() ||
			static_cast<uint64_t>(override_source()) != p_root.get_instance_id())
		return;
	install_override_from(&p_root);
}

void TextTables::_bind_methods() {
	ClassDB::bind_static_method("TextTables", D_METHOD("register_table", "name", "table"),
			&TextTables::register_table);
	ClassDB::bind_static_method("TextTables", D_METHOD("get_table", "name"),
			&TextTables::get_table);
	ClassDB::bind_static_method("TextTables", D_METHOD("clear"), &TextTables::clear);
	ClassDB::bind_static_method("TextTables", D_METHOD("load_rtxt", "root", "file"),
			&TextTables::load_rtxt);
	ClassDB::bind_static_method("TextTables", D_METHOD("lookup", "table", "section", "key"),
			&TextTables::lookup);
	ClassDB::bind_static_method("TextTables",
			D_METHOD("lookup_or", "table", "section", "key", "fallback"), &TextTables::lookup_or);
	ClassDB::bind_static_method("TextTables", D_METHOD("has_text", "table", "section", "key"),
			&TextTables::has_text);
	ClassDB::bind_static_method("TextTables",
			D_METHOD("lookup_display", "table", "section", "key"), &TextTables::lookup_display);
	ClassDB::bind_static_method("TextTables", D_METHOD("menu_text", "key", "fallback"),
			&TextTables::menu_text);
	ClassDB::bind_static_method("TextTables", D_METHOD("track_expansion_override", "root"),
			&TextTables::track_expansion_override);
}

} // namespace godot
