#include "nova_mnu_document.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

using namespace godot;

// Snapshot restore and hierarchy reparenting share one invariant: the parallel
// id tree must remain structurally identical to the native MNU window tree.
// Keeping that state transition implementation together makes the invariant
// local while NovaMnuDocument remains the sole interface used by callers.

void NovaMnuDocument::collect_id_window(const IdWindow &node, PackedInt32Array &out) const {
	out.push_back(node.id);
	for (const auto &child : node.children) {
		collect_id_window(child, out);
	}
}

PackedInt32Array NovaMnuDocument::collect_ids() const {
	PackedInt32Array out;
	for (const auto &s : ids_) {
		out.push_back(s.id);
		collect_id_window(s.root, out);
	}
	return out;
}

bool NovaMnuDocument::build_id_window_from_list(const mnu::Window &w, const PackedInt32Array &ids, int &k, IdWindow &out) const {
	if (k >= ids.size()) {
		return false;
	}
	out.id = ids[k++];
	out.children.clear();
	out.children.reserve(w.children.size());
	for (const auto &child : w.children) {
		IdWindow child_node;
		if (!build_id_window_from_list(child, ids, k, child_node)) {
			return false;
		}
		out.children.push_back(std::move(child_node));
	}
	return true;
}

Dictionary NovaMnuDocument::capture_state() const {
	Dictionary state;
	state["mnu"] = to_byte_array();
	state["ids"] = collect_ids();
	state["next_id"] = next_id_;
	state["menu_size"] = menu_size_;
	return state;
}

void NovaMnuDocument::apply_state(const Dictionary &p_state) {
	if (!p_state.has("mnu")) {
		return;
	}
	const PackedByteArray packed = p_state.get("mnu", PackedByteArray());
	std::vector<uint8_t> bytes(static_cast<size_t>(packed.size()));
	if (!bytes.empty()) {
		std::memcpy(bytes.data(), packed.ptr(), bytes.size());
	}
	mnu::Document parsed;
	std::string error;
	if (!mnu::parse(bytes.data(), bytes.size(), parsed, error)) {
		UtilityFunctions::push_warning("NovaMnuDocument::apply_state: parse failed: ", error.c_str());
		return;
	}
	doc_ = std::move(parsed);

	// Rebuild the id tree from the stored pre-order list (byte-identical restore),
	// walking doc_ in the same order rebuild_ids would. On any under-/over-run the
	// list is out of step with the parsed tree, so fall back to positional ids.
	const PackedInt32Array ids = p_state.get("ids", PackedInt32Array());
	bool ok = ids.size() > 0;
	std::vector<IdScreen> rebuilt;
	int k = 0;
	if (ok) {
		rebuilt.reserve(doc_.screens.size());
		for (const auto &screen : doc_.screens) {
			if (k >= ids.size()) {
				ok = false;
				break;
			}
			IdScreen s;
			s.id = ids[k++];
			if (!build_id_window_from_list(screen.root_window, ids, k, s.root)) {
				ok = false;
				break;
			}
			rebuilt.push_back(std::move(s));
		}
		if (ok && k != ids.size()) {
			ok = false; // extra ids: tree had fewer nodes than the list
		}
	}

	if (ok) {
		ids_ = std::move(rebuilt);
		int max_id = 0;
		for (int64_t i = 0; i < ids.size(); ++i) {
			if (ids[i] > max_id) {
				max_id = ids[i];
			}
		}
		const int stored_next = static_cast<int>(p_state.get("next_id", max_id + 1));
		next_id_ = stored_next > max_id + 1 ? stored_next : max_id + 1;
	} else {
		UtilityFunctions::push_warning("NovaMnuDocument::apply_state: id list desync, falling back to positional ids");
		rebuild_ids();
	}

	const Vector2i restored_size = p_state.get("menu_size", menu_size_);
	menu_size_ = restored_size;
	touch();
}

bool NovaMnuDocument::reparent_widget(int p_id, int p_new_parent_id, int p_index) {
	const Locator src = locate(p_id);
	// Refuse an unknown id, a screen, or a screen's root window (path empty).
	if (!src.valid() || src.is_screen || src.path.empty()) {
		return false;
	}
	if (p_new_parent_id == p_id) {
		return false;
	}
	if (!locate(p_new_parent_id).valid()) {
		return false;
	}
	// Cycle check: the new parent must not be p_id or one of its descendants.
	for (int cur = p_new_parent_id; cur > 0 && widget_exists(cur);) {
		if (cur == p_id) {
			return false;
		}
		if (is_screen(cur)) {
			break;
		}
		cur = get_parent_id(cur);
	}

	// Resolve the source parent vectors + the child's index within them.
	const int src_index = src.path.back();
	mnu::Window *src_parent = &doc_.screens[src.screen_index].root_window;
	IdWindow *src_id_parent = &ids_[src.screen_index].root;
	for (size_t i = 0; i + 1 < src.path.size(); ++i) {
		src_parent = &src_parent->children[src.path[i]];
		src_id_parent = &src_id_parent->children[src.path[i]];
	}

	// Move the subtree (and its mirrored id subtree) out, then erase the slot.
	mnu::Window moved = std::move(src_parent->children[src_index]);
	IdWindow moved_id = std::move(src_id_parent->children[src_index]);
	src_parent->children.erase(src_parent->children.begin() + src_index);
	src_id_parent->children.erase(src_id_parent->children.begin() + src_index);

	// Re-locate the destination AFTER the erase (its path may have shifted when it
	// was a later sibling in the same parent). It cannot have vanished: it is not
	// inside the moved subtree (cycle check) and still exists in the tree.
	const Locator dst = locate(p_new_parent_id);
	mnu::Window *dst_parent = nullptr;
	IdWindow *dst_id_parent = nullptr;
	if (dst.valid()) {
		if (dst.is_screen) {
			dst_parent = &doc_.screens[dst.screen_index].root_window;
			dst_id_parent = &ids_[dst.screen_index].root;
		} else {
			dst_parent = window_at(dst);
			dst_id_parent = id_window_at(dst);
		}
	}
	if (dst_parent == nullptr || dst_id_parent == nullptr) {
		// Unreachable given the upfront guards; restore the node to keep the tree
		// consistent rather than dropping it.
		src_parent->children.insert(src_parent->children.begin() + src_index, std::move(moved));
		src_id_parent->children.insert(src_id_parent->children.begin() + src_index, std::move(moved_id));
		return false;
	}

	// When src and dst share the same parent vector and the node sat before the
	// requested slot, the erase shifted everything down by one (the requested
	// index was computed against the pre-erase tree).
	const bool same_parent = (dst_parent == src_parent);
	int insert_index = p_index;
	if (same_parent && src_index < insert_index) {
		insert_index -= 1;
	}
	if (insert_index < 0) {
		insert_index = 0;
	}
	if (insert_index > static_cast<int>(dst_parent->children.size())) {
		insert_index = static_cast<int>(dst_parent->children.size());
	}
	dst_parent->children.insert(dst_parent->children.begin() + insert_index, std::move(moved));
	dst_id_parent->children.insert(dst_id_parent->children.begin() + insert_index, std::move(moved_id));
	// A drop that lands the node back in its original slot is a no-op: the insert
	// above already restored the tree byte-for-byte, so report "not moved" without
	// a change event, so the caller records no undo entry and does not dirty the
	// document.
	if (same_parent && insert_index == src_index) {
		return false;
	}
	touch();
	return true;
}
