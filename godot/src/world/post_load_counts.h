#pragma once

// The process's object counts at a mission load's end and at each post-load frame's start, with
// what the targets' cameras admit (an instrument, --verbose only): the GameWorld frame's
// post-load timeline prints them. Split out of game_world_frame.cpp by responsibility (the
// oversize-TU ratchet, A2-43).

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class ResourceRoot;

void print_post_load_counts(Node *p_world, const ResourceRoot *p_root, const String &p_label);

} // namespace godot
