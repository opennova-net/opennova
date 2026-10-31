#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/audio/music_context.h>

namespace godot {

// One resolved interactive-music pair (engine runtime/audio/music_context.h MusicPair): the loose
// .sbf bank path and the .bin script's resource name, always from the same stem. An empty half is
// unresolved. Read-only, made by MusicDirector::resolve_music_pair.
class MusicPair : public RefCounted {
	GDCLASS(MusicPair, RefCounted)

public:
	void assign(const opennova::audio::MusicPair &p_pair);

	String get_bank() const { return bank_; }
	String get_script_name() const { return script_name_; }

protected:
	static void _bind_methods();

private:
	String bank_;
	String script_name_;
};

} // namespace godot
