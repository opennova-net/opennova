#include "render/q3_geometry_cache.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector2.hpp>

using namespace godot;
using namespace opennova::renderer;

namespace {

// One packed vertex: position, normal, UV, colour, CUSTOM0..2, UV2 as 26
// floats at kQ3VertexStride; the writer stores straight into the stream.
struct Q3VertexWriter {
	float *dst = nullptr;

	void f32(float p_value) { *dst++ = p_value; }
	void vec2(const Vector2 &p_value) { f32(p_value.x); f32(p_value.y); }
	void vec3(const Vector3 &p_value) { f32(p_value.x); f32(p_value.y); f32(p_value.z); }
	void vec4(const Vector4 &p_value) {
		f32(p_value.x); f32(p_value.y); f32(p_value.z); f32(p_value.w);
	}
	void color(const Color &p_value) {
		f32(p_value.r); f32(p_value.g); f32(p_value.b); f32(p_value.a);
	}
};

Vector4 custom_at(const float *p_values, int64_t p_size, int p_index) {
	const int64_t base = static_cast<int64_t>(p_index) * 4;
	if (base < 0 || base + 3 >= p_size)
		return Vector4();
	return Vector4(p_values[base], p_values[base + 1],
			p_values[base + 2], p_values[base + 3]);
}

// Packs one surface into the interleaved Q3 stream: position, normal, UV,
// colour, CUSTOM0..2, UV2. Object UVs (UV1 and the detail UV2 alike, the
// wrappers' obj_transform_uv over both) take the material's row-vector UV
// transform, water UVs the witnessed camera-relative world/128 pair;
// positions stay in bind space (a skin-channel consumer skins on the GPU).
// The arrays are read through their raw pointers and the stream is sized
// once: the water strip re-packs every frame, so the pack is a plain
// per-element store, never a per-float resize.
bool pack_surface(const Q3SurfaceArrays &p_arrays,
		const Q3PackParameters &p_pack, PackedByteArray &r_vertices) {
	if (p_arrays.empty())
		return false;
	const int vertex_count = static_cast<int>(p_arrays.element_count());
	if (vertex_count <= 0)
		return false;
	const int64_t position_count = p_arrays.positions.size();
	const Vector3 *positions = p_arrays.positions.ptr();
	const int64_t normal_count = p_arrays.normals.size();
	const Vector3 *normals = p_arrays.normals.ptr();
	const int64_t uv_count = p_arrays.uvs.size();
	const Vector2 *uvs = p_arrays.uvs.ptr();
	const int64_t uv2_count = p_arrays.uv2s.size();
	const Vector2 *uv2s = p_arrays.uv2s.ptr();
	const int64_t color_count = p_arrays.colors.size();
	const Color *colors = p_arrays.colors.ptr();
	const int64_t custom0_count = p_arrays.custom0.size();
	const float *custom0 = p_arrays.custom0.ptr();
	const int64_t custom1_count = p_arrays.custom1.size();
	const float *custom1 = p_arrays.custom1.ptr();
	const int64_t custom2_count = p_arrays.custom2.size();
	const float *custom2 = p_arrays.custom2.ptr();
	const int64_t bone_count = p_arrays.bones.size();
	const int32_t *bones = p_arrays.bones.ptr();
	const int64_t weight_count = p_arrays.weights.size();
	const float *weights = p_arrays.weights.ptr();
	const int64_t index_count = p_arrays.indices.size();
	const int32_t *indices = p_arrays.indices.ptr();
	r_vertices.resize(static_cast<int64_t>(vertex_count) * kQ3VertexStride);
	std::uint8_t *out = r_vertices.ptrw();
	for (int element = 0; element < vertex_count; ++element) {
		const int index = index_count == 0 ? element : indices[element];
		if (index < 0 || index >= position_count)
			return false;
		Q3VertexWriter writer{reinterpret_cast<float *>(
				out + static_cast<std::size_t>(element) * kQ3VertexStride)};
		const Vector3 position = positions[index];
		const Vector3 normal = index < normal_count ? normals[index] : Vector3(0, 1, 0);
		const bool has_skin_rows = bone_count >= static_cast<int64_t>(index + 1) * 4 &&
				weight_count >= static_cast<int64_t>(index + 1) * 4;
		Vector2 uv = index < uv_count ? uvs[index] : Vector2();
		Vector2 uv2 = index < uv2_count ? uv2s[index] : Vector2();
		if (p_pack.source == Q3Source::Object) {
			// The same two rows the wrappers apply to UV and UV2
			// (vertex_standard.gdshaderinc), so a detail sampled over the packed
			// UV2 (the slot capture's _MT coverage) follows a scrolled or
			// scaled material like the beauty pass does.
			const auto transform_uv = [&](const Vector2 &p_uv) {
				return Vector2(p_pack.uv_u.x * p_uv.x + p_pack.uv_u.y * p_uv.y + p_pack.uv_u.z,
						p_pack.uv_v.x * p_uv.x + p_pack.uv_v.y * p_uv.y + p_pack.uv_v.z);
			};
			uv = transform_uv(uv);
			uv2 = transform_uv(uv2);
		}
		writer.vec3(position);
		writer.vec3(normal);
		writer.vec2(uv);
		writer.color(index < color_count ? colors[index] : Color(1, 1, 1, 1));
		Vector4 c0 = custom_at(custom0, custom0_count, index);
		Vector4 c1 = custom_at(custom1, custom1_count, index);
		const Vector4 c2 = custom_at(custom2, custom2_count, index);
		if (p_pack.skin_channels) {
			// Bone indices as floats (exactly representable) and their weights;
			// an unskinned surface carries an identity row (bone 0, weight 1)
			// so one vertex shader serves rigid and skinned surfaces alike.
			if (has_skin_rows) {
				const int row = index * 4;
				c0 = Vector4(float(bones[row]), float(bones[row + 1]),
						float(bones[row + 2]), float(bones[row + 3]));
				c1 = Vector4(weights[row], weights[row + 1],
						weights[row + 2], weights[row + 3]);
			} else {
				c0 = Vector4();
				c1 = Vector4(1.0f, 0.0f, 0.0f, 0.0f);
			}
		}
		writer.vec4(c0);
		writer.vec4(c1);
		writer.vec4(c2);
		writer.vec2(uv2);
	}
	return r_vertices.size() ==
			static_cast<int64_t>(vertex_count) * kQ3VertexStride;
}

} // namespace

Q3SurfaceArrays Q3SurfaceArrays::from_mesh_arrays(const Array &p_arrays) {
	Q3SurfaceArrays result;
	if (p_arrays.size() <= Mesh::ARRAY_INDEX)
		return result;
	result.positions = p_arrays[Mesh::ARRAY_VERTEX];
	result.normals = p_arrays[Mesh::ARRAY_NORMAL];
	result.uvs = p_arrays[Mesh::ARRAY_TEX_UV];
	result.uv2s = p_arrays[Mesh::ARRAY_TEX_UV2];
	result.colors = p_arrays[Mesh::ARRAY_COLOR];
	result.custom0 = p_arrays[Mesh::ARRAY_CUSTOM0];
	result.custom1 = p_arrays[Mesh::ARRAY_CUSTOM1];
	result.custom2 = p_arrays[Mesh::ARRAY_CUSTOM2];
	result.bones = p_arrays[Mesh::ARRAY_BONES];
	result.weights = p_arrays[Mesh::ARRAY_WEIGHTS];
	result.indices = p_arrays[Mesh::ARRAY_INDEX];
	return result;
}

std::size_t Q3SurfaceArrays::element_count() const {
	return static_cast<std::size_t>(indices.is_empty() ? positions.size() :
			indices.size());
}

bool Q3PackParameters::operator==(const Q3PackParameters &p_other) const {
	return source == p_other.source && uv_u == p_other.uv_u &&
			uv_v == p_other.uv_v &&
			skin_channels == p_other.skin_channels;
}

void Q3GeometryCache::begin_frame(std::uint64_t p_frame_id) {
	frame_id_ = p_frame_id;
	counters_ = {};
}

std::shared_ptr<const Q3PackedStream> Q3GeometryCache::acquire(
		const Request &p_request, const std::function<Array(bool &)> &p_read_arrays) {
	Entry &entry = entries_[p_request.key];
	if (entry.entry_id == 0)
		entry.entry_id = next_entry_id_++;
	bool dirty = false;
	if (p_request.geometry_generation != entry.geometry_generation) {
		// A geometry-invalidated source rebuilt its mesh: drop the arrays so
		// they are re-read (once) unless a publication supersedes them. Instance
		// row invalidations never reach here.
		entry.geometry_generation = p_request.geometry_generation;
		entry.arrays = Q3SurfaceArrays();
		entry.arrays_read = false;
		entry.published_generation = 0;
		dirty = true;
	}
	if (p_request.published != nullptr &&
			p_request.published_generation != entry.published_generation) {
		entry.arrays = *p_request.published;
		entry.published_generation = p_request.published_generation;
		entry.arrays_read = true;
		dirty = true;
	} else if (!entry.arrays_read) {
		bool read_back = false;
		entry.arrays = Q3SurfaceArrays::from_mesh_arrays(p_read_arrays(read_back));
		entry.arrays_read = true;
		if (read_back)
			++counters_.readbacks;
		dirty = true;
	}
	if (entry.arrays.empty())
		return nullptr;
	if (!dirty && entry.stream && entry.pack == p_request.pack)
		return entry.stream;
	PackedByteArray bytes;
	if (!pack_surface(entry.arrays, p_request.pack, bytes))
		return nullptr;
	auto stream = std::make_shared<Q3PackedStream>();
	stream->entry_id = entry.entry_id;
	stream->generation = ++entry.generation;
	stream->vertex_count = static_cast<std::uint32_t>(bytes.size() / kQ3VertexStride);
	stream->bytes = bytes;
	entry.stream = stream;
	entry.pack = p_request.pack;
	++counters_.repacked_entries;
	counters_.packed_vertices += stream->vertex_count;
	counters_.packed_vertex_bytes += static_cast<std::size_t>(bytes.size());
	return stream;
}

const std::vector<Transform3D> &Q3GeometryCache::instance_transforms(
		std::uint64_t p_source_id, std::uint64_t p_instance_generation,
		MultiMesh *p_multimesh) {
	if (p_multimesh == nullptr)
		return no_transforms_;
	InstanceRows &rows = instances_[p_source_id];
	const int instance_count = p_multimesh->get_instance_count();
	const int visible = p_multimesh->get_visible_instance_count();
	if (rows.instance_generation == p_instance_generation &&
			rows.instance_count == instance_count &&
			rows.visible_instance_count == visible)
		return rows.local_transforms;
	rows.instance_generation = p_instance_generation;
	++counters_.instance_row_reads;
	rows.instance_count = instance_count;
	rows.visible_instance_count = visible;
	const int count = visible < 0 ? instance_count : std::min(visible, instance_count);
	rows.local_transforms.clear();
	rows.local_transforms.reserve(static_cast<std::size_t>(std::max(count, 0)));
	for (int instance = 0; instance < count; ++instance)
		rows.local_transforms.push_back(p_multimesh->get_instance_transform(instance));
	return rows.local_transforms;
}

void Q3GeometryCache::prune(
		const std::function<bool(std::uint64_t)> &p_source_alive) {
	for (auto it = entries_.begin(); it != entries_.end();) {
		if (p_source_alive(it->first.source_id)) {
			++it;
			continue;
		}
		evictions_.push_back({it->second.entry_id, frame_id_});
		it = entries_.erase(it);
	}
	for (auto it = instances_.begin(); it != instances_.end();) {
		if (p_source_alive(it->first))
			++it;
		else
			it = instances_.erase(it);
	}
}

void Q3GeometryCache::evict_source(std::uint64_t p_source_id) {
	for (auto it = entries_.lower_bound({p_source_id, 0});
			it != entries_.end() && it->first.source_id == p_source_id;) {
		evictions_.push_back({it->second.entry_id, frame_id_});
		it = entries_.erase(it);
	}
	instances_.erase(p_source_id);
}

std::vector<std::uint64_t> Q3GeometryCache::pending_evictions(
		std::uint64_t p_consumed_frame_id) {
	evictions_.erase(std::remove_if(evictions_.begin(), evictions_.end(),
			[p_consumed_frame_id](const Eviction &p_eviction) {
				return p_eviction.frame_id <= p_consumed_frame_id;
			}), evictions_.end());
	std::vector<std::uint64_t> result;
	result.reserve(evictions_.size());
	for (const Eviction &eviction : evictions_)
		result.push_back(eviction.entry_id);
	return result;
}

void Q3GeometryCache::append_generations(
		std::vector<Q3ResourceGeneration> &r_generations) const {
	r_generations.reserve(r_generations.size() + entries_.size());
	for (const auto &entry : entries_) {
		if (entry.second.stream)
			r_generations.push_back({entry.second.entry_id,
					entry.second.generation});
	}
}


std::size_t Q3GeometryCache::cached_vertex_bytes() const {
	std::size_t total = 0;
	for (const auto &entry : entries_) {
		if (entry.second.stream)
			total += static_cast<std::size_t>(entry.second.stream->bytes.size());
	}
	return total;
}
