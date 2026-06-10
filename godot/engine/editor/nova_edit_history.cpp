#include "nova_edit_history.h"

#include <godot_cpp/core/error_macros.hpp>

using namespace godot;

void NovaEditHistory::set_limit(int p_limit) {
	ERR_FAIL_COND_MSG(p_limit < 1, "NovaEditHistory limit must be at least 1.");
	history_ = opennova::edit::EditHistory<Variant, VariantEqual>(static_cast<std::size_t>(p_limit));
}

void NovaEditHistory::begin_edit(const Variant &p_live) {
	ERR_FAIL_COND_MSG(p_live.get_type() == Variant::NIL, "NovaEditHistory snapshots cannot be null.");
	history_.begin(p_live);
}

bool NovaEditHistory::commit_edit(const Variant &p_live) {
	ERR_FAIL_COND_V_MSG(p_live.get_type() == Variant::NIL, false, "NovaEditHistory snapshots cannot be null.");
	return history_.commit(p_live);
}

Variant NovaEditHistory::undo_swap(const Variant &p_live) {
	ERR_FAIL_COND_V_MSG(p_live.get_type() == Variant::NIL, Variant(), "NovaEditHistory snapshots cannot be null.");
	Variant doc = p_live;
	if (!history_.swap_undo(doc)) {
		return Variant();
	}
	return doc;
}

Variant NovaEditHistory::redo_swap(const Variant &p_live) {
	ERR_FAIL_COND_V_MSG(p_live.get_type() == Variant::NIL, Variant(), "NovaEditHistory snapshots cannot be null.");
	Variant doc = p_live;
	if (!history_.swap_redo(doc)) {
		return Variant();
	}
	return doc;
}

void NovaEditHistory::push_step(const Variant &p_snapshot) {
	ERR_FAIL_COND_MSG(p_snapshot.get_type() == Variant::NIL, "NovaEditHistory snapshots cannot be null.");
	history_.push(p_snapshot);
}

Variant NovaEditHistory::pop_undo() {
	Variant step;
	if (!history_.pop_undo(step)) {
		return Variant();
	}
	return step;
}

Variant NovaEditHistory::pop_redo() {
	Variant step;
	if (!history_.pop_redo(step)) {
		return Variant();
	}
	return step;
}

bool NovaEditHistory::can_undo() const {
	return history_.can_undo();
}

bool NovaEditHistory::can_redo() const {
	return history_.can_redo();
}

int NovaEditHistory::undo_depth() const {
	return static_cast<int>(history_.undo_depth());
}

void NovaEditHistory::mark_clean(const Variant &p_live) {
	ERR_FAIL_COND_MSG(p_live.get_type() == Variant::NIL, "NovaEditHistory snapshots cannot be null.");
	history_.mark_clean(p_live);
}

bool NovaEditHistory::is_dirty(const Variant &p_live, bool p_fallback) const {
	return history_.is_dirty(p_live, p_fallback);
}

void NovaEditHistory::clear() {
	history_.clear();
}

void NovaEditHistory::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_limit", "limit"), &NovaEditHistory::set_limit);
	ClassDB::bind_method(D_METHOD("begin_edit", "live"), &NovaEditHistory::begin_edit);
	ClassDB::bind_method(D_METHOD("commit_edit", "live"), &NovaEditHistory::commit_edit);
	ClassDB::bind_method(D_METHOD("undo_swap", "live"), &NovaEditHistory::undo_swap);
	ClassDB::bind_method(D_METHOD("redo_swap", "live"), &NovaEditHistory::redo_swap);
	ClassDB::bind_method(D_METHOD("push_step", "snapshot"), &NovaEditHistory::push_step);
	ClassDB::bind_method(D_METHOD("pop_undo"), &NovaEditHistory::pop_undo);
	ClassDB::bind_method(D_METHOD("pop_redo"), &NovaEditHistory::pop_redo);
	ClassDB::bind_method(D_METHOD("can_undo"), &NovaEditHistory::can_undo);
	ClassDB::bind_method(D_METHOD("can_redo"), &NovaEditHistory::can_redo);
	ClassDB::bind_method(D_METHOD("undo_depth"), &NovaEditHistory::undo_depth);
	ClassDB::bind_method(D_METHOD("mark_clean", "live"), &NovaEditHistory::mark_clean);
	ClassDB::bind_method(D_METHOD("is_dirty", "live", "fallback"), &NovaEditHistory::is_dirty, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("clear"), &NovaEditHistory::clear);
}
