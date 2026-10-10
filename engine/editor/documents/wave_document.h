#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/io/json.h>
#include <editor/model/diagnostic.h>
#include <editor/model/document_base.h>
#include <editor/model/edit_history.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/value.h>
#include <formats/lwf/wav_source.h>

namespace opennova::editor {

// A whole wave a wave document takes as one edit (an Apply edit's payload, "wave.bytes"): the file's new bytes,
// made by a wave operation through the engine's converter (wave_operations below), and the step's words.
struct WaveBytesEdit final : EditPayload {
	std::vector<uint8_t> bytes;
	std::string words;
	const char *token() const override { return "wave.bytes"; }
};

// A wave as a document (ADR 0046, round S23 lane A): a .wav of the project, held as the file stores it and read as
// the game's loader reads it [orig: Audio_LoadWavFileFromArchive @ 0x766480] (docs/audio/lwf-dbf-sound-re.md "The
// wave loader's rules"): its format, length, peak and RMS, its picture (the loudest sample of each of a few hundred
// stretches), and whether the game plays it and why not (lwf::wave_retail_check: one channel, no LIST ahead of the
// data, the sizes and chunks the loader's walk takes; D-SND-33). It takes whole waves (an Apply edit of a
// WaveBytesEdit, one undo step each: a trim, a normalise), each written in the form the game takes through the
// engine's converter (lwf::convert_wave: one channel, 8 or 16 bits, fmt and data alone), its history the file's
// versions under the history's byte budget, and Save writes the version it is at.
class WaveDocument final : public DocumentBase {
public:
	explicit WaveDocument(HistoryBudget budget = {}) : budget_(budget) {}
	WaveDocument(const WaveDocument &other) = default;
	// The file's bytes as the document holds them now, and what they are (read as they are first asked for).
	const std::vector<uint8_t> &bytes() const;
	const lwf::WaveFacts &facts() const;
	void end_edit_group() override {}
	bool dirty() const override;
	bool can_undo() const override { return cursor_ > 0; }
	bool can_redo() const override { return cursor_ + 1 < versions_.size(); }
	uint64_t revision() const override;
	size_t history_bytes() const override;
	// The same state answers no change; any other cannot say (each is a whole wave).
	bool changes_since(uint64_t load_generation, uint64_t revision, ChangeSet &out) const override;
	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override;
	bool holds_bytes() const override { return true; }

protected:
	bool apply_edits(const std::vector<Edit> &edits, Diagnostic &error) override;
	void undo_step() override;
	void redo_step() override;
	bool read_source(const std::vector<uint8_t> &decoded, bool adopt, std::vector<SourceIssue> &issues,
	                 Diagnostic &error) override;
	void on_saved() override;

private:
	struct Version {
		std::shared_ptr<const std::vector<uint8_t>> bytes;
		uint64_t revision = 0;
	};
	void moved() { facts_.reset(); }
	HistoryBudget budget_;
	std::vector<Version> versions_;
	size_t cursor_ = 0;
	uint64_t next_revision_ = 1, saved_revision_ = 0;
	mutable std::shared_ptr<const lwf::WaveFacts> facts_;
};

// The whole-wave edits of a wave document (the wave_operation request): each makes the file anew from its samples,
// written in the form the game's loader takes (one channel, the source's 8 bits kept and anything else 16, its rate
// kept, fmt and data alone: lwf::convert_wave), and is one undo step. No sample editor: no cut inside it, no fade.
// - trim: `start` and `end` (seconds, end past the last frame the end; at least one kept);
// - normalise: `peak` (0..1 of full scale, 1 when left out): every sample scaled so the loudest is that.
enum class WaveOperationKind : uint8_t { Trim, Normalise, kCount };
const char *wave_operation_token(WaveOperationKind kind);
bool wave_operation_kind(const std::string &token, WaveOperationKind &out);
struct WaveOperation {
	WaveOperationKind kind = WaveOperationKind::Trim;
	std::vector<std::pair<std::string, std::string>> params;
};
// The wave `bytes` made anew by `operation`: its bytes in `out` and the step's words ("Trimmed to 0.25 s ..."); false
// with `why` in a modder's words.
bool apply_wave_operation(const std::vector<uint8_t> &bytes, const WaveOperation &operation, std::vector<uint8_t> &out,
                          std::string &words, std::string &why);

// The wave type's row (documents/document_types.cpp): its documents; its file's own finding, the one place a wave
// the game's loader refuses is found (asset.wave_unplayable: wave_retail_check, the loader's own walk); no fields;
// its finding codes (none of its own); its content on the wire, the document query's `wave`: its format, frames,
// seconds, peak, RMS, picture and the loader's verdict.
std::unique_ptr<DocumentBase> make_wave_document();
std::vector<Diagnostic> validate_wave_file(const DocumentBase &document);
const std::vector<FieldSchema> &wave_fields(NodeKind kind);
FindingTable wave_finding_codes();
io::JsonValue wave_content_json(const DocumentBase &document);
// The bins of a wave's picture on the wire and in its view.
inline constexpr size_t kWavePictureBins = 400;

} // namespace opennova::editor
