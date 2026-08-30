#pragma once

#include <cstddef>
#include <memory>

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <runtime/renderer/q3_frame.h>

namespace godot {

class Camera3D;
class GeometryInstance3D;
class Material;
class Node;
class RenderData;
class RenderingDevice;
class Viewport;

// Device adapter for the portable Q3 compiler. Producer registration and
// scene extraction run on the main thread; render callbacks consume only an
// immutable packed frame and RenderingDevice/server RIDs.
class Q3FrameAdapter {
public:
	Q3FrameAdapter();
	~Q3FrameAdapter();

	static void register_object_material(const Ref<Material> &p_material,
			const opennova::renderer::ObjectMaterialClassification &p_classification);
	static void clone_object_material(const Ref<Material> &p_source,
			const Ref<Material> &p_clone);
	static void register_object_source(GeometryInstance3D *p_source,
			const Ref<Material> &p_material);
	static void register_source(GeometryInstance3D *p_source,
			opennova::renderer::Q3Source p_kind);
	// A producer that already holds a surface's CPU arrays (the water strip
	// rebuilt from WaterCore every frame) hands them over here, so the cache
	// re-packs from memory and never reads that surface back through the
	// server. `p_arrays` is the Mesh::ARRAY_MAX layout the producer uploaded.
	static void publish_geometry(GeometryInstance3D *p_source, int p_surface,
			const Array &p_arrays);
	// A producer that rebuilt a registered mesh bumps the source's geometry
	// generation: the cache re-reads and re-packs that source's surfaces once
	// at its next sight. A producer that only rewrote a MultiMesh's instance
	// transforms (a static RLOD switch, a destruction carve) bumps the
	// instance generation instead: only the cached rows are re-read, the
	// packed surfaces and their device buffers stay.
	static void invalidate_source(GeometryInstance3D *p_source);
	static void invalidate_instances(GeometryInstance3D *p_source);

	void compile_frame(Node *p_scope, Viewport *p_viewport, Camera3D *p_camera);
	void clear_frame();
	bool has_commands() const;

	// Draw the published frame into p_framebuffer (the Q3 colour attachment
	// sharing resolved beauty depth). Every technique shades from its leased
	// textures and snapshotted values; nothing samples the beauty colour.
	bool draw_view(RenderingDevice *p_rd, RenderData *p_render_data,
			std::uint32_t p_view, const RID &p_framebuffer,
			std::size_t &r_draw_calls);
	// Free resources only through the caller's known-live device. A null device
	// discards cached handles without dereferencing the adapter's old raw pointer.
	void release_device(RenderingDevice *p_rd);
	Dictionary get_report() const;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace godot
