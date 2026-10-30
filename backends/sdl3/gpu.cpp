#include "gpu.h"

#include "backend.h"
#include "window.h"

#include <SDL3/SDL.h>

namespace opennova::sdl3 {

struct Gpu::Impl {
	SDL_GPUDevice *device = nullptr;
	SDL_Window *window = nullptr;
	SDL_GPUCommandBuffer *commands = nullptr;
	SDL_GPUTexture *target = nullptr;
};

Gpu::Gpu() : impl_(std::make_unique<Impl>()) {}

Gpu::~Gpu() {
	if (impl_->device == nullptr) return;
	// A frame begun and never presented is submitted (a command buffer that holds a swapchain
	// image cannot be cancelled), and the device drained before the window goes back.
	if (impl_->commands != nullptr) SDL_SubmitGPUCommandBuffer(impl_->commands);
	SDL_WaitForGPUIdle(impl_->device);
	if (impl_->window != nullptr) SDL_ReleaseWindowFromGPUDevice(impl_->device, impl_->window);
	SDL_DestroyGPUDevice(impl_->device);
}

std::unique_ptr<Gpu> Gpu::create(Backend &backend, std::string &error) {
	SDL_Window *window = backend.window().handle();
	if (window == nullptr) {
		error = "the window is not created";
		return nullptr;
	}
	std::unique_ptr<Gpu> gpu(new Gpu());
	// The shader forms the engine's programs come in (ADR 0077 d3: SPIR-V compiled from GLSL 450 at
	// build time, DXIL and MSL from SDL_shadercross): SDL picks the first driver of its order that
	// takes one of them.
	gpu->impl_->device = SDL_CreateGPUDevice(
			SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
			/*debug_mode=*/false, /*name=*/nullptr);
	if (gpu->impl_->device == nullptr) {
		error = std::string("SDL_CreateGPUDevice: ") + SDL_GetError();
		return nullptr;
	}
	if (!SDL_ClaimWindowForGPUDevice(gpu->impl_->device, window)) {
		error = std::string("SDL_ClaimWindowForGPUDevice: ") + SDL_GetError();
		return nullptr;
	}
	gpu->impl_->window = window;
	return gpu;
}

std::string Gpu::driver() const {
	const char *name = SDL_GetGPUDeviceDriver(impl_->device);
	return name != nullptr ? name : "";
}

bool Gpu::begin_frame() {
	if (impl_->commands != nullptr) present(); // the last frame never presented
	impl_->target = nullptr;
	impl_->commands = SDL_AcquireGPUCommandBuffer(impl_->device);
	if (impl_->commands == nullptr) return false;
	if (!SDL_WaitAndAcquireGPUSwapchainTexture(impl_->commands, impl_->window, &impl_->target,
				nullptr, nullptr)) {
		SDL_CancelGPUCommandBuffer(impl_->commands);
		impl_->commands = nullptr;
		return false;
	}
	return true;
}

void Gpu::clear(float r, float g, float b, float a) {
	if (impl_->commands == nullptr || impl_->target == nullptr) return;
	SDL_GPUColorTargetInfo target{};
	target.texture = impl_->target;
	target.clear_color = SDL_FColor{ r, g, b, a };
	target.load_op = SDL_GPU_LOADOP_CLEAR;
	target.store_op = SDL_GPU_STOREOP_STORE;
	SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(impl_->commands, &target, 1, nullptr);
	if (pass != nullptr) SDL_EndGPURenderPass(pass);
}

void Gpu::present() {
	if (impl_->commands == nullptr) return;
	SDL_SubmitGPUCommandBuffer(impl_->commands);
	impl_->commands = nullptr;
	impl_->target = nullptr;
}

} // namespace opennova::sdl3
