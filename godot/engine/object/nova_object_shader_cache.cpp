#include "object/nova_object_shader_cache.h"

#include "renderer/material_classify.h"
#include "renderer/object_shader_template.h"

#include <godot_cpp/core/class_db.hpp>

namespace godot {

NovaObjectShaderCache *NovaObjectShaderCache::singleton = nullptr;

NovaObjectShaderCache *NovaObjectShaderCache::get_singleton() {
	if (singleton == nullptr) {
		singleton = memnew(NovaObjectShaderCache);
	}
	return singleton;
}

NovaObjectShaderCache::NovaObjectShaderCache() {
	if (singleton == nullptr) {
		singleton = this;
	}
}

NovaObjectShaderCache::~NovaObjectShaderCache() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

void NovaObjectShaderCache::clear() {
	cache.clear();
}

void NovaObjectShaderCache::_bind_methods() {
	ClassDB::bind_static_method("NovaObjectShaderCache", D_METHOD("get_singleton"), &NovaObjectShaderCache::get_singleton);
	ClassDB::bind_method(D_METHOD("get_shader_for_key", "key"), &NovaObjectShaderCache::get_shader_for_key);
	ClassDB::bind_method(D_METHOD("classify", "shader_tag", "material_flags", "emissive_type", "is_glass_flag", "alpha_test_byte"), &NovaObjectShaderCache::classify);
	ClassDB::bind_method(D_METHOD("family_for_key", "key"), &NovaObjectShaderCache::family_for_key);
	ClassDB::bind_method(D_METHOD("clear"), &NovaObjectShaderCache::clear);
}

Ref<Shader> NovaObjectShaderCache::get_shader_for_key(int32_t key) {
	const uint32_t ukey = static_cast<uint32_t>(key);
	auto it = cache.find(ukey);
	if (it != cache.end()) {
		return it->second;
	}
	Ref<Shader> shader;
	shader.instantiate();
	const std::string code = renderer::compose_object_shader_glsl(ukey);
	shader->set_code(String(code.c_str()));
	cache[ukey] = shader;
	return shader;
}

int32_t NovaObjectShaderCache::classify(const String &shader_tag,
		int32_t material_flags,
		int32_t emissive_type,
		int32_t is_glass_flag,
		int32_t alpha_test_byte) {
	const std::string tag = shader_tag.utf8().get_data();
	const auto cls = renderer::classify_object_material(
			tag,
			static_cast<uint8_t>(material_flags),
			static_cast<uint8_t>(emissive_type),
			static_cast<uint8_t>(is_glass_flag),
			static_cast<uint8_t>(alpha_test_byte));
	return static_cast<int32_t>(renderer::build_object_shader_key(cls));
}

int32_t NovaObjectShaderCache::family_for_key(int32_t key) const {
	return static_cast<int32_t>(renderer::decode_object_shader_family(static_cast<uint32_t>(key)));
}

} // namespace godot
