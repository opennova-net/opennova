#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/graph/graph_edge.h>
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
// document and writes back byte for byte (the chunks' audio areas held as the bank's encoder left them). A stream's
// audio is made from a wave (MusicStreamAudioEdit, the music_stream_audio request), one undo step.

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

// The bank as the game reads it (sbf::SbfFile's model): its channel format and its streams. The header's version and
// reserved words and each chunk's reserved pair and tail, which the game never reads, are the writer's constants.
struct MusicBankRow : TableRow {
	uint32_t flags = sbf::SBF_FLAGS_BYTE_PAIRED_STEREO;
	std::vector<MusicBankStream> streams;

	MusicBankRow();
	std::shared_ptr<Node> clone() const override { return std::make_shared<MusicBankRow>(*this); }
	std::string name() const override { return "Bank"; }
	RecordHandle record() const override;
	size_t footprint() const override;
};

const RecordTable &music_bank_table();

// A stream's audio a music bank takes as one edit (an Apply edit's payload, "music_bank.stream_audio", on a stream's
// address): its new chunks, made from a wave (music_stream_audio_from_wave), and what they were made from in words.
struct MusicStreamAudioEdit final : EditPayload {
	std::shared_ptr<const std::vector<sbf::SbfChunk>> chunks;
	std::string words;
	const char *token() const override { return "music_bank.stream_audio"; }
};

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
	// The stream at the place `place` writes (a whole number, as the music script names a stream: the game plays a
	// stream by its place, never its name): -1 for none.
	int stream_index(const std::string &place) const;

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	bool accept_step(const EditStep &step, const StagedRows &rows, StepRefusal &refusal) const override;
	// A MusicStreamAudioEdit on a stream: its chunks replaced by the payload's (shared, never copied); the same audio
	// changes nothing.
	bool apply_payload(Node &row, const NodeAddress &address, const EditPayload &payload,
	                   std::shared_ptr<const FileState> &state, const IdAllocator &allocate, bool &changed,
	                   std::string &error) override;
};

// A wave's audio as a stream's chunks (the music_stream_audio request): its samples made byte-paired stereo at 22050 a
// second as the stream plays them (lwf::wave_stereo_pcm16: one channel on both sides, two kept, another rate
// resampled as the wave importer resamples), the wave's full scale the stream's (an offset byte's whole range at
// shift 0, which the decoder sounds at +-127 * 256), encoded chunk by chunk (sbf::sbf_encode_stream).
// `out.words` says what it was made from ("tone.wav (16-bit PCM, mono, 44100 Hz: the channel on both sides, resampled to 22050 Hz),
// 2.0 s"). False, with `why`, for a wave that does not read, holds no sample, or has more than two channels.
bool music_stream_audio_from_wave(const std::string &wave_name, const std::vector<uint8_t> &wave, MusicStreamAudioEdit &out,
                                  std::string &why);

// A new stream's name from the wave it is made from: the file's stem upper case, what is not plain ASCII an
// underscore, its first 16 characters (the index's slot).
std::string music_stream_name_of(const std::string &wave_name);

// The batch that gives a stream `audio`, one undo step: an Apply on the stream at `place`; or, `place` -1, a new
// stream added at the end of the bank and its audio set on what the Add made (batch_made). `name` (not empty): the
// stream's name set in the same step.
std::vector<Edit> music_stream_audio_edits(const MusicBankDocument &bank, int place, const std::string &name,
                                           std::shared_ptr<const MusicStreamAudioEdit> audio);

// The stream at `place` of the bank `bytes` hold as the preview player sounds it: its chunks decoded (each byte a
// sample at its channel's shift, left and right in turn [orig: AudioChannel_ComputeMixCoefficients @ 0x7BD4B0;
// Audio_SubmitStereoSampleSplit @ 0x7BD252]) as interleaved 16-bit stereo at 22050 a second. False, with `error`, for
// a bank that does not read or a place it has no stream at.
bool music_stream_pcm(const std::vector<uint8_t> &bytes, int place, lwf::WavPcm &out, std::string &error);

// A stream's length in seconds, as the game streams it: its valid bytes, a left and a right sample a pair, at 22050
// pairs a second [orig: Audio_StreamNextChunk @ 0x4ED7D0].
double music_stream_seconds(const MusicBankStream &stream);

// The streams a music script plays (DocumentType::record_references): each stream a symbol of its place ("0",
// "1", ...), scoped to the bank's file name upper case ("GAMEMUS.SBF"), as the script's play names it [orig:
// AudioVM_StartSound @ 0x671FF0 -> AudioVM_SelectStream @ 0x671BC0, the bank's stream by that index].
void music_bank_references(const Document &document, Extracted &out);

// The music bank type's validator (DocumentType::validate_file): its source findings (a layout the writer lays out
// otherwise) and a stream of no audio. A stream's name is no finding's: the game never reads one.
std::vector<Diagnostic> validate_music_bank_file(const DocumentBase &document);

enum class MusicBankFinding { InvalidInput, IgnoredInput, StreamSilent, kCount };
const FindingCodeRow &finding_code(MusicBankFinding code);
FindingTable music_bank_finding_codes();

} // namespace opennova::editor
