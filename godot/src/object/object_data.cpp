// ObjectData: immutable .3di runtime loading plus the retail network-challenge
// model registry fed by mounted model-definition loads. Inspection, geometry,
// and runtime evaluation live in the sibling translation units.
#include "object/object_data_internal.h"
#include "util/string_convert.h"

#include <cstring>
#include <unordered_set>

using namespace novaobj;
using namespace opennova::threedi;

namespace {

// Retail's C2S 0x3D producer is a renderer-side cache of unique loaded .3DI
// definitions, not the live entity pool. The resource epoch prevents names
// from a previous mount/rescan leaking into a later renderer generation.
struct NetworkChallengeModelRegistry {
	int64_t epoch = -1;
	std::unordered_set<std::string> loaded;
	std::unordered_set<std::string> foliage;
};

NetworkChallengeModelRegistry &network_challenge_model_registry() {
	static NetworkChallengeModelRegistry registry;
	const int64_t epoch = ResourceRoot::cache_epoch();
	if (registry.epoch != epoch) {
		registry.epoch = epoch;
		registry.loaded.clear();
		registry.foliage.clear();
	}
	return registry;
}

std::string network_challenge_model_key(const String &name) {
	return opennova::to_std(name.get_file().to_lower());
}

void register_network_challenge_model(const String &name, bool include) {
	const std::string key = network_challenge_model_key(name);
	if (key.empty()) return;
	NetworkChallengeModelRegistry &registry = network_challenge_model_registry();
	registry.loaded.insert(key);
	// Retail's foliage mark is sticky on the shared model-def node.
	if (!include) registry.foliage.insert(key);
}

String filename_stem(const String &path) {
	const String stem = path.get_file().get_basename();
	return stem.is_empty() ? String("untitled") : stem;
}

} // namespace

ObjectData::ObjectData() = default;

ObjectData::~ObjectData() {
	_clear();
}

void ObjectData::_clear() {
	source_model_.reset();
	submesh_cache.clear();
	_invalidate_panm_cache();
	_invalidate_runtime_control_names();
	source_path = String();
	source_dir = String();
	resource_root.unref();
	object_name = "untitled";
	last_error = String();
}

std::atomic<uint64_t> ObjectData::global_change_counter_{0};

void ObjectData::_notify_object_changed() {
	// Loading replaces the entire immutable content snapshot, so every derived
	// view is invalidated together. There are no block-level edit masks.
	submesh_cache.clear();
	_invalidate_panm_cache();
	_invalidate_runtime_control_names();
	++change_revision_;
	if (change_revision_ == 0) ++change_revision_;
	global_change_counter_.fetch_add(1, std::memory_order_relaxed);
	emit_signal("object_changed");
	emit_changed();
}

Error ObjectData::open_file(const String &p_path) {
	if (p_path.get_extension().to_lower() != "3di") {
		last_error = "Only .3di object files are supported";
		return ERR_FILE_UNRECOGNIZED;
	}
	return _open_3di(p_path);
}

Error ObjectData::open_from_resource_root(
		const Ref<ResourceRoot> &p_resource_root, const String &p_name,
		bool p_include_in_network_challenge) {
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty()) {
		last_error = "Resource root is not configured";
		return ERR_INVALID_PARAMETER;
	}
	const String file = p_name.get_file();
	if (file.get_extension().to_lower() != "3di") {
		last_error = "Only mounted .3di object files are supported";
		return ERR_FILE_UNRECOGNIZED;
	}
	const auto model = p_resource_root->native_assets().model(file.utf8().get_data());
	if (!model) {
		last_error = "Object file missing or invalid in resource root: " + file;
		return p_resource_root->has_file(file) ? ERR_FILE_CANT_READ : ERR_FILE_NOT_FOUND;
	}
	_clear();
	source_model_ = model;
	source_path = file.get_file();
	source_dir = p_resource_root->get_root_dir();
	object_name = model->header.name[0] ? from_native(model->header.name) : filename_stem(file);
	resource_root = p_resource_root;
	register_network_challenge_model(file, p_include_in_network_challenge);
	_notify_object_changed();
	return OK;
}

void ObjectData::mark_cached_network_challenge_foliage_model(
		const String &p_name) {
	register_network_challenge_model(p_name, false);
}

void ObjectData::reset_network_challenge_model_registry() {
	NetworkChallengeModelRegistry &registry = network_challenge_model_registry();
	registry.loaded.clear();
	registry.foliage.clear();
}

int64_t ObjectData::network_challenge_model_count() {
	const NetworkChallengeModelRegistry &registry =
			network_challenge_model_registry();
	std::size_t included = 0;
	for (const std::string &name : registry.loaded) {
		if (registry.foliage.find(name) == registry.foliage.end()) ++included;
	}
	return static_cast<int64_t>(included);
}

Error ObjectData::_open_3di(const String &p_path) {
	auto model = opennova::assets::read_model_file(to_native_path(p_path));
	if (!model) {
		last_error = "Failed to read 3DI";
		return ERR_FILE_CANT_READ;
	}
	_clear();
	source_model_ = std::move(model);
	source_path = p_path;
	source_dir = p_path.get_base_dir();
	object_name = source_model_->header.name[0]
			? from_native(source_model_->header.name) : filename_stem(p_path);
	_notify_object_changed();
	return OK;
}
