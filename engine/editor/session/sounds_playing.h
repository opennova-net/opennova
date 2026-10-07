#pragma once

#include <base/io/json.h>

namespace opennova::editor {

class SessionCore;

// What the editor plays now, in one answer (ADR 0046 DI-36, the `sounds_playing` query): `sound`, the workspace's
// one preview play (play_sound: a wave, a set, a profile's slot, its state and its voices); `clip_sounds`, the last
// sounds the previews fired for the Shell's clip voices, oldest first (a clip's events, a death, a weapon's actions,
// the Listen's thunder and its script's and items' sounds: each its seq, the document, the set, its state, words and
// voices); and `listening`, each mission view whose Listen is on, its path and its `listen` (what each of the game's
// channels plays, the sources by what they do, the weather, the script).
io::JsonValue sounds_playing_json(SessionCore &core);

} // namespace opennova::editor
