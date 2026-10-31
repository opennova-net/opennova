#pragma once

namespace opennova::games {

// Fills the engine's title and protocol registries (base/title/title.h install_titles,
// net/protocol/protocol.h install_protocols) with every game's rows: opennova_games, which links
// base and net alone. Each program calls it once at its start, before anything reads a title; a
// second call does nothing.
void install();

// Fills the engine's rules registry (runtime/world/title_rules.h install_title_rules) with the
// TitleRules every title runs: opennova_games_rules, which links the runtime. A program that runs
// a match (a session, a World, the editor's previews) calls it after install(); one that only
// reads titles and protocols (the updater, the gate, the data program) links and calls neither
// it nor the runtime. A second call does nothing.
void install_rules();

// Both are included only at the registration points: a program's start and the tests'
// opennova_test_games (scripts/lint/include_graph_check.py).

} // namespace opennova::games
