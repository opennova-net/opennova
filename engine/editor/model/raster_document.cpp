#include "raster_document.h"

#include <algorithm>
#include <map>
#include <utility>

#include <editor/model/diagnostic.h>
#include <editor/model/finding_code_row.h>

namespace opennova::editor {

namespace {

bool refuse(Diagnostic &error, const std::string &path, CoreFinding code, const std::string &message) {
	error = make_finding(code, DiagnosticSeverity::Error, message, path);
	return false;
}

// A change put back on `state` (a batch refused after its edits wrote, an amend that cannot land).
void revert(const RasterChange &change, RasterState &state) {
	if (change.whole) {
		state = change.whole_before;
		return;
	}
	for (auto it = change.swaps.rbegin(); it != change.swaps.rend(); ++it)
		state.planes[static_cast<size_t>(it->plane)].set_tile(it->tile, it->before);
	if (change.side_after) state.side = change.side_before;
}

} // namespace

RasterCodec::~RasterCodec() = default;

std::string RasterCodec::save_words(const RasterState &state) const {
	(void)state;
	return std::string();
}

RasterWriter &RasterCanvas::write(size_t index) {
	if (writers_.size() < state_.planes.size()) writers_.resize(state_.planes.size());
	std::unique_ptr<RasterWriter> &writer = writers_[index];
	if (!writer) writer = std::make_unique<RasterWriter>(state_.planes[index], static_cast<int>(index));
	return *writer;
}

void RasterCanvas::set_side(std::shared_ptr<const RasterSideState> side) {
	if (!side_set_) side_before_ = state_.side;
	side_set_ = true;
	state_.side = std::move(side);
}

void RasterCanvas::replace_whole(RasterState state) {
	if (!whole_) {
		// What the edits wrote before stays in the whole before: the content as the batch found it.
		RasterChange partial = finish();
		RasterState found = state_;
		revert(partial, found);
		whole_before_ = std::move(found);
	}
	whole_ = true;
	state_ = std::move(state);
}

RasterChange RasterCanvas::finish() {
	RasterChange change;
	if (whole_) {
		change.whole = true;
		change.whole_before = std::move(whole_before_);
		change.whole_after = state_;
	} else {
		for (std::unique_ptr<RasterWriter> &writer : writers_)
			if (writer)
				for (TileSwap &swap : writer->finish()) change.swaps.push_back(std::move(swap));
		if (side_set_) {
			change.side_before = side_before_;
			change.side_after = state_.side;
		}
	}
	writers_.clear();
	side_before_.reset();
	side_set_ = false;
	whole_ = false;
	whole_before_ = RasterState();
	return change;
}

void RasterCanvas::put_back() { revert(finish(), state_); }

RasterDocument::RasterDocument(RasterCodecPicker pick, HistoryBudget budget) : pick_(pick), history_(budget) {}

RasterDocument::RasterDocument(const RasterDocument &other)
		: DocumentBase(other), pick_(other.pick_), codec_(other.codec_), state_(other.state_),
		  history_(other.history_.frozen()) {}

Edit RasterDocument::edit(std::shared_ptr<const RasterEditPayload> payload, uint64_t gesture) {
	Edit out;
	out.operation = EditOperation::Apply;
	out.gesture = gesture;
	out.payload = std::move(payload);
	return out;
}

bool RasterDocument::apply_edits(const std::vector<Edit> &edits, Diagnostic &error) {
	RasterCanvas canvas(state_);
	bool amends = false;
	for (const Edit &edit : edits) {
		const auto *payload = edit.operation == EditOperation::Apply
		                              ? dynamic_cast<const RasterEditPayload *>(edit.payload.get())
		                              : nullptr;
		if (!payload) {
			canvas.put_back();
			return refuse(error, path(), CoreFinding::DocumentPayload,
			              "A raster document takes only its type's raster edits.");
		}
		amends = amends || payload->amends();
		if (!payload->apply(canvas, error)) {
			canvas.put_back();
			if (error.asset.empty()) error.asset = path();
			return false;
		}
	}
	RasterChange change = canvas.finish();
	if (change.empty()) return true;
	if (amends) {
		if (change.whole || !history_.amend_last(change)) {
			revert(change, state_);
			return refuse(error, path(), CoreFinding::DocumentStale,
			              "The step this edit belongs to is no longer the last one: it was undone or edited since.");
		}
		return true;
	}
	history_.commit(std::move(change), edits.empty() ? 0 : edits.front().gesture);
	return true;
}

bool RasterDocument::changes_since(uint64_t load_generation, uint64_t revision, ChangeSet &out) const {
	if (load_generation != this->load_generation()) return false;
	std::vector<std::pair<int, int>> tiles;
	uint32_t parts = 0;
	if (!history_.changes_since(revision, tiles, parts)) return false;
	RasterChanges changes;
	changes.parts = parts;
	// Each plane's changed tiles row by row, consecutive tiles of a row one region (tiles sorted).
	for (size_t i = 0; i < tiles.size();) {
		const int plane = tiles[i].first;
		if (plane < 0 || static_cast<size_t>(plane) >= state_.planes.size()) {
			++i;
			continue;
		}
		const RasterPlane &p = state_.planes[static_cast<size_t>(plane)];
		const int across = p.tiles_across();
		const int first = tiles[i].second;
		int last = first;
		size_t j = i + 1;
		while (j < tiles.size() && tiles[j].first == plane && tiles[j].second == last + 1 &&
		       tiles[j].second / across == first / across) {
			last = tiles[j].second;
			++j;
		}
		RasterRegion region;
		region.plane = plane;
		region.x = (first % across) * kRasterTileSide;
		region.y = (first / across) * kRasterTileSide;
		region.width = std::min((last % across + 1) * kRasterTileSide, p.width()) - region.x;
		region.height = std::min(kRasterTileSide, p.height() - region.y);
		changes.regions.push_back(region);
		i = j;
	}
	out = std::move(changes);
	return true;
}

SerializeResult RasterDocument::serialize() const {
	if (!codec_) {
		SerializeResult refused;
		refused.issues.push_back({});
		refused.issues.back().message = "The raster has no form to write: it was never read.";
		return refused;
	}
	return codec_->write(state_);
}

std::string RasterDocument::save_words() const {
	const std::string own = codec_ ? codec_->save_words(state_) : std::string();
	return own.empty() ? DocumentBase::save_words() : own;
}

std::unique_ptr<DocumentBase> RasterDocument::snapshot() const { return std::unique_ptr<DocumentBase>(new RasterDocument(*this)); }

bool RasterDocument::read_source(const std::vector<uint8_t> &decoded, bool adopt, std::vector<SourceIssue> &issues,
                                 Diagnostic &error) {
	std::shared_ptr<const RasterCodec> codec = pick_codec(path(), decoded);
	if (!codec) return refuse(error, path(), CoreFinding::DocumentDecode, "No raster form reads " + path() + ".");
	RasterState read;
	if (!codec->read(decoded, read, issues, error)) {
		if (error.asset.empty()) error.asset = path();
		return false;
	}
	if (adopt) {
		codec_ = std::move(codec);
		state_ = std::move(read);
		history_.reset();
	}
	return true;
}

void RasterDocument::on_saved() {
	history_.mark_saved();
	history_.end_edit_group();
}

} // namespace opennova::editor
