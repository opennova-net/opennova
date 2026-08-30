// A typed weather mutation the F3 Environment window queues for its embedder
// to drain (ADR 0042 d6; the DebugRequest pattern). The window never mutates
// engine state itself — the embedder routes each request into the ONE
// command layer every WAC handler and MCP row uses (world::EntityCommands,
// ADR 0042 d5; world::WeatherState carries the handler cites).
#pragma once

#include <cstdint>

namespace opennova::devtools {

struct EnvironmentRequest {
	enum class Kind {
		Rain,             // rain(percent, seconds)
		Snow,             // snow(percent, seconds)
		Overcast,         // overcast(percent, seconds)
		FogDistance,      // fogdist(metres)
		MoveFog,          // movefog(metres, seconds)
		SkySpeed,         // skyspeed(rate)
		SkyHeight,        // skyheight(raw 16.16)
		Quake,            // quake(seconds)
		TimeOfDayMinutes, // TOD(minute of day)
		FogType,          // fogtype(type)
		SunFade,          // sunfade(percent, seconds)
		ColorFade,        // colorfade(seconds)
		Flash,            // flash
		FarFlash,         // farflash
		WindScale,        // the `wind` named value
		BlockColor,       // sun/sky/ground/floor/ceiling/cloud/fogcolor/skyfogcolor/gain(r, g, b):
		                  // a = world::WeatherColorTarget, b = packed 0xRRGGBB
		LightningColor,   // lightning(r, g, b): b = packed 0xRRGGBB
	};

	Kind kind = Kind::Rain;
	int32_t a = 0; // percent / metres / rate / seconds / minute / type / value
	int32_t b = 0; // seconds for the timed commands
};

}  // namespace opennova::devtools
