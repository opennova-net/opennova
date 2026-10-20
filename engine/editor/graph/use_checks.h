#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

class AssetGraph;
class ValidationCache;
struct ValidationInput;

// What other files make of what a file defines (ADR 0046 S13 D4): the findings about a file that
// its own validation (DocumentType::validate_file, over its document alone) cannot make, since
// they read the whole project through the asset graph: what the menus use a stylesheet's
// variables as; what each texture reference's role asks of the file its loader opens (S18). One row
// per asset kind with such a check, in AssetKind's order (kUseChecks, use_checks.cpp); a new
// cross-file check is one row. Each reads the graph as its last update left it (its symbols carry a
// definition's record, line and value) and, of the files the validation asked, only those whose own
// checks read their records (ValidationCache::records_checked: a file that did not load, or that a
// source error blocks, reports that alone), and the project's files as the validation's input lists
// them (the texture check reads its files' headers, each again only once its stamp moves), and adds
// its findings in the files' order. One row also takes a file's own finding back: the terrain gate,
// which the game runs over each mission's merged configuration (its .trn, overcast.def, its .env), takes
// back the .trn's own terrain.refused where every mission reading the .trn loads it.
using UseCheck = void (*)(const AssetGraph &graph, const ValidationCache &files, const ValidationInput &input,
		std::vector<Diagnostic> &out);

struct UseCheckRow {
	AssetKind kind = AssetKind::Unknown;
	UseCheck check = nullptr;
};

// The row of a kind; null for a kind with no use check.
const UseCheckRow *use_check(AssetKind kind);
// Every row's check, in the table's order.
void run_use_checks(const AssetGraph &graph, const ValidationCache &files, const ValidationInput &input,
		std::vector<Diagnostic> &out);
// What the cross-file checks keep between compositions let go (each check's answers by what they were made of:
// the graph's generation, the scan's serial, the files read): the next run makes every answer again, which is
// the answer it kept (a test's, comparing the two).
void forget_use_checks();

// A few answers of a cross-file check kept by what each was made of (a value of `Key`, compared whole), the one
// asked least recently let go past kKept: the session's project and the game's own data's run validate apart,
// each keeping its own. Not thread-safe: its owner holds its lock.
template <typename Key, typename Value> class UseCheckMemo {
public:
	static constexpr size_t kKept = 4;
	// The answer kept for `key`; null for none.
	Value *find(const Key &key) {
		for (Slot &slot : slots_)
			if (slot.key == key) {
				slot.used = ++clock_;
				return &slot.value;
			}
		return nullptr;
	}
	// `value` kept for `key` (in place of one kept for it), the least recently asked let go past kKept.
	Value &keep(const Key &key, Value value) {
		for (Slot &slot : slots_)
			if (slot.key == key) {
				slot.value = std::move(value);
				slot.used = ++clock_;
				return slot.value;
			}
		if (slots_.size() >= kKept)
			slots_.erase(std::min_element(slots_.begin(), slots_.end(),
					[](const Slot &a, const Slot &b) { return a.used < b.used; }));
		slots_.push_back({key, std::move(value), ++clock_});
		return slots_.back().value;
	}
	void clear() { slots_.clear(); }

private:
	struct Slot {
		Key key;
		Value value;
		uint64_t used = 0;
	};
	std::vector<Slot> slots_;
	uint64_t clock_ = 0;
};

} // namespace opennova::editor
