#include "network/novaworld_identity.h"

#include <formats/gamecfg/game_cfg.h> // gamecfg::display_size
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

// The NWHWI device facts (engine HardwareIdentity, make_nwhwi): on Windows the engine's reads,
// retail's own (read_display_adapter on the cfg's hw3d_deviceno, read_primary_screen_size);
// elsewhere the renderer's adapter name and memory and the display server's primary screen stand
// in. The physical memory is the OS's, and the display mode game.cfg's over that desktop.
opennova::HardwareIdentity collect_hardware_identity(const opennova::gamecfg::GameCfg &cfg) {
	opennova::HardwareIdentity hw;
	if (!opennova::read_display_adapter(static_cast<uint32_t>(cfg.hw3d_deviceno), hw)) {
		if (RenderingServer *rendering = RenderingServer::get_singleton()) {
			hw.adapter_description = to_std(rendering->get_video_adapter_name());
			hw.has_adapter = !hw.adapter_description.empty();
			if (RenderingDevice *device = rendering->get_rendering_device()) {
				uint64_t video_memory = device->get_device_total_memory();
				if (video_memory == 0) video_memory = device->get_driver_total_memory();
				hw.display_memory_mb = static_cast<int64_t>(video_memory >> 20u);
			}
		}
	}
	opennova::read_total_physical_memory(hw.total_phys_bytes);
	if (!opennova::read_primary_screen_size(hw.desktop_width, hw.desktop_height)) {
		if (DisplayServer *display = DisplayServer::get_singleton()) {
			const Vector2i size = display->screen_get_size(DisplayServer::SCREEN_PRIMARY);
			hw.desktop_width = size.x;
			hw.desktop_height = size.y;
		}
	}
	opennova::gamecfg::display_size(cfg, hw.desktop_width, hw.desktop_height, hw.display_width,
			hw.display_height);
	return hw;
}

} // namespace

opennova::LobbyIdentityParams collect_lobby_identity_params(const opennova::gamecfg::GameCfg &cfg) {
	opennova::LobbyIdentityParams out;

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

	out.nwhwi = opennova::make_nwhwi(collect_hardware_identity(cfg));

	// The stand-in tokens' machine words take the renderer's adapter name (the model name, then
	// "OpenNova", where it has none).
	RenderingServer *rendering = RenderingServer::get_singleton();
	std::string adapter_name =
			rendering ? to_std(rendering->get_video_adapter_name()) : std::string();
	if (adapter_name.empty() && os) adapter_name = to_std(os->get_model_name());
	if (adapter_name.empty()) adapter_name = "OpenNova";
	std::replace(adapter_name.begin(), adapter_name.end(), '$', '_');

	// Retail derives these telemetry-only tokens from stable volume/MAC data. Off
	// Windows, Godot's platform-stable opaque ID stands in: hashed into the words
	// retail's inputs would supply, then retail's own encoders (engine
	// fallback_machine_tokens), so no raw machine identifier reaches the wire.
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
