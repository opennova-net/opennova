#pragma once

#include <string>
#include <vector>

#include <editor/preview/sound_preview.h>
#include <editor/session/editor_request.h>
#include <runtime/audio/sound_profile.h>

namespace opennova::editor {

class SessionCore;
struct SessionView;

// The editor's sound plays over the project (the sound lane, DI-02): what preview/sound_preview plans, fed
// from the project's own files, each open document standing in for its file so an edit not saved yet is
// heard, and handed to the Shell's one preview player through the workspace's sound
// (WorkspaceView::Sound, ProjectSession::report_sound).

// The project's sound banks: each .lwf of the scan, an open bank document's rows in place of its file; a
// bank that does not read left out.
std::vector<PreviewBank> project_banks(const SessionView &view);
// The project's sound profiles: its SndProf.def as the game reads it, an open document's rows in place of
// the file; none without one.
std::vector<audio::SoundProfile> project_profiles(const SessionView &view);
// The project's expansion's name ("" standalone): the chain's first two banks are its own.
std::string project_expansion(const SessionView &view);

// The play_sound request (request_kinds.cpp's row): `values` name what plays, `path` a file it plays from:
// - `set`: a sound set by name, from the bank `path` names, else the first bank of the game's chain
//   holding it;
// - `profile` and `slot` (a keyword, SSRFootGND, or its number): the profile's slot; `slot` "footstep"
//   with `surface` (ground, snow, object, water) and `foot` (left, right) the slot a footstep plays
//   there (DI-04's seam), `profile` left out meaning "default" (what an item with no sound_profile binds);
// - `frame`: what the clip the animation document at `path` (the active one when left out) plays at that
//   frame under its model viewport's sound options, as a press of the event's mark on the timeline asks
//   (DI-04, preview/preview_clip_sounds);
// - none: the project's wave `path`.
// Refused, the status line saying why (workspace.refused): a name no file has, a set no bank the game
// searches holds, an empty slot, waves the project lacks, a wave past what a card reads.
void serve_sound_play(SessionCore &core, const EditorRequest &request);

} // namespace opennova::editor
