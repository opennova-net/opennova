#pragma once

// The mission-start weather seed: the ONE derivation from the mission-selected
// ENV resource + the BMS header onto world::WeatherState (ADR 0042 d2 — an
// engine fact computed in one engine function). Every serving embedder runs
// it (the shell from its parsed EnvFile + mission header, the dedicated host
// from the mounted resource) before the eager WAC execution and the 255-tick
// settle [orig: Environment_LoadTimeOfDayConfig @ 0x57db30 ->
// Environment_SnapStateToTargets @ 0x57d1e0; the clock @ 0x525371;
// Game_LoadTerrainDuringConnect @ 0x520710 applies the BMS fog override
// before the snapshot @ 0x525383/0x525393].

#include <formats/env/env.h>
#include <formats/mission/bms.h>
#include <runtime/world/weather_state.h>

#include <istream>
#include <string>

namespace opennova::env {

// The seed from an already-parsed (and BMS-overridden) config plus the BMS
// clock header: fog_level << 16, sky_height << 16, sky_speed << 10, start_time
// << 16 (8.24 hours), 0x18000000 / (3720 * max(minutes_per_day, 60)), the fog
// type, lightning_rgb, and the retail wind scale.
world::WeatherSeed weather_seed_from_config(const Config &config, const bms::Header &header);

// Parse one actual ENV resource, fold the BMS header overrides onto it, and
// seed the weather home. Independent of socket/session startup so focused
// runtime harnesses need no synthetic environment.
bool seed_weather_from_env(std::istream &input, const bms::Header &header,
		world::WeatherState &weather, std::string &error);

} // namespace opennova::env
