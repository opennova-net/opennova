#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/document.h>
#include <editor/model/document_search.h>

namespace opennova::editor {

// Where the Document window's find bar is among a document's hits (ADR 0046 S12 D8; S13 V1:
// the model decides, the find bar draws). refresh() finds the hits again only when the
// document (its identity and revision), the text or whether case matters moved since. The bar
// is on a hit (the one shown: its record and field kept to find it among new hits), or between
// hits: after a hit that stopped matching, the next goes to the first hit after it that still
// does. step() is where the next or the previous goes, wrapping at either end; show() puts the
// cursor on a hit.
class FindCursor {
public:
	void refresh(const Document &document, const std::string &text, bool match_case);

	const std::vector<DocumentHit> &hits() const { return hits_; }
	// True while a hit is shown; current() is it, else the hit the next goes to (hits().size()
	// or SIZE_MAX: none, the next the first).
	bool on_hit() const { return on_hit_ && current_ < hits_.size(); }
	size_t current() const { return current_; }
	// The hit the next (forward) or the previous goes to; SIZE_MAX when there is none.
	size_t step(bool forward) const;
	// Hit `index` shown: the cursor on it. Null for an index past the hits.
	const DocumentHit *show(size_t index);

private:
	size_t current_ = SIZE_MAX;
	bool on_hit_ = false;
	NodeAddress hit_address_;
	std::string hit_field_;
	// What the hits were found with.
	uint64_t identity_ = 0;
	uint64_t revision_ = 0;
	std::string searched_;
	bool searched_case_ = false;
	std::vector<DocumentHit> hits_;
};

} // namespace opennova::editor
