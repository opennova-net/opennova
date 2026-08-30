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
class Node;
class RenderData;
class RenderingDevice;
class Viewport;

// Device adapter for the portable Q3 compiler. Producers register through
// Q3SourceRegistry (persistent per-source records); the compile walks those
// records on the main thread and touches only what changed; render
// callbacks consume only an immutable packed frame and RenderingDevice/
// server RIDs.
class Q3FrameAdapter {
public:
	Q3FrameAdapter();
	~Q3FrameAdapter();

	void compile_frame(Node *p_scope, Viewport *p_viewport, Camera3D *p_camera);
	void clear_frame();
	bool has_commands() const;

	// Consume the published frame on the render side without drawing it:
	// frees the device buffers of every cache entry it names as evicted and
	// stamps it consumed. The compositor runs this every render, so a frame
	// with no commands (the last glow source pruned, every source out of
	// frustum) still releases what the cache evicted.
	void consume_frame(RenderingDevice *p_rd);
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
