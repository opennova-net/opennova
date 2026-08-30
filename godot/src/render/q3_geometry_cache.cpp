#include "render/q3_geometry_cache.h"

#include <algorithm>
#include <cstring>

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector2.hpp>

using namespace godot;
using namespace opennova::renderer;

namespace {

void append_f32(PackedByteArray &p_bytes, float p_value) {
	const int64_t offset = p_bytes.size();
	p_bytes.resize(offset + 4);
	std::memcpy(p_bytes.ptrw() + offset, &p_value, sizeof(p_value));
}

void append_vec2(PackedByteArray &p_bytes, const Vector2 &p_value) {
	append_f32(p_bytes, p_value.x);
	append_f32(p_bytes, p_value.y);
}

void append_vec3(PackedByteArray &p_bytes, const Vector3 &p_value) {
	append_f32(p_bytes, p_value.x);
	append_f32(p_bytes, p_value.y);
	append_f32(p_bytes, p_value.z);
}

void append_color(PackedByteArray &p_bytes, const Color &p_value) {
	append_f32(p_bytes, p_value.r);
	append_f32(p_bytes, p_value.g);
	append_f32(p_bytes, p_value.b);
	append_f32(p_bytes, p_value.a);
}

Vector4 custom_at(const PackedFloat32Array &p_values, int p_index) {
	const int base = p_index * 4;
	if (base < 0 || base + 3 >= p_values.size())
		return Vector4();
	return Vector4(p_values[base], p_values[base + 1],
			p_values[base + 2], p_values[base + 3]);
}

// Packs one surface into the interleaved Q3 stream: position, normal, UV,
// colour, CUSTOM0..2, UV2. Object UVs (UV1 and the detail UV2 alike, the
// wrappers' obj_transform_uv over both) take the material's row-vector UV
// transform, water UVs the witnessed camera-relative world/128 pair;
// positions stay in bind space (a skin-channel consumer skins on the GPU).
bool pack_surface(const Q3SurfaceArrays &p_arrays,
		const Q3PackParameters &p_pack, PackedByteArray &r_vertices) {
	if (p_arrays.empty())
		return false;
	const int vertex_count = static_cast<int>(p_arrays.element_count());
	if (vertex_count <= 0)
		return false;
	r_vertices.resize(0);
	for (int element = 0; element < vertex_count; ++element) {
		const int index = p_arrays.indices.is_empty() ? element :
				p_arrays.indices[element];
		if (index < 0 || index >= p_arrays.positions.size())
			return false;
		const Vector3 position = p_arrays.positions[index];
		const Vector3 normal = index < p_arrays.normals.size() ?
				p_arrays.normals[index] : Vector3(0, 1, 0);
		const bool has_skin_rows = p_arrays.bones.size() >= (index + 1) * 4 &&
				p_arrays.weights.size() >= (index + 1) * 4;
		Vector2 uv = index < p_arrays.uvs.size() ? p_arrays.uvs[index] : Vector2();
		Vector2 uv2 = index < p_arrays.uv2s.size() ? p_arrays.uv2s[index] : Vector2();
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
		} else if (p_pack.source == Q3Source::Water) {
			const Vector2 relative(position.z - p_pack.camera_position.z,
					position.x - p_pack.camera_position.x);
			uv = relative * (p_pack.water_uv.x / 128.0f) -
					Vector2(p_pack.water_uv.y, p_pack.water_uv.y) +
					Vector2(p_pack.water_uv.z, p_pack.water_uv.w);
		}
		append_vec3(r_vertices, position);
		append_vec3(r_vertices, normal);
		append_vec2(r_vertices, uv);
		append_color(r_vertices, index < p_arrays.colors.size() ?
				p_arrays.colors[index] : Color(1, 1, 1, 1));
		Vector4 c0 = custom_at(p_arrays.custom0, index);
		Vector4 c1 = custom_at(p_arrays.custom1, index);
		const Vector4 c2 = custom_at(p_arrays.custom2, index);
		if (p_pack.skin_channels) {
			// Bone indices as floats (exactly representable) and their weights;
			// an unskinned surface carries an identity row (bone 0, weight 1)
			// so one vertex shader serves rigid and skinned surfaces alike.
			if (has_skin_rows) {
				const int row = index * 4;
				c0 = Vector4(float(p_arrays.bones[row]), float(p_arrays.bones[row + 1]),
						float(p_arrays.bones[row + 2]), float(p_arrays.bones[row + 3]));
				c1 = Vector4(p_arrays.weights[row], p_arrays.weights[row + 1],
						p_arrays.weights[row + 2], p_arrays.weights[row + 3]);
			} else {
				c0 = Vector4();
				c1 = Vector4(1.0f, 0.0f, 0.0f, 0.0f);
			}
		}
		append_f32(r_vertices, c0.x); append_f32(r_vertices, c0.y);
		append_f32(r_vertices, c0.z); append_f32(r_vertices, c0.w);
		append_f32(r_vertices, c1.x); append_f32(r_vertices, c1.y);
		append_f32(r_vertices, c1.z); append_f32(r_vertices, c1.w);
		append_f32(r_vertices, c2.x); append_f32(r_vertices, c2.y);
		append_f32(r_vertices, c2.z); append_f32(r_vertices, c2.w);
		append_vec2(r_vertices, uv2);
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
			uv_v == p_other.uv_v && water_uv == p_other.water_uv &&
			camera_position == p_other.camera_position &&
			skin_channels == p_other.skin_channels;
}

void Q3GeometryCache::begin_frame(std::uint64_t p_frame_id) {
	frame_id_ = p_frame_id;
	counters_ = {};
}

std::shared_ptr<const Q3PackedStream> Q3GeometryCache::acquire(
		const Request &p_request, const std::function<Array()> &p_read_arrays) {
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
		entry.arrays = Q3SurfaceArrays::from_mesh_arrays(p_read_arrays());
		entry.arrays_read = true;
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
