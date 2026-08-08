#include "nova_reference_index.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "../resource_index/nova_resource_root.h"
#include "../util/texture_path_resolver.h"

namespace godot {

namespace {

bool has_extension(const String &name) {
	return name.get_file().contains(".");
}

// Candidate filenames to probe for one (kind, name) reference, in priority
// order. Empty = the kind has no file semantics ("unprobed": sound profiles are
// named sets INSIDE .lwf containers, not files).
std::vector<String> candidates_for(const String &target_kind, const String &target_name) {
	std::vector<String> out;
	if (target_name.is_empty()) {
		return out;
	}
	if (target_kind == String("texture") || target_kind == String("image")) {
		// The texture resolver's own fallback list (tga/dds/mdt/pcx...), so the
		// badge agrees with what the renderer would actually load.
		return opennova::texture_candidate_filenames(target_name);
	}
	if (target_kind == String("terrain")) {
		out.push_back(has_extension(target_name) ? target_name : target_name + String(".trn"));
	} else if (target_kind == String("environment")) {
		out.push_back(has_extension(target_name) ? target_name : target_name + String(".env"));
	} else if (target_kind == String("object_model")) {
		out.push_back(has_extension(target_name) ? target_name : target_name + String(".3di"));
	} else if (target_kind == String("font")) {
		out.push_back(has_extension(target_name) ? target_name : target_name + String(".fnt"));
	} else if (target_kind == String("anim_def")) {
		out.push_back(has_extension(target_name) ? target_name : target_name + String(".adm"));
	} else if (target_kind == String("item_defs")) {
		out.push_back(target_name);
	} else if (target_kind == String("menu")) {
		out.push_back(has_extension(target_name) ? target_name : target_name + String(".mnu"));
	} else if (target_kind == String("sound")) {
		// A .lwf bank FILE (mnu <SOUND> elements name the bank; the trigger is a
		// set inside it). Distinct from "sound_profile", which is a named set and
		// stays unprobed.
		out.push_back(has_extension(target_name) ? target_name : target_name + String(".lwf"));
	} else if (target_kind == String("strings")) {
		// text_rsrc values carry their extension in retail (menutxt.BIN); probe
		// verbatim first, with a .bin fallback for extensionless authoring.
		out.push_back(target_name);
		if (!has_extension(target_name)) {
			out.push_back(target_name + String(".bin"));
		}
	} else if (target_kind == String("datasource")) {
		// Runtime resolves datasource names verbatim through the root.
		out.push_back(target_name);
	}
	// "string_id" deliberately has no candidates: a key resolves against a
	// string TABLE, which a flat (kind, name) probe cannot know - callers with
	// table context (the editor's string widgets) status keys themselves.
	return out;
}

} // namespace

void NovaReferenceIndex::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_resource_root", "root"), &NovaReferenceIndex::set_resource_root);
	ClassDB::bind_method(D_METHOD("references_of", "path"), &NovaReferenceIndex::references_of);
	ClassDB::bind_method(D_METHOD("referrers_of", "target_name"), &NovaReferenceIndex::referrers_of);
	ClassDB::bind_method(D_METHOD("resolve", "target_kind", "target_name"), &NovaReferenceIndex::resolve);
	ClassDB::bind_method(D_METHOD("is_built"), &NovaReferenceIndex::is_built);
	ClassDB::bind_method(D_METHOD("get_build_stats"), &NovaReferenceIndex::get_build_stats);
}

void NovaReferenceIndex::set_resource_root(const Ref<NovaResourceRoot> &root) {
	root_ = root;
	built_ = false;
	built_epoch_ = -1;
}

void NovaReferenceIndex::check_epoch() {
	const int64_t epoch = NovaResourceRoot::cache_epoch();
	if (epoch != built_epoch_) {
		built_ = false;
	}
}

bool NovaReferenceIndex::read_bytes(const String &path, std::vector<uint8_t> &out) const {
	if (root_.is_valid()) {
		// Ref<T>::ptr() is const but read_file is const too; resolve through the VFS first.
		const PackedByteArray bytes = root_->read_file(path);
		if (!bytes.is_empty()) {
			out.assign(bytes.ptr(), bytes.ptr() + bytes.size());
			return true;
		}
	}
	// An absolute path the editor opened from disk (outside the mounted root).
	if (FileAccess::file_exists(path)) {
		const PackedByteArray bytes = FileAccess::get_file_as_bytes(path);
		out.assign(bytes.ptr(), bytes.ptr() + bytes.size());
		return true;
	}
	return false;
}

void NovaReferenceIndex::ensure_graph() {
	check_epoch();
	if (built_ || root_.is_null()) {
		return;
	}
	std::vector<opennova::refs::GraphFileInfo> listing;
	for (const opennova::ResourceFileEntry &entry : root_->native_index().resource_files("all")) {
		opennova::refs::GraphFileInfo info;
		info.path = entry.logical_name;
		info.size_bytes = entry.size_bytes;
		info.modified_time = static_cast<uint64_t>(entry.modified_time);
		listing.push_back(info);
	}
	// items.def carries no resource-index kind so the scan skips it; the item
	// table is the hub of the mission->item->model chain, so probe it explicitly.
	// Zero stamps mean it re-extracts every build (one small text file).
	if (root_->has_file("items.def")) {
		opennova::refs::GraphFileInfo items;
		items.path = "items.def";
		listing.push_back(items);
	}
	stats_ = graph_.build(listing, [this](const std::string &path, std::vector<uint8_t> &out) {
		return read_bytes(String(path.c_str()), out);
	});
	built_ = true;
	built_epoch_ = NovaResourceRoot::cache_epoch();
}

Dictionary NovaReferenceIndex::edge_to_dictionary(const opennova::refs::Reference &edge) {
	Dictionary out;
	out["source_path"] = String(edge.source_path.c_str());
	out["source_kind"] = String(edge.source_kind.c_str());
	out["target_name"] = String(edge.target_name.c_str());
	out["target_kind"] = String(edge.target_kind.c_str());
	out["site"] = String(edge.site.c_str());
	const Dictionary resolution = resolve(String(edge.target_kind.c_str()), String(edge.target_name.c_str()));
	out["status"] = resolution["status"];
	out["target_path"] = resolution["path"];
	return out;
}

Array NovaReferenceIndex::references_of(const String &path) {
	Array out;
	std::vector<uint8_t> bytes;
	if (!read_bytes(path, bytes)) {
		return out;
	}
	std::vector<opennova::refs::Reference> edges;
	std::string error;
	const std::string source(path.utf8().get_data());
	if (!opennova::refs::extract(source, bytes.data(), bytes.size(), edges, error)) {
		return out;
	}
	for (const opennova::refs::Reference &edge : edges) {
		out.push_back(edge_to_dictionary(edge));
	}
	return out;
}

Array NovaReferenceIndex::referrers_of(const String &target_name) {
	Array out;
	ensure_graph();
	if (!built_) {
		return out;
	}
	const std::string name(target_name.utf8().get_data());
	for (const opennova::refs::Reference &edge : graph_.referrers_of(name)) {
		out.push_back(edge_to_dictionary(edge));
	}
	return out;
}

Dictionary NovaReferenceIndex::resolve(const String &target_kind, const String &target_name) {
	Dictionary out;
	out["status"] = "unprobed";
	out["path"] = String();
	if (root_.is_null() || target_name.is_empty()) {
		return out;
	}
	const std::vector<String> candidates = candidates_for(target_kind, target_name);
	if (candidates.empty()) {
		return out;
	}
	for (const String &candidate : candidates) {
		const String resolved = root_->resolve_file(candidate);
		if (!resolved.is_empty()) {
			out["status"] = "found";
			out["path"] = resolved;
			return out;
		}
	}
	out["status"] = "missing";
	return out;
}

bool NovaReferenceIndex::is_built() {
	check_epoch();
	return built_;
}

Dictionary NovaReferenceIndex::get_build_stats() {
	ensure_graph();
	Dictionary out;
	out["files"] = static_cast<int64_t>(stats_.files);
	out["recognized"] = static_cast<int64_t>(stats_.recognized);
	out["extracted"] = static_cast<int64_t>(stats_.extracted);
	out["memo_hits"] = static_cast<int64_t>(stats_.memo_hits);
	out["failed"] = static_cast<int64_t>(stats_.failed);
	return out;
}

} // namespace godot
