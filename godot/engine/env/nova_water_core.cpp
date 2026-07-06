#include "env/nova_water_core.h"

using namespace godot;

void NovaWaterCore::_bind_methods() {
	ClassDB::bind_method(D_METHOD("update", "frame_counter"), &NovaWaterCore::update);
	ClassDB::bind_method(D_METHOD("get_color_rgba8"), &NovaWaterCore::get_color_rgba8);
	ClassDB::bind_method(D_METHOD("get_normal_rgba8"), &NovaWaterCore::get_normal_rgba8);
	ClassDB::bind_method(D_METHOD("get_texture_size"), &NovaWaterCore::get_texture_size);
}

void NovaWaterCore::update(int p_frame_counter) {
	opennova::env::water_noise_color_pixels(color_pixels, tables,
			static_cast<uint32_t>(p_frame_counter));
	opennova::env::water_noise_normal_pixels(normal_pixels, color_pixels);
}

namespace {

// libs/env packs A<<24|R<<16|G<<8|B (the D3D dword order); Godot RGBA8 wants
// R,G,B,A bytes.
PackedByteArray pixels_to_rgba8(const uint32_t *pixels, int count) {
	PackedByteArray bytes;
	bytes.resize(count * 4);
	uint8_t *write = bytes.ptrw();
	for (int i = 0; i < count; ++i) {
		const uint32_t pixel = pixels[i];
		write[i * 4 + 0] = static_cast<uint8_t>((pixel >> 16) & 0xFF);
		write[i * 4 + 1] = static_cast<uint8_t>((pixel >> 8) & 0xFF);
		write[i * 4 + 2] = static_cast<uint8_t>(pixel & 0xFF);
		write[i * 4 + 3] = static_cast<uint8_t>((pixel >> 24) & 0xFF);
	}
	return bytes;
}

} // namespace

PackedByteArray NovaWaterCore::get_color_rgba8() const {
	return pixels_to_rgba8(color_pixels,
			opennova::env::kWaterNoiseSize * opennova::env::kWaterNoiseSize);
}

PackedByteArray NovaWaterCore::get_normal_rgba8() const {
	return pixels_to_rgba8(normal_pixels,
			opennova::env::kWaterNoiseSize * opennova::env::kWaterNoiseSize);
}

int NovaWaterCore::get_texture_size() const {
	return opennova::env::kWaterNoiseSize;
}
