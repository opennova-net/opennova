#include <editor/ui/find_cursor.h>

#include <utility>

namespace opennova::editor {

void FindCursor::refresh(const Document &document, const std::string &text, bool match_case) {
	if (identity_ == document.identity() && revision_ == document.revision() && searched_ == text &&
	    searched_case_ == match_case)
		return;
	const bool same_search =
	        identity_ == document.identity() && searched_ == text && searched_case_ == match_case;
	SearchOptions options;
	options.match_case = match_case;
	std::vector<DocumentHit> hits = find_in_document(document, text, options);
	// The hit shown found again by its record and field; when it no longer matches, the next
	// goes to the first hit after it that still does (none: the next the first).
	const auto index_of = [&hits](const NodeAddress &address, const std::string &field) {
		for (size_t i = 0; i < hits.size(); ++i)
			if (hits[i].address == address && hits[i].field == field) return i;
		return SIZE_MAX;
	};
	size_t current = SIZE_MAX;
	bool on_hit = false;
	if (same_search && on_hit_) {
		current = index_of(hit_address_, hit_field_);
		on_hit = current != SIZE_MAX;
		for (size_t i = current_ + 1; !on_hit && current == SIZE_MAX && i < hits_.size(); ++i)
			current = index_of(hits_[i].address, hits_[i].field);
		if (!on_hit && current == SIZE_MAX) current = hits.size();
	} else if (same_search && current_ < hits_.size()) {
		for (size_t i = current_; current == SIZE_MAX && i < hits_.size(); ++i)
			current = index_of(hits_[i].address, hits_[i].field);
	}
	hits_ = std::move(hits);
	current_ = current;
	on_hit_ = on_hit;
	identity_ = document.identity();
	revision_ = document.revision();
	searched_ = text;
	searched_case_ = match_case;
}

size_t FindCursor::step(bool forward) const {
	const size_t count = hits_.size();
	if (!count) return SIZE_MAX;
	if (on_hit()) return forward ? (current_ + 1) % count : (current_ + count - 1) % count;
	if (current_ < count) return forward ? current_ : (current_ + count - 1) % count;
	return forward ? 0 : count - 1;
}

const DocumentHit *FindCursor::show(size_t index) {
	if (index >= hits_.size()) return nullptr;
	current_ = index;
	on_hit_ = true;
	hit_address_ = hits_[index].address;
	hit_field_ = hits_[index].field;
	return &hits_[index];
}

} // namespace opennova::editor
