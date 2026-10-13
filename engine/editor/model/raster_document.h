#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/change_set.h>
#include <editor/model/document_base.h>
#include <editor/model/edit.h>
#include <editor/model/raster_history.h>
#include <editor/model/raster_plane.h>

namespace opennova::editor {

// What a raster edit writes (RasterEditPayload::apply): the document's planes, each through a writer made
// on its first write (copy-on-write, the tiles it touches recorded), its side state, or the whole content.
class RasterCanvas {
public:
	explicit RasterCanvas(RasterState &state) : state_(state) {}
	RasterCanvas(const RasterCanvas &) = delete;
	RasterCanvas &operator=(const RasterCanvas &) = delete;

	const RasterState &state() const { return state_; }
	size_t plane_count() const { return state_.planes.size(); }
	const RasterPlane &plane(size_t index) const { return state_.planes[index]; }
	// The writer of plane `index` (one per plane for the edit's life).
	RasterWriter &write(size_t index);
	const RasterSideState *side() const { return state_.side.get(); }
	const std::shared_ptr<const RasterSideState> &side_shared() const { return state_.side; }
	// The side state replaced (the first replacement's before kept).
	void set_side(std::shared_ptr<const RasterSideState> side);
	// The whole content replaced (a byte-level operation: a resize, a re-encode): a step of its own, which
	// changes_since cannot say regions of. A canvas that replaced the whole writes nothing else.
	void replace_whole(RasterState state);
	bool replaced_whole() const { return whole_; }

	// What the edits made, the canvas then empty; and the content put back as it was (a refused batch).
	RasterChange finish();
	void put_back();

private:
	RasterState &state_;
	std::vector<std::unique_ptr<RasterWriter>> writers_;
	std::shared_ptr<const RasterSideState> side_before_;
	bool side_set_ = false;
	bool whole_ = false;
	RasterState whole_before_;
};

// The change a raster edit carries (an Apply edit's payload, ADR 0046 S25): made by the raster's type and
// immutable once made. apply() writes it on the canvas; false, with `error`, refuses the batch.
struct RasterEditPayload : EditPayload {
	virtual bool apply(RasterCanvas &canvas, Diagnostic &error) const = 0;
	// Whether the edit joins the document's last step instead of making one (a bake landing in its stroke's
	// step: RasterHistory::amend_last); refused where it cannot, unless lands_alone().
	virtual bool amends() const { return false; }
	// An amending edit the last step cannot take (a redo kept past it, or it is the saved checkpoint) made a step of
	// its own carrying land_link() (the link of the step it would have joined, so a linked undo still pairs it)
	// rather than refused.
	virtual bool lands_alone() const { return false; }
	virtual uint64_t land_link() const { return 0; }
};

// How a raster's file reads into planes and a side state, and writes back (one per form: a terrain's .cpt
// heights, a heightmap image, a texture's form), chosen per file at its read.
class RasterCodec {
public:
	virtual ~RasterCodec();
	// The file's bytes (decoded as the game's loader decodes a stored file) into `out`, with its source
	// findings; false, with `error`, when they do not read.
	virtual bool read(const std::vector<uint8_t> &decoded, RasterState &out, std::vector<SourceIssue> &issues,
	                  Diagnostic &error) const = 0;
	// The bytes a Save writes of `state`, or the issues that keep it from writing.
	virtual SerializeResult write(const RasterState &state) const = 0;
	// What a Save changes beyond the edits, in plain words ("" none: DocumentBase::save_words).
	virtual std::string save_words(const RasterState &state) const;
};

// The codec that reads a file, by its name and decoded bytes (null: none reads it).
using RasterCodecPicker = std::shared_ptr<const RasterCodec> (*)(const std::string &path,
                                                                  const std::vector<uint8_t> &decoded);

// A document of planes of texels (ADR 0046 S25, the raster core: a terrain's heights, a painted map), edited
// by Apply edits whose payload is a RasterEditPayload, one undoable step a batch (a gesture's batches one
// step), its history of tile swaps under a byte budget; it reads and writes its file through the codec its
// picker chose at the read. changes_since answers regions of tiles (each plane's rows of changed tiles as
// runs) and its side state's changed parts, or cannot say across a whole change or another load.
class RasterDocument : public DocumentBase {
public:
	explicit RasterDocument(RasterCodecPicker pick, HistoryBudget budget = {});

	const RasterState &state() const { return state_; }
	const RasterPlane &plane(size_t index) const { return state_.planes[index]; }
	size_t plane_count() const { return state_.planes.size(); }
	const RasterSideState *side() const { return state_.side.get(); }
	const RasterCodec *codec() const { return codec_.get(); }
	// Whether a payload marked amends() would land now (RasterHistory::can_amend).
	bool can_amend() const { return history_.can_amend(); }

	// The edit carrying `payload` (gesture: the stroke it belongs to, 0 none).
	static Edit edit(std::shared_ptr<const RasterEditPayload> payload, uint64_t gesture = 0);

	void end_edit_group() override { history_.end_edit_group(); }
	bool dirty() const override { return history_.dirty(); }
	bool can_undo() const override { return history_.can_undo(); }
	bool can_redo() const override { return history_.can_redo(); }
	uint64_t revision() const override { return history_.revision(); }
	size_t history_bytes() const override { return history_.bytes(); }
	StepMark undo_mark() const override { return history_.undo_mark(); }
	StepMark redo_mark() const override { return history_.redo_mark(); }
	bool holds_link(uint64_t link) const override { return history_.holds_link(link); }
	bool changes_since(uint64_t load_generation, uint64_t revision, ChangeSet &out) const override;
	SerializeResult serialize() const override;
	std::string save_words() const override;
	std::unique_ptr<DocumentBase> snapshot() const override;
	const RasterDocument *as_raster() const override { return this; }
	RasterDocument *as_raster() override { return this; }

protected:
	RasterDocument(const RasterDocument &other);

	bool apply_edits(const std::vector<Edit> &edits, Diagnostic &error) override;
	void undo_step() override { history_.undo(state_); }
	void redo_step() override { history_.redo(state_); }
	bool cancel_gesture_step(uint64_t gesture) override { return history_.drop_open(gesture, state_); }
	bool read_source(const std::vector<uint8_t> &decoded, bool adopt, std::vector<SourceIssue> &issues,
	                 Diagnostic &error) override;
	void on_saved() override;
	// The codec that reads the file (the picker's, by default): a type whose read takes a setting the session
	// hands it (a heightmap's white) picks its own.
	virtual std::shared_ptr<const RasterCodec> pick_codec(const std::string &path, const std::vector<uint8_t> &decoded) const {
		return pick_ ? pick_(path, decoded) : nullptr;
	}

private:
	RasterCodecPicker pick_ = nullptr;
	std::shared_ptr<const RasterCodec> codec_;
	RasterState state_;
	RasterHistory history_;
};

} // namespace opennova::editor
