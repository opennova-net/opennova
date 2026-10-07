#include <editor/session/sounds_playing.h>

#include <string>
#include <vector>

#include <editor/preview/mission_viewport.h>
#include <editor/preview/viewports.h>
#include <editor/session/clip_sounds.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>

namespace opennova::editor {

namespace {

// How many of the previews' last sounds the answer carries.
constexpr size_t kRecentClipSounds = 32;

} // namespace

io::JsonValue sounds_playing_json(SessionCore &core) {
	io::JsonValue out = io::JsonValue::make_object();
	const SessionView &view = core.view();
	const io::JsonValue workspace = workspace_to_json(view);
	const io::JsonValue *sound = workspace.get("sound");
	out.set("sound", sound ? *sound : io::JsonValue::make_null());
	io::JsonValue clips = io::JsonValue::make_array();
	for (const ClipSoundFired *fired : clip_sounds_recent(core, kRecentClipSounds)) clips.push(clip_sound_fired_to_json(*fired));
	out.set("clip_sounds", std::move(clips));
	io::JsonValue listening = io::JsonValue::make_array();
	const Viewports &viewports = core.viewports();
	for (size_t i = 0; i < viewports.size(); ++i) {
		const auto *mission = dynamic_cast<const MissionViewport *>(&viewports.at(i));
		if (!mission || !mission->options().listen.on || !mission->listen().open()) continue;
		io::JsonValue row = io::JsonValue::make_object();
		row.set("path", io::json_string(mission->path()));
		row.set("listen", mission->listen().to_json(mission->options().listen));
		listening.push(std::move(row));
	}
	out.set("listening", std::move(listening));
	return out;
}

} // namespace opennova::editor
