#include "network/novaworld_identity.h"

#include <formats/gamecfg/game_cfg.h> // gamecfg::display_size
#include <net/novaworld/cookie_jar_file.h> // process_menu_browser (the jar the activation rides)
#include <net/novaworld/http_login.h> // activation_cd_keys
#include <net/novaworld/lobby_identity.h>
#include <net/protocol/protocol.h> // Protocol::cd_key

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
#include <optional>
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

// The boot's sample of the NWHWI device facts (sample_device_identity), held for the process.
std::optional<opennova::HardwareIdentity> &device_identity() {
	static std::optional<opennova::HardwareIdentity> sample;
	return sample;
}

// The NWHWI device facts (engine HardwareIdentity, make_nwhwi): on Windows the engine's reads,
// retail's own (read_display_adapter on the cfg's hw3d_deviceno, read_primary_screen_size);
// elsewhere the renderer's adapter name and memory and the display server's primary screen stand
// in. The physical memory is the OS's. A failed DxDiag query leaves the display memory 0.
opennova::HardwareIdentity read_device_identity(const opennova::gamecfg::GameCfg &cfg) {
	opennova::HardwareIdentity hw;
	if (!opennova::read_display_adapter(static_cast<uint32_t>(cfg.hw3d_deviceno), hw)) {
		if (RenderingServer *rendering = RenderingServer::get_singleton()) {
			hw.adapter_description = to_std(rendering->get_video_adapter_name());
			hw.has_adapter = !hw.adapter_description.empty();
			if (RenderingDevice *device = rendering->get_rendering_device()) {
				uint64_t video_memory = device->get_device_total_memory();
				if (video_memory == 0) video_memory = device->get_driver_total_memory();
				// In retail's 32-bit view, as the engine's read gives it (wow64_display_memory_mb).
				hw.display_memory_mb =
						opennova::wow64_display_memory_mb(static_cast<int64_t>(video_memory >> 20u));
			}
		}
	}
	opennova::read_total_physical_memory(hw.total_phys_bytes);
	const Vector2i desktop = primary_desktop_size();
	hw.desktop_width = desktop.x;
	hw.desktop_height = desktop.y;
	return hw;
}

} // namespace

Vector2i primary_desktop_size() {
	int32_t width = 0;
	int32_t height = 0;
	if (opennova::read_primary_screen_size(width, height)) return Vector2i(width, height);
	if (DisplayServer *display = DisplayServer::get_singleton())
		return display->screen_get_size(DisplayServer::SCREEN_PRIMARY);
	return Vector2i();
}

void sample_device_identity(const opennova::gamecfg::GameCfg &cfg) {
	std::optional<opennova::HardwareIdentity> &sample = device_identity();
	if (!sample) sample = read_device_identity(cfg);
}

opennova::LobbyMachineTokens machine_tokens() {
	// Retail derives these tokens from stable volume/MAC data. Off Windows, Godot's
	// platform-stable opaque ID stands in: hashed into the words retail's inputs would supply,
	// then retail's own encoders (engine fallback_machine_tokens), so no raw machine identifier
	// reaches the wire.
	opennova::RetailMachineInputs machine;
	if (opennova::read_retail_machine_inputs(machine))
		return opennova::make_retail_machine_tokens(machine);
	OS *os = OS::get_singleton();
	// The stand-in tokens' machine words take the renderer's adapter name (the model name, then
	// "OpenNova", where it has none).
	RenderingServer *rendering = RenderingServer::get_singleton();
	std::string adapter_name =
			rendering ? to_std(rendering->get_video_adapter_name()) : std::string();
	if (adapter_name.empty() && os)
		adapter_name = to_std(os->get_model_name());
	if (adapter_name.empty())
		adapter_name = "OpenNova";
	std::replace(adapter_name.begin(), adapter_name.end(), '$', '_');
	std::string stable_identity = os ? to_std(os->get_unique_id()) : std::string();
	if (stable_identity.empty() && os) {
		stable_identity = to_std(os->get_processor_name()) + "|" + to_std(os->get_model_name());
	}
	stable_identity += "|" + adapter_name;
	return opennova::fallback_machine_tokens(stable_identity);
}

opennova::cdkey::WireKeys machine_activation_cd_keys(const opennova::Protocol *protocol) {
	if (protocol == nullptr || protocol->cd_key == nullptr)
		return {};
	return opennova::activation_cd_keys(
			protocol->cd_key, opennova::process_menu_browser().jar, machine_tokens().nwpssk);
}

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

	// NWHWI at the connect only formats: the boot's device facts (a process that skipped the boot's
	// sample takes it here, once) and game.cfg's display mode now.
	sample_device_identity(cfg);
	opennova::HardwareIdentity hw = *device_identity();
	opennova::gamecfg::display_size(cfg, hw.desktop_width, hw.desktop_height, hw.display_width,
			hw.display_height);
	out.nwhwi = opennova::make_nwhwi(hw);

	const opennova::LobbyMachineTokens tokens = machine_tokens();
	out.nwpssk = tokens.nwpssk;
	out.nwusid = tokens.nwusid;

	// The supported retail capture and OpenNova base install both advertise 0.
	// Keep the field explicit so an expansion-aware binding can replace it.
	out.my_installed_exp_bits = "0";
	return out;
}

} // namespace godot
