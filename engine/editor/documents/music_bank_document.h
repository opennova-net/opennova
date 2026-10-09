#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/table_document.h>
#include <formats/lwf/wav_pcm.h>
#include <formats/sbf/sbf.h>

namespace opennova::editor {

// A music bank (ADR 0046, round S23 lane A): a `.sbf`, the music the game streams loose beside its archives
// (MENUMUS.SBF, GAMEMUS.SBF; an expansion's M<n>.sbf and G<n>.sbf) [orig: AudioVM_InitMenuMusicStreaming @
// 0x56aa60; Expansion_LoadAssets @ 0x4a4906, @ 0x4a4936], read and written through the engine's own reader and
// writer (sbf::sbf_read_bank, sbf::sbf_write_bank: the writer from scratch, ADR 0003) over the engine's own record,
// sbf::SbfFile (docs/audio/mus-sbf-re.md). One row, the bank, its header's words; and its streams in the index's
// order, each its name, its block size and its chunks of byte-paired stereo at 22050 a second. The music script
// beside it plays a stream by its place in that order [orig: AudioVM_Op_Play @ 0x672CB0; AudioVM_StartSound @
// 0x671ff0], so a stream moved or removed changes what the script plays. Every shipped bank reads into the
// document and writes back byte for byte (the chunks' audio areas held as the bank's encoder left them).

enum class MusicBankKind : NodeKind { Bank = 0, Stream = 1 };
constexpr NodeKind node_kind(MusicBankKind kind) { return static_cast<NodeKind>(kind); }

// The bank row: the engine's record, its streams' chunks shared from one stream to its copies (an edit of a
// stream's name or hint never changes its audio).
struct MusicBankStream {
	std::string name;
	uint32_t block_size = sbf::SBF_CHUNK_TOTAL;
	uint32_t sample_length_hint = 0;
	std::shared_ptr<const std::vector<sbf::SbfChunk>> chunks;
};

struct MusicBankRow : TableRow {
	uint32_t version = sbf::SBF_VERSION_DEFAULT;
	uint32_t flags = sbf::SBF_FLAGS_BYTE_PAIRED_STEREO;
	uint32_t reserved = 0;
	std::vector<MusicBankStream> streams;

	MusicBankRow();
	std::shared_ptr<Node> clone() const override { return std::make_shared<MusicBankRow>(*this); }
	std::string name() const override { return "Bank"; }
	RecordHandle record() const override;
	size_t footprint() const override;
};

const RecordTable &music_bank_table();

class MusicBankDocument : public TableDocument {
public:
	const RecordTable &table() const override { return music_bank_table(); }
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return music_bank_table().fields(kind); }
	// A stream by its place and name ("0: NULLS").
	std::string record_title(const NodeAddress &address) const override;
	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<MusicBankDocument>(*this); }
	std::string save_words() const override;

	const MusicBankRow *bank_row() const;
	// The bank as the engine holds it, made from the row (what serialize() writes).
	sbf::SbfFile bank() const;
	// The stream at `index` (its place), its name matched without case where `name` is given: -1 for none.
	int stream_index(const std::string &name_or_index) const;

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	bool accept_step(const EditStep &step, const StagedRows &rows, StepRefusal &refusal) const override;
};

bool is_music_bank_kind(AssetKind kind);

// The stream at `place` of the bank `bytes` hold as the preview player sounds it: its chunks decoded (each byte a
// sample at its channel's shift, left and right in turn [orig: AudioChannel_ComputeMixCoefficients @ 0x7BD4B0;
// Audio_SubmitStereoSampleSplit @ 0x7BD252]) as interleaved 16-bit stereo at 22050 a second. False, with `error`, for
// a bank that does not read or a place it has no stream at.
bool music_stream_pcm(const std::vector<uint8_t> &bytes, int place, lwf::WavPcm &out, std::string &error);

// A stream's length in seconds, as the game streams it: its valid bytes, a left and a right sample a pair, at 22050
// pairs a second [orig: Audio_StreamNextChunk @ 0x4ED7D0].
double music_stream_seconds(const MusicBankStream &stream);

// The music bank type's validator (DocumentType::validate_file): its source findings (a layout the writer lays out
// otherwise), two streams of one name (a lookup by name finds the first), a stream with no name, a stream of no
// audio.
std::vector<Diagnostic> validate_music_bank_file(const DocumentBase &document);

enum class MusicBankFinding { InvalidInput, IgnoredInput, NameRepeated, NameEmpty, StreamSilent, kCount };
const FindingCodeRow &finding_code(MusicBankFinding code);
FindingTable music_bank_finding_codes();

} // namespace opennova::editor
