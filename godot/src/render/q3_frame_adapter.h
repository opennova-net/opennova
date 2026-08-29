#pragma once

#include <cstddef>
#include <memory>

#include <godot_cpp/classes/ref.hpp>
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

	void compile_frame(Node *p_scope, Viewport *p_viewport, Camera3D *p_camera);
	void clear_frame();
	bool has_commands() const;

	bool draw_view(RenderingDevice *p_rd, RenderData *p_render_data,
			std::uint32_t p_view, const RID &p_framebuffer,
			const RID &p_beauty_snapshot, const Vector2i &p_size,
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
