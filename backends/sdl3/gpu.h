#pragma once

#include <memory>
#include <string>

// The SDL_GPU device over the backend's window (ADR 0077 d9): Vulkan, Direct3D 12 or Metal,
// whichever SDL finds first, the window claimed for its swapchain. R4 brings the device up and
// clears and presents the swapchain each frame; R5's engine GPU API (runtime/gpu) and its render
// frame encode their passes between begin_frame and present, in clear()'s place.
//
// STAGED, NOT WIRED to the engine: no engine GPU interface exists before R5, so the app shell
// drives this class directly. Its API names no SDL type.
namespace opennova::sdl3 {

class Backend;

class Gpu final {
public:
	// The device created and the backend's window (created already) claimed; null with `error`
	// when no GPU driver comes up (no Vulkan, Direct3D 12 or Metal device) or the window cannot be
	// claimed.
	static std::unique_ptr<Gpu> create(Backend &backend, std::string &error);
	~Gpu(); // the window released and the device destroyed, ahead of the backend
	Gpu(const Gpu &) = delete;
	Gpu &operator=(const Gpu &) = delete;

	// The driver SDL chose ("vulkan", "direct3d12", "metal").
	std::string driver() const;

	// The frame's command buffer acquired and the swapchain's next image waited for. False when
	// the frame cannot be recorded (the command buffer failed); a frame with no image (the window
	// minimized) records nothing and still presents.
	bool begin_frame();
	// The frame's one pass: the swapchain image cleared to the colour (linear 0..1) and stored.
	void clear(float r, float g, float b, float a);
	// The frame's commands submitted; the swapchain image presents with them.
	void present();

private:
	Gpu();
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace opennova::sdl3
