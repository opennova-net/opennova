#include "network/novaworld_identity.h"

#include <net/novaworld/lobby_identity.h>

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/translation_server.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include "util/string_convert.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace godot {

using opennova::to_std;

namespace {

struct LocaleCodes {
	std::string language;
	std::string country;
};

LocaleCodes locale_codes(std::string locale) {
	std::replace(locale.begin(), locale.end(), '-', '_');
	if (const std::size_t suffix = locale.find_first_of(".@"); suffix != std::string::npos) {
		locale.resize(suffix);
	}

	LocaleCodes out;
	std::size_t begin = 0;
	for (int part = 0; begin <= locale.size(); ++part) {
		const std::size_t end = locale.find('_', begin);
		const std::string token = locale.substr(begin, end - begin);
		if (part == 0) {
			out.language = token;
		} else if (out.country.empty() && (token.size() == 2 || token.size() == 3)) {
			out.country = token;
		}
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return out;
}

std::string dimensions(const Vector2i &size) {
	return std::to_string(size.x) + "x" + std::to_string(size.y);
}

Vector2i usable_size(Vector2i preferred, Vector2i fallback) {
	if (preferred.x > 0 && preferred.y > 0) return preferred;
	if (fallback.x > 0 && fallback.y > 0) return fallback;
	return Vector2i(1920, 1080);
}

} // namespace

opennova::LobbyIdentityParams collect_lobby_identity_params(uint32_t client_index,
		uint32_t client_key) {
	opennova::LobbyIdentityParams out;
	out.client_index = client_index;
	out.client_key = client_key;

	OS *os = OS::get_singleton();
	// The engine reads the locale trio where the retail APIs exist (Windows); elsewhere the
	// Godot locale and time zone stand in (the bias inverts Godot's local-minus-UTC offset).
	if (!opennova::read_locale_identity(out)) {
		const String locale = os ? os->get_locale() : String();
		const LocaleCodes codes = locale_codes(to_std(locale));
		TranslationServer *translations = TranslationServer::get_singleton();
		if (translations) {
			if (!codes.language.empty()) {
				const std::string name = to_std(translations->get_language_name(opennova::to_gd(codes.language)));
				if (!name.empty()) out.language = name;
			}
			if (!codes.country.empty()) {
				const std::string name = to_std(translations->get_country_name(opennova::to_gd(codes.country)));
				if (!name.empty()) out.country = name;
			}
		}
		if (Time *time = Time::get_singleton()) {
			const Dictionary zone = time->get_time_zone_from_system();
			if (zone.has(String("bias"))) {
				const int64_t utc_offset_minutes = zone[String("bias")];
				out.tz_bias = std::to_string(-utc_offset_minutes);
			}
		}
	}

	RenderingServer *rendering = RenderingServer::get_singleton();
	std::string adapter_name;
	uint64_t video_memory_mib = 0;
	uint64_t max_texture_size = 2048;
	if (rendering) {
		adapter_name = to_std(rendering->get_video_adapter_name());
		if (RenderingDevice *device = rendering->get_rendering_device()) {
			uint64_t video_memory = device->get_device_total_memory();
			if (video_memory == 0) video_memory = device->get_driver_total_memory();
			video_memory_mib = video_memory >> 20u;
			const uint64_t limit = device->limit_get(RenderingDevice::LIMIT_MAX_TEXTURE_SIZE_2D);
			if (limit > 0 && limit < static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
				max_texture_size = limit;
			}
		}
	}
	if (adapter_name.empty() && os) adapter_name = to_std(os->get_model_name());
	if (adapter_name.empty()) adapter_name = "OpenNova";
	std::replace(adapter_name.begin(), adapter_name.end(), '$', '_');

	Vector2i window_size;
	Vector2i screen_size;
	if (DisplayServer *display = DisplayServer::get_singleton()) {
		window_size = display->window_get_size();
		screen_size = display->screen_get_size();
	}
	window_size = usable_size(window_size, screen_size);
	screen_size = usable_size(screen_size, window_size);
	out.nwhwi = adapter_name + "$" + std::to_string(video_memory_mib) + "$"
			+ std::to_string(max_texture_size) + "$" + dimensions(window_size) + "$"
			+ dimensions(screen_size);

	// Retail derives these telemetry-only tokens from stable volume/MAC data.
	// Godot exposes a platform-stable opaque ID instead; hash it before applying
	// the existing fixed-length A-Z encoding so raw machine identifiers never
	// enter the wire or the portable protocol library.
	std::string stable_identity = os ? to_std(os->get_unique_id()) : std::string();
	if (stable_identity.empty() && os) {
		stable_identity = to_std(os->get_processor_name()) + "|" + to_std(os->get_model_name());
	}
	stable_identity += "|" + adapter_name;
	const opennova::LobbyMachineTokens fallback = opennova::fallback_machine_tokens(stable_identity);
	out.nwpssk = fallback.nwpssk;
	out.nwusid = fallback.nwusid;
	opennova::RetailMachineInputs machine;
	if (opennova::read_retail_machine_inputs(machine)) {
		const opennova::LobbyMachineTokens tokens = opennova::make_retail_machine_tokens(machine);
		out.nwpssk = tokens.nwpssk;
		out.nwusid = tokens.nwusid;
	}

	// The supported retail capture and OpenNova base install both advertise 0.
	// Keep the field explicit so an expansion-aware binding can replace it.
	out.my_installed_exp_bits = "0";
	return out;
}

} // namespace godot
