#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <runtime/renderer/q3_frame.h>

namespace godot {

class MultiMesh;

// The focused Q3 vertex layout: one 104-byte interleaved vertex per drawn
// element (indices are resolved at pack time, so every stream is a plain
// triangle list the adapter draws without an index buffer).
inline constexpr std::uint32_t kQ3VertexStride = 104u;

// Bind-space surface arrays: what a producer published, or what the cache
// read once through the server at first sight. Packed arrays are
// copy-on-write, so holding them beside the producer's own copy costs nothing.
struct Q3SurfaceArrays {
	PackedVector3Array positions;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedVector2Array uv2s;
	PackedColorArray colors;
	PackedFloat32Array custom0;
	PackedFloat32Array custom1;
	PackedFloat32Array custom2;
	PackedInt32Array bones;
	PackedFloat32Array weights;
	PackedInt32Array indices;

	static Q3SurfaceArrays from_mesh_arrays(const Array &p_arrays);
	bool empty() const { return positions.is_empty(); }
	// Drawn elements: the index count, or the vertex count for a raw list.
	std::size_t element_count() const;
};

// The per-frame inputs baked into a packed stream. Equal parameters share
// one packing; a change re-packs the entry from its cached arrays.
struct Q3PackParameters {
	opennova::renderer::Q3Source source = opennova::renderer::Q3Source::Object;
	Vector3 uv_u = Vector3(1, 0, 0);
	Vector3 uv_v = Vector3(0, 1, 0);
	Vector4 water_uv = Vector4(1.0f, 0.2f, 0.0f, 0.0f);
	Vector3 camera_position;
	// GPU-skin packing (the slot capture pass): the bind-space positions stay
	// unskinned and the surface's bone indices ride CUSTOM0 (as floats) with
	// the weights in CUSTOM1, so a consumer with a bone-palette buffer skins
	// in its vertex shader and the entry packs once like a rigid one.
	bool skin_channels = false;

	bool operator==(const Q3PackParameters &p_other) const;
};

// One immutable packed stream. The main thread mints a new one on every
// re-pack; the render side uploads each (entry_id, generation) exactly once
// into that entry's own device buffer.
struct Q3PackedStream {
	std::uint64_t entry_id = 0;
	std::uint64_t generation = 0;
	std::uint32_t vertex_count = 0;
	PackedByteArray bytes;
};

// Main-thread geometry cache for the focused Q3 adapter, keyed by (source
// node id, surface index). An entry packs once when first seen and re-packs
// only when its generation moves: the producer published new arrays, the
// source's geometry was invalidated, or the pack parameters (the material's
// UV transform, the water UV state, the skin channels) changed. Nothing here
// reads a mesh back through the server per frame; the one-time first-sight
// read is counted so a stable frame can prove it made none. The cache also
// retains MultiMesh instance rows per source under the source's separate
// instance generation: a static RLOD switch or destruction carve rewrites
// rows only, so it re-reads the rows and never touches the packed surfaces.
//
// Threading: every method runs on the compile (main) thread. Streams are
// shared immutably with the render side; evictions are handed over through
// the published frame and applied once that frame has been consumed.
class Q3GeometryCache {
public:
	struct Key {
		std::uint64_t source_id = 0;
		int surface = 0;

		bool operator<(const Key &p_other) const {
			return source_id != p_other.source_id ? source_id < p_other.source_id :
					surface < p_other.surface;
		}
	};

	struct Request {
		Key key;
		// The source's geometry generation (invalidate_q3_source bumps it when
		// a producer rebuilt the mesh).
		std::uint64_t geometry_generation = 1;
		// A producer publication for this surface and its generation, or null.
		const Q3SurfaceArrays *published = nullptr;
		std::uint64_t published_generation = 0;
		Q3PackParameters pack;
	};

	struct FrameCounters {
		std::size_t readbacks = 0;
		// MultiMesh sources whose instance rows were (re-)read this frame.
		std::size_t instance_row_reads = 0;
		std::size_t repacked_entries = 0;
		std::size_t packed_vertices = 0;
		std::size_t packed_vertex_bytes = 0;
	};

	// Opens the compile of one frame: resets the frame counters and stamps
	// every eviction minted during it.
	void begin_frame(std::uint64_t p_frame_id);
	// The current stream for one source surface, or null when the surface has
	// no drawable arrays. `p_read_arrays` runs only when the entry holds no
	// arrays (its one server readback) and is counted in the frame counters.
	std::shared_ptr<const Q3PackedStream> acquire(const Request &p_request,
			const std::function<Array()> &p_read_arrays);
	// Cached local instance rows of a MultiMesh source, re-read through the
	// server only after an instance invalidation (invalidate_q3_instances) or
	// an instance-count change; the packed surfaces are untouched by either.
	const std::vector<Transform3D> &instance_transforms(
			std::uint64_t p_source_id, std::uint64_t p_instance_generation,
			MultiMesh *p_multimesh);
	// Drops every entry and instance row whose source no longer exists; the
	// entries become pending evictions for the render side.
	void prune(const std::function<bool(std::uint64_t)> &p_source_alive);
	// Drops one source's entries and instance rows (the focused compile
	// names its dead sources from the registry instead of probing ObjectDB).
	void evict_source(std::uint64_t p_source_id);
	// Evictions the render side has not consumed yet: those minted in frames
	// after `p_consumed_frame_id`. Older ones are dropped here.
	std::vector<std::uint64_t> pending_evictions(
			std::uint64_t p_consumed_frame_id);
	// The owner's generation table for the compiler's lease check: every
	// retained entry's id and current packed generation, appended to
	// `r_generations`. A lease minted from an entry the cache has since
	// re-packed or evicted no longer matches this table and is rejected.
	void append_generations(
			std::vector<opennova::renderer::Q3ResourceGeneration>
					&r_generations) const;

	const FrameCounters &frame_counters() const { return counters_; }
	std::size_t entry_count() const { return entries_.size(); }
	std::size_t cached_vertex_bytes() const;

private:
	struct Entry {
		std::uint64_t entry_id = 0;
		std::uint64_t generation = 0;
		std::uint64_t geometry_generation = 0;
		std::uint64_t published_generation = 0;
		bool arrays_read = false;
		Q3SurfaceArrays arrays;
		Q3PackParameters pack;
		std::shared_ptr<const Q3PackedStream> stream;
	};

	struct InstanceRows {
		std::uint64_t instance_generation = 0;
		int instance_count = -1;
		int visible_instance_count = -1;
		std::vector<Transform3D> local_transforms;
	};

	struct Eviction {
		std::uint64_t entry_id = 0;
		std::uint64_t frame_id = 0;
	};

	std::uint64_t frame_id_ = 0;
	std::uint64_t next_entry_id_ = 1;
	std::map<Key, Entry> entries_;
	std::map<std::uint64_t, InstanceRows> instances_;
	std::vector<Eviction> evictions_;
	FrameCounters counters_{};
	std::vector<Transform3D> no_transforms_;
};

} // namespace godot
