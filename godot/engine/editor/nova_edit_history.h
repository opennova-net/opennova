#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <oned_edit/edit_history.h>

namespace godot {

// Boxed-snapshot undo/redo history for the GDScript editors: the thin
// GDExtension wrapper over the shared C++ core (libs/oned_edit) that
// EnvironmentEditor, SoundController, StringsEditor and the fnt/mnu op-stack
// editors otherwise each hand-roll. The mission editor uses the same core
// typed on bms::File inside NovaMissionData; this is the Variant flavor the
// edit_history.h header anticipates.
//
// Snapshots are opaque Variants the caller produces and applies itself
// (PackedByteArray from to_bytes(), op Dictionaries, ...). Equality is
// Variant == (deep for Dictionary/Array, content compare for packed arrays),
// which matches the `bytes != pending` gates the GDScript editors use today.
// A NIL Variant is reserved as the "nothing there" sentinel returned by
// undo_swap/redo_swap/pop_undo/pop_redo, so NIL snapshots are rejected.
class NovaEditHistory : public RefCounted {
	GDCLASS(NovaEditHistory, RefCounted)

	struct VariantEqual {
		bool operator()(const Variant &a, const Variant &b) const { return a == b; }
	};

	opennova::edit::EditHistory<Variant, VariantEqual> history_{100};

protected:
	static void _bind_methods();

public:
	// Replaces the history with an empty one holding `limit` steps; call
	// before use (state, baseline and any open session are dropped).
	void set_limit(int p_limit);

	// --- Whole-document bracket (environment/sound/strings flavor) ------
	void begin_edit(const Variant &p_live);
	bool commit_edit(const Variant &p_live); // true iff a step was recorded

	// Push `live` onto the redo stack and return the snapshot to adopt as
	// the new document, or NIL when there is nothing to undo (callers must
	// compare against null, not truthiness: an empty PackedByteArray is falsy).
	Variant undo_swap(const Variant &p_live);
	Variant redo_swap(const Variant &p_live);

	// --- Delta flavor (terrain/fnt/mnu op records, caller-applied) ------
	void push_step(const Variant &p_snapshot); // clears redo, caps
	Variant pop_undo();                        // moves top undo -> redo and returns it; NIL when empty
	Variant pop_redo();

	// --- State -----------------------------------------------------------
	bool can_undo() const;
	bool can_redo() const;
	int undo_depth() const;
	void mark_clean(const Variant &p_live);
	bool is_dirty(const Variant &p_live, bool p_fallback = false) const;
	void clear();
};

} // namespace godot
