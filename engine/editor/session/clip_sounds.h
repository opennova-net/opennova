#pragma once

#include <cstdint>
#include <vector>

#include <editor/session/view/workspace_view.h>

namespace opennova::editor {

class SessionCore;

// A clip's events heard in the model preview (DI-04; preview/preview_clip_sounds): the session runs what
// the clock runs. Each time the Shell's frames pass the preview clock (ProjectSession::advance), the clip
// the Preview's model viewport plays fires the sounds of the ticks it ran through, each member picked
// through the session's one sound selector (the one every preview play draws from, as the game's one
// stream), and keeps them for its envelope (sounds_fired) and the Shell, which plays them alongside one
// another (clip_sounds_since).

// A clip sound for the Shell to play: its place in the order of every clip sound fired, and its voices
// (each a wave of the project at the pitch and the volume the game's pick gave it).
struct ClipSoundPlay {
	uint64_t seq = 0;
	std::vector<WorkspaceView::Voice> voices;
};

// The sounds the clip the Preview's model viewport plays fired over the ticks the clock ran through since
// the last call (ModelViewport::fire_sounds), or, for a model, the sounds of the death its damage state
// plays (DI-10: ModelViewport::fire_damage_sounds), its viewport followed first where no device follows it;
// nothing while the clock holds. Moves the Viewports concern when one fired.
void fire_clip_sounds(SessionCore &core);

// The clip sounds fired after `after` that sound (played: not muted, a wave of the project to play),
// oldest first: what the Shell plays.
std::vector<ClipSoundPlay> clip_sounds_since(SessionCore &core, uint64_t after);

} // namespace opennova::editor
