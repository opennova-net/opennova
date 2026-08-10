#include "nova_edit_history.h"

#include <godot_cpp/core/error_macros.hpp>

using namespace godot;

void EditHistory::set_limit(int p_limit) {
	ERR_FAIL_COND_MSG(p_limit < 1, "EditHistory limit must be at least 1.");
	history_ = opennova::edit::EditHistory<Variant, VariantEqual>(static_cast<std::size_t>(p_limit));
}

void EditHistory::begin_edit(const Variant &p_live) {
	ERR_FAIL_COND_MSG(p_live.get_type() == Variant::NIL, "EditHistory snapshots cannot be null.");
	history_.begin(p_live);
}

bool EditHistory::commit_edit(const Variant &p_live) {
	ERR_FAIL_COND_V_MSG(p_live.get_type() == Variant::NIL, false, "EditHistory snapshots cannot be null.");
	return history_.commit(p_live);
}

Variant EditHistory::undo_swap(const Variant &p_live) {
	ERR_FAIL_COND_V_MSG(p_live.get_type() == Variant::NIL, Variant(), "EditHistory snapshots cannot be null.");
	Variant doc = p_live;
	if (!history_.swap_undo(doc)) {
		return Variant();
	}
	return doc;
}

Variant EditHistory::redo_swap(const Variant &p_live) {
	ERR_FAIL_COND_V_MSG(p_live.get_type() == Variant::NIL, Variant(), "EditHistory snapshots cannot be null.");
	Variant doc = p_live;
	if (!history_.swap_redo(doc)) {
		return Variant();
	}
	return doc;
}

void EditHistory::push_step(const Variant &p_snapshot) {
	ERR_FAIL_COND_MSG(p_snapshot.get_type() == Variant::NIL, "EditHistory snapshots cannot be null.");
	history_.push(p_snapshot);
}

Variant EditHistory::pop_undo() {
	Variant step;
	if (!history_.pop_undo(step)) {
		return Variant();
	}
	return step;
}

Variant EditHistory::pop_redo() {
	Variant step;
	if (!history_.pop_redo(step)) {
		return Variant();
	}
	return step;
}

bool EditHistory::can_undo() const {
	return history_.can_undo();
}

bool EditHistory::can_redo() const {
	return history_.can_redo();
}

int EditHistory::undo_depth() const {
	return static_cast<int>(history_.undo_depth());
}

void EditHistory::mark_clean(const Variant &p_live) {
	ERR_FAIL_COND_MSG(p_live.get_type() == Variant::NIL, "EditHistory snapshots cannot be null.");
	history_.mark_clean(p_live);
}

bool EditHistory::is_dirty(const Variant &p_live, bool p_fallback) const {
	return history_.is_dirty(p_live, p_fallback);
}

void EditHistory::clear() {
	history_.clear();
}

void EditHistory::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_limit", "limit"), &EditHistory::set_limit);
	ClassDB::bind_method(D_METHOD("begin_edit", "live"), &EditHistory::begin_edit);
	ClassDB::bind_method(D_METHOD("commit_edit", "live"), &EditHistory::commit_edit);
	ClassDB::bind_method(D_METHOD("undo_swap", "live"), &EditHistory::undo_swap);
	ClassDB::bind_method(D_METHOD("redo_swap", "live"), &EditHistory::redo_swap);
	ClassDB::bind_method(D_METHOD("push_step", "snapshot"), &EditHistory::push_step);
	ClassDB::bind_method(D_METHOD("pop_undo"), &EditHistory::pop_undo);
	ClassDB::bind_method(D_METHOD("pop_redo"), &EditHistory::pop_redo);
	ClassDB::bind_method(D_METHOD("can_undo"), &EditHistory::can_undo);
	ClassDB::bind_method(D_METHOD("can_redo"), &EditHistory::can_redo);
	ClassDB::bind_method(D_METHOD("undo_depth"), &EditHistory::undo_depth);
	ClassDB::bind_method(D_METHOD("mark_clean", "live"), &EditHistory::mark_clean);
	ClassDB::bind_method(D_METHOD("is_dirty", "live", "fallback"), &EditHistory::is_dirty, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("clear"), &EditHistory::clear);
}
