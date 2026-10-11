#pragma once

// What the Event board draws of the running game's logic (ADR 0046 S24, Live; documents/mission_live.h): the
// board calls it on each card while the game Play runs reports the mission the board shows. Lane C draws it
// (the event's latch and countdowns, Fire, a trigger's last truth, the variables' values).

#include <cstddef>
#include <string>

#include <editor/documents/mission_live.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// Where on a card: its heading (the event's, `trigger` -1), or beside one trigger's line (`trigger` its place in
// the event).
struct CardLiveSpot {
	size_t event = 0;
	int trigger = -1;
};

// The live state at a spot of the card of the mission at `path`, drawn inline after what the card drew there.
void draw_card_live(Workspace &workspace, const std::string &path, const LiveLogic &live, const CardLiveSpot &spot);

} // namespace opennova::editor
