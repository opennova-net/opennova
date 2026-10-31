#pragma once
// The rules a fixture runs where it names no other title's: Joint Operations'. No program and no
// test runs a default title's rules: every holder (a World's session and match rules, a
// GameConfig, a replica pipeline or a client runtime, a joiner role, match rules, a waypoint walk's
// context; a role's binding catalog too) starts with none and aborts, named, at its first read
// (world::require_title_rules, inmatch::Role::controls), and world::title_rules has no default
// for a code no title has (ADR 0076, 2026-10-30). So every holder a test builds names its title's,
// through these where that title is Joint Operations'; a test about another title names that
// title's itself.
//
// Each namer takes the holder by reference, a template over its type so this header pulls in none
// of theirs, and returns true so a fixture's member can name the holder declared before it
// (`bool world_jo = test_titles::jo_world(world);`).

#include <base/title/title.h>
#include <runtime/controls/controls.h>
#include <runtime/world/title_rules.h>

namespace test_titles {

// Joint Operations' rules.
inline const opennova::world::TitleRules &jo_rules() { return opennova::world::title_rules("jo"); }

// A World (a kernel's: its world) runs Joint Operations' rules: its session's and its match's.
template <class World> bool jo_world(World &world) {
	world.rules.title_rules = &jo_rules();
	world.match.live_rules().title_rules = &jo_rules();
	return true;
}

// A GameConfig (a session context's: its config) holds Joint Operations' rules; its title stays
// as it was (GameConfig::set_title names both).
template <class Config> bool jo_config(Config &config) {
	config.rules = &jo_rules();
	return true;
}
// A temporary GameConfig under Joint Operations' rules.
template <class Config> Config jo_config_of(Config config) {
	jo_config(config);
	return config;
}

// A replica pipeline or a client runtime folds under Joint Operations' rules.
template <class Folds> bool jo_folds(Folds &folds) {
	folds.set_rules(&jo_rules());
	return true;
}

// A role reads Joint Operations' binding catalog (inmatch::Role::set_controls, no default either:
// a role with none aborts at its first input frame).
template <class Role> bool jo_controls(Role &role) {
	role.set_controls(opennova::controls::title_catalog(*opennova::title_by_code("jo")));
	return true;
}

// A joiner role runs Joint Operations' rules and reads its binding catalog.
template <class Role> bool jo_joiner(Role &role) {
	role.set_rules(jo_rules());
	return jo_controls(role);
}

// Match rules or a waypoint walk's context hold Joint Operations' rules.
template <class Rules> bool jo_match(Rules &rules) {
	rules.title_rules = &jo_rules();
	return true;
}
// Temporary match rules under Joint Operations' rules.
template <class Rules> Rules jo_match_of(Rules rules) {
	jo_match(rules);
	return rules;
}

} // namespace test_titles
