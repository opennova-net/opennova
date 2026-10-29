#pragma once

namespace opennova::games {

// Fills the engine's title, protocol and rules registries (base/title/title.h install_titles,
// net/protocol/protocol.h install_protocols, runtime/world/title_rules.h install_title_rules) with
// every game's rows. Each program calls it once
// at its start, before anything reads a title; a second call does nothing. Included only at the
// registration points: a program's start and the tests' opennova_test_games
// (scripts/lint/include_graph_check.py).
void install();

} // namespace opennova::games
