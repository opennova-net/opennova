// The music bank document (music_bank_document.h): the bank's table over its header and its streams, its parse
// through the engine's reader and its save through the engine's writer from the row alone, and its findings.
#include "music_bank_document.h"

#include <cctype>
#include <cmath>
#include <cstdio>

#include <base/io/fixed.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/model/staged_rows.h>
#include <editor/project/project_files.h>
#include <formats/lwf/wav_source.h>

namespace opennova::editor {

using namespace opennova::sbf;

namespace {

constexpr NodeKind kBank = node_kind(MusicBankKind::Bank);
constexpr NodeKind kStream = node_kind(MusicBankKind::Stream);

MusicBankRow &bank_of(const RecordHandle &r) { return r.as<MusicBankRow>(); }
MusicBankStream &stream_of(const RecordHandle &r) { return r.as<MusicBankStream>(); }

FieldSchema schema_of(const char *id, FieldType type, const char *label, const char *description) {
	FieldSchema schema;
	schema.id = id;
	schema.type = type;
	schema.label = label;
	schema.description = description;
	return schema;
}

size_t valid_bytes(const MusicBankStream &stream) {
	size_t bytes = 0;
	if (stream.chunks)
		for (const SbfChunk &chunk : *stream.chunks) bytes += chunk.audio.size();
	return bytes;
}

// One chunk of silence: what a new stream holds until it is given audio (sbf_encode_stream of no sample).
std::shared_ptr<const std::vector<SbfChunk>> silence() {
	static const std::shared_ptr<const std::vector<SbfChunk>> chunks = [] {
		const SbfStream stream = sbf_encode_stream(std::string(), nullptr, 0);
		return std::make_shared<const std::vector<SbfChunk>>(stream.chunks);
	}();
	return chunks;
}

RecordTable make_table() {
	using RF = LabelledField;
	// --- the bank ---------------------------------------------------------------------------------
	TableKind bank(RecordKindRow{kBank, "bank", "Bank", "", true});
	{
		FieldSchema flags = schema_of("flags", FieldType::Integer, "Channel format",
				"The header's channel-format word: the game opens a bank whose word is 2 at the most [orig: "
				"AudioVM_OpenContextFile @ 0x672160, Sbf_OpenFile_Gamemus @ 0x4ED6C0]; every shipped bank says 1, byte-paired "
				"stereo.");
		flags.read_only = true;
		bank.field(RF{flags, {[](const RecordHandle &r, Value &out) { return out = int64_t(bank_of(r).flags), true; }}});
		FieldSchema streams = schema_of("stream_count", FieldType::Integer, "Streams",
				"The streams the bank holds: the music script plays one by its place among them.");
		streams.read_only = true;
		bank.field(RF{streams, {[](const RecordHandle &r, Value &out) {
			              return out = int64_t(bank_of(r).streams.size()), true;
		              }}});
	}
	TableList streams;
	streams.spec = Document::CollectionSpec{kStream, "Streams", "name", false, Applicability::Reads, 0};
	streams.spec.first_number = 0;
	streams.ops = vector_list<MusicBankRow, MusicBankStream>(
			kStream, [](MusicBankRow &b) -> std::vector<MusicBankStream> & { return b.streams; },
			[](const MusicBankRow &, size_t) {
				MusicBankStream fresh;
				fresh.name = "NEWSTREAM";
				fresh.chunks = silence();
				return fresh;
			});
	bank.list(std::move(streams));

	// --- a stream ---------------------------------------------------------------------------------
	TableKind stream(RecordKindRow{kStream, "stream", "Stream", "", false});
	{
		FieldSchema name = schema_of("name", FieldType::Text, "Name",
				"The stream's name in the index, 16 characters at the most. The game never reads it: the music script "
				"plays a stream by its place [orig: AudioVM_Op_Play @ 0x672CB0].");
		name.width = SBF_NAME_SIZE;
		stream.field(RF{name,
		                {[](const RecordHandle &r, Value &out) { return out = stream_of(r).name, true; },
		                 [](const RecordHandle &r, const Value &v, std::string &e) {
			                 const auto *text = std::get_if<std::string>(&v);
			                 if (!text || text->size() > SBF_STREAM_NAME_MAX) {
				                 e = "A stream's name is a text of 16 characters at the most.";
				                 return false;
			                 }
			                 for (const unsigned char c : *text)
				                 if (c < 0x20 || c > 0x7E) {
					                 e = "A stream's name is plain ASCII.";
					                 return false;
				                 }
			                 stream_of(r).name = *text;
			                 return true;
		                 }}});
		FieldSchema seconds = schema_of("seconds", FieldType::Real, "Length",
				"How long the stream plays: its valid bytes, a left and a right sample a pair, at 22050 pairs a second "
				"[orig: Audio_StreamNextChunk @ 0x4ED7D0].");
		seconds.read_only = true;
		seconds.unit = "s";
		stream.field(RF{seconds, {[](const RecordHandle &r, Value &out) { return out = music_stream_seconds(stream_of(r)), true; }}});
		FieldSchema chunks = schema_of("chunks", FieldType::Integer, "Chunks",
				"The blocks the game reads one at a time as it streams [orig: Audio_StreamNextChunk @ 0x4ED7D0].");
		chunks.read_only = true;
		stream.field(RF{chunks, {[](const RecordHandle &r, Value &out) {
			                const MusicBankStream &s = stream_of(r);
			                return out = int64_t(s.chunks ? s.chunks->size() : 0), true;
		                }}});
		FieldSchema block = schema_of("block_size", FieldType::Integer, "Block size",
				"The bytes the game reads a block, its 8-byte header among them [orig: Audio_StreamNextChunk @ 0x4ED7D0]; "
				"every shipped stream says 4104.");
		block.read_only = true;
		block.unit = "bytes";
		stream.field(RF{block, {[](const RecordHandle &r, Value &out) { return out = int64_t(stream_of(r).block_size), true; }}});
		FieldSchema hint = schema_of("sample_length_hint", FieldType::Integer, "Length word",
				"The entry's last word, which the game copies to its scheduler [orig: Sbf_StartEntry @ 0x4ED910]; every "
				"shipped stream says 0.");
		hint.read_only = true;
		stream.field(RF{hint, {[](const RecordHandle &r, Value &out) {
			              return out = int64_t(stream_of(r).sample_length_hint), true;
		              }}});
	}
	return RecordTable({std::move(bank), std::move(stream)});
}

const MusicBankRow &bank_row_of(const Node &node) { return static_cast<const MusicBankRow &>(node); }

size_t stream_place(const MusicBankRow &bank, NodeId id) {
	if (bank.ids.lists.empty()) return SIZE_MAX;
	const std::vector<RecordIds> &ids = bank.ids.lists[0];
	for (size_t i = 0; i < ids.size(); ++i)
		if (ids[i].id == id) return i;
	return SIZE_MAX;
}

} // namespace

const RecordTable &music_bank_table() {
	static const RecordTable table = make_table();
	return table;
}

bool music_stream_pcm(const std::vector<uint8_t> &bytes, int place, lwf::WavPcm &out, std::string &error) {
	SbfFile bank;
	if (!sbf_read_bank(bytes.data(), bytes.size(), bank, error)) return false;
	if (place < 0 || size_t(place) >= bank.streams.size()) {
		error = "the bank has no stream " + std::to_string(place);
		return false;
	}
	const std::vector<int16_t> samples = sbf_decode_stream(bank.streams[size_t(place)]);
	out = lwf::WavPcm();
	out.channels = SBF_CHANNELS;
	out.sample_rate = SBF_SAMPLE_RATE;
	out.pcm16.resize(samples.size() * 2);
	for (size_t i = 0; i < samples.size(); ++i) {
		out.pcm16[i * 2] = uint8_t(uint16_t(samples[i]) & 0xFF);
		out.pcm16[i * 2 + 1] = uint8_t(uint16_t(samples[i]) >> 8);
	}
	out.loader_samples = uint32_t(samples.size() / SBF_CHANNELS);
	out.loader_pitch_q16 = io::kFp16OneInt;
	return true;
}

double music_stream_seconds(const MusicBankStream &stream) {
	return double(valid_bytes(stream)) / 2.0 / double(SBF_SAMPLE_RATE);
}

// --- the row -------------------------------------------------------------------------------------

MusicBankRow::MusicBankRow() { kind = kBank; }

RecordHandle MusicBankRow::record() const { return {kBank, const_cast<MusicBankRow *>(this)}; }

// The chunks are shared from one version of the row to the next and never edited in place (a stream's new audio
// replaces its chunks whole, MusicStreamAudioEdit): a step holds them once, so they count for nothing here.
size_t MusicBankRow::footprint() const {
	size_t bytes = sizeof(*this) + footprint_of(streams) + ids_footprint();
	for (const MusicBankStream &stream : streams) bytes += footprint_of(stream.name);
	return bytes;
}

// --- the document ----------------------------------------------------------------------------------

const MusicBankRow *MusicBankDocument::bank_row() const {
	for (const auto &node : rows())
		if (node && node->kind == kBank) return &bank_row_of(*node);
	return nullptr;
}

std::string MusicBankDocument::record_title(const NodeAddress &address) const {
	const Node *node = row(address.row);
	if (!node) return std::string();
	if (!address.child) return "Bank";
	const MusicBankRow &bank = bank_row_of(*node);
	const size_t place = stream_place(bank, address.child);
	if (place == SIZE_MAX || place >= bank.streams.size()) return record_name(address);
	return std::to_string(place) + ": " + bank.streams[place].name;
}

SbfFile MusicBankDocument::bank() const {
	SbfFile out;
	const MusicBankRow *row = bank_row();
	if (!row) return out;
	out.flags = row->flags;
	for (const MusicBankStream &stream : row->streams) {
		SbfStream made;
		made.name = stream.name;
		made.block_size = stream.block_size;
		made.sample_length_hint = stream.sample_length_hint;
		if (stream.chunks) made.chunks = *stream.chunks;
		out.streams.push_back(std::move(made));
	}
	return out;
}

int MusicBankDocument::stream_index(const std::string &place) const {
	const MusicBankRow *row = bank_row();
	if (!row) return -1;
	const std::optional<int> number = strutil::parse_int(place);
	if (number && *number >= 0 && size_t(*number) < row->streams.size() && std::to_string(*number) == place) return *number;
	return -1;
}

bool MusicBankDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                              std::shared_ptr<const FileState> &, std::vector<SourceIssue> &issues, Diagnostic &error) {
	SbfFile read;
	SbfFileLayout layout;
	std::string message;
	if (!sbf_read_bank(bytes.data(), bytes.size(), read, message, &layout)) {
		error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error,
		                     "The music bank could not be read: " + message + ".", path());
		return false;
	}
	auto row = std::make_shared<MusicBankRow>();
	row->flags = read.flags;
	for (SbfStream &stream : read.streams) {
		MusicBankStream held;
		held.name = std::move(stream.name);
		held.block_size = stream.block_size;
		held.sample_length_hint = stream.sample_length_hint;
		held.chunks = std::make_shared<const std::vector<SbfChunk>>(std::move(stream.chunks));
		row->streams.push_back(std::move(held));
	}
	// What a save lays out otherwise: the header's never-read words the writer's, the streams one after another in the
	// index's order, nothing past the last, the names zero-padded, every chunk a whole block under the writer's reserved
	// pair and tail.
	if (layout.header_words_other)
		issues.push_back({false, 0, std::string(), std::string(),
		                  "The header's version or reserved word, which the game never reads, is not the writer's: a save "
		                  "writes the writer's."});
	if (!layout.packed)
		issues.push_back({false, 0, std::string(), std::string(),
		                  "The streams are not laid out one after another in the index's order: a save writes them so."});
	if (layout.trailing_bytes)
		issues.push_back({false, 0, std::string(), std::string(),
		                  std::to_string(layout.trailing_bytes) + " bytes follow the last stream, which the game never reads: a "
		                                                          "save leaves them out."});
	if (layout.names_with_tails)
		issues.push_back({false, 0, std::string(), std::string(),
		                  std::to_string(layout.names_with_tails) +
		                          " names hold bytes after their terminator, which the game never reads: a save writes zeros."});
	if (layout.reserved_other)
		issues.push_back({false, 0, std::string(), std::string(),
		                  std::to_string(layout.reserved_other) +
		                          " chunks carry another pair of never-read header bytes than the writer's: a save writes "
		                          "the writer's pair."});
	if (layout.tails_other)
		issues.push_back({false, 0, std::string(), std::string(),
		                  std::to_string(layout.tails_other) +
		                          " chunks hold never-read bytes past their audio other than the writer's tail: a save writes "
		                          "the writer's."});
	if (layout.short_chunks)
		issues.push_back({false, 0, std::string(), std::string(),
		                  std::to_string(layout.short_chunks) +
		                          " chunks are shorter than their block, or count more audio than it holds: a save writes each "
		                          "a whole block of the audio it holds."});
	shape(*row);
	rows.push_back(std::move(row));
	return true;
}

SerializeResult MusicBankDocument::serialize() const {
	SerializeResult result;
	std::vector<uint8_t> bytes;
	std::string error;
	if (!bank_row() || !sbf_write_bank(bank(), bytes, error)) {
		result.issues.push_back({true, 0, std::string(), std::string(),
		                         "The music bank could not be written: " + (error.empty() ? std::string("it holds no bank") : error) + "."});
		return result;
	}
	result.text.assign(bytes.begin(), bytes.end());
	return result;
}

std::string MusicBankDocument::save_words() const {
	return "A save writes the bank from its header and its streams, one after another in their order, each chunk a whole "
	       "block: its audio, then the bytes the bank's encoder rule makes.";
}

std::shared_ptr<Node> MusicBankDocument::make_node(NodeKind, NodeId, const std::vector<std::shared_ptr<const Node>> &,
                                                   std::string &error) {
	error = "A music bank keeps its one row; add streams inside it.";
	return nullptr;
}

bool MusicBankDocument::accept_step(const EditStep &, const StagedRows &staged, StepRefusal &refusal) const {
	if (staged.rows().size() == 1) return true;
	refusal.message = "A music bank keeps its one row.";
	return false;
}

namespace {

bool same_audio(const std::vector<SbfChunk> &a, const std::vector<SbfChunk> &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i].scale_a != b[i].scale_a || a[i].scale_b != b[i].scale_b || a[i].audio != b[i].audio) return false;
	return true;
}

} // namespace

bool MusicBankDocument::apply_payload(Node &row, const NodeAddress &address, const EditPayload &payload,
                                      std::shared_ptr<const FileState> &, const IdAllocator &, bool &changed,
                                      std::string &error) {
	const auto *audio = dynamic_cast<const MusicStreamAudioEdit *>(&payload);
	if (!audio || !audio->chunks) {
		error = "A music bank takes a stream's audio, and nothing else, as a change of its own.";
		return false;
	}
	if (row.kind != kBank || address.kind != kStream || !address.child) {
		error = "A stream's audio is set on a stream.";
		return false;
	}
	MusicBankRow &bank = static_cast<MusicBankRow &>(row);
	const size_t place = stream_place(bank, address.child);
	if (place >= bank.streams.size()) {
		error = "The stream no longer exists.";
		return false;
	}
	MusicBankStream &stream = bank.streams[place];
	changed = !stream.chunks || (stream.chunks != audio->chunks && !same_audio(*stream.chunks, *audio->chunks));
	if (changed) stream.chunks = audio->chunks;
	return true;
}

bool music_stream_audio_from_wave(const std::string &wave_name, const std::vector<uint8_t> &wave, MusicStreamAudioEdit &out,
                                  std::string &why) {
	// The wave's full scale is the stream's: an offset byte's 127 steps at shift 0, which the decoder sounds at
	// +-127 * 128 (sbf_decode_sample: half the 16-bit range), so a wave at full scale streams at the loudest a stream
	// plays, never clipped.
	constexpr float kStreamFullScale = float(127 * 128) / 32767.0f;
	std::vector<int16_t> pcm;
	lwf::WaveFormat format;
	if (!lwf::wave_stereo_pcm16(wave, SBF_SAMPLE_RATE, kStreamFullScale, pcm, why, &format)) return false;
	const SbfStream stream = sbf_encode_stream(std::string(), pcm.data(), pcm.size());
	out.chunks = std::make_shared<const std::vector<SbfChunk>>(stream.chunks);
	// What was made of it: its channels and its rate as the stream plays them.
	std::string made;
	if (format.channels == 1) made = "the channel on both sides";
	if (format.rate != uint32_t(SBF_SAMPLE_RATE))
		made += (made.empty() ? "" : ", ") + std::string("resampled to ") + std::to_string(SBF_SAMPLE_RATE) + " Hz";
	char seconds[32];
	std::snprintf(seconds, sizeof(seconds), "%.1f s", double(pcm.size() / SBF_CHANNELS) / double(SBF_SAMPLE_RATE));
	out.words = basename_of(wave_name) + " (" + lwf::wave_format_words(format) + (made.empty() ? "" : ": " + made) + "), " +
	            seconds;
	return true;
}

std::string music_stream_name_of(const std::string &wave_name) {
	std::string stem = basename_of(wave_name);
	const size_t dot = stem.find_last_of('.');
	if (dot != std::string::npos && dot > 0) stem.resize(dot);
	std::string name;
	for (const unsigned char c : stem) {
		if (name.size() == SBF_STREAM_NAME_MAX) break;
		name.push_back(c < 0x20 || c > 0x7E ? '_' : char(std::toupper(c)));
	}
	return name;
}

std::vector<Edit> music_stream_audio_edits(const MusicBankDocument &bank, int place, const std::string &name,
                                           std::shared_ptr<const MusicStreamAudioEdit> audio) {
	std::vector<Edit> edits;
	const MusicBankRow *row = bank.bank_row();
	if (!row) return edits;
	NodeAddress stream{row->id, kStream, 0};
	if (place < 0) {
		// A new stream at the end, named in its Add; its audio set on what the Add made.
		Edit add;
		add.operation = EditOperation::Add;
		add.address = stream;
		if (!name.empty()) {
			add.field = "name";
			add.value = name;
		}
		edits.push_back(std::move(add));
		stream.child = batch_made(0);
	} else {
		if (row->ids.lists.empty() || size_t(place) >= row->ids.lists[0].size()) return {};
		stream.child = row->ids.lists[0][size_t(place)].id;
		if (!name.empty()) {
			Edit rename;
			rename.address = stream;
			rename.field = "name";
			rename.value = name;
			edits.push_back(std::move(rename));
		}
	}
	Edit apply;
	apply.operation = EditOperation::Apply;
	apply.address = stream;
	apply.payload = std::move(audio);
	edits.push_back(std::move(apply));
	return edits;
}

// --- the findings ------------------------------------------------------------------------------------

namespace {

constexpr FindingCodeEntry<MusicBankFinding> kFindingEntries[] = {
	{ MusicBankFinding::InvalidInput, { "music_bank.invalid_input", FindingFix::None, nullptr, true } },
	{ MusicBankFinding::IgnoredInput, { "music_bank.ignored_input", FindingFix::Rewrite, kRewriteDropsIgnoredInput } },
	// A stream of no audio: the game streams its block and plays nothing [orig: Audio_StreamNextChunk @ 0x4ED7D0]; it
	// refuses nothing: listed.
	{ MusicBankFinding::StreamSilent, listed_code("music_bank.stream_silent") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(MusicBankFinding::kCount),
              "every MusicBankFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
              "the music bank's rows follow MusicBankFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::MusicBanks);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const FindingCodeRow &finding_code(MusicBankFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable music_bank_finding_codes() { return {kFindingRows.data(), kFindingRows.size()}; }

std::vector<Diagnostic> validate_music_bank_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *bank_document = dynamic_cast<const MusicBankDocument *>(&document);
	if (!bank_document) return findings;
	source_issue_findings(*bank_document, finding_code(MusicBankFinding::InvalidInput),
	                      finding_code(MusicBankFinding::IgnoredInput), findings);
	if (document.blocked()) return findings;
	const MusicBankRow *bank = bank_document->bank_row();
	if (!bank || bank->ids.lists.empty()) return findings;
	const std::vector<RecordIds> &ids = bank->ids.lists[0];
	const auto add = [&](size_t place, DiagnosticSeverity severity, MusicBankFinding code, const char *field,
	                     const std::string &message) {
		Diagnostic d = make_finding(code, severity, message, document.path(), field);
		d.row_id = bank->id;
		d.child_id = place < ids.size() ? ids[place].id : 0;
		d.record_kind = kStream;
		d.record = place < bank->streams.size() ? bank->streams[place].name : std::string();
		findings.push_back(std::move(d));
	};
	for (size_t i = 0; i < bank->streams.size(); ++i) {
		const MusicBankStream &stream = bank->streams[i];
		if (valid_bytes(stream) == 0)
			add(i, DiagnosticSeverity::Info, MusicBankFinding::StreamSilent, "seconds",
			    "Stream " + std::to_string(i) + " holds no audio: playing it plays nothing [orig: Audio_StreamNextChunk @ "
			    "0x4ED7D0].");
	}
	return findings;
}

void music_bank_references(const Document &document, Extracted &out) {
	const auto *bank_document = dynamic_cast<const MusicBankDocument *>(&document);
	const MusicBankRow *bank = bank_document ? bank_document->bank_row() : nullptr;
	if (!bank || bank->ids.lists.empty()) return;
	std::string scope = document.path();
	const size_t slash = scope.find_last_of("/\\");
	if (slash != std::string::npos) scope.erase(0, slash + 1);
	scope = strutil::to_upper(scope);
	const std::vector<RecordIds> &ids = bank->ids.lists[0];
	for (size_t place = 0; place < bank->streams.size() && place < ids.size(); ++place) {
		const NodeAddress address{bank->id, kStream, ids[place].id};
		GraphSymbol symbol;
		symbol.kind = ReferenceKind::MusicStream;
		symbol.display = std::to_string(place);
		symbol.name = symbol.display; // NameCase::Exact
		symbol.value = bank->streams[place].name;
		symbol.file = document.path();
		symbol.record = document.record_path(address);
		symbol.locator = document.locator(address);
		symbol.address = address;
		symbol.scope = scope;
		out.symbols.push_back(std::move(symbol));
	}
}

} // namespace opennova::editor
