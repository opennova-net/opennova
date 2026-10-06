#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/table_document.h>
#include <formats/lwf/lwf.h>

namespace opennova::editor {

// A sound bank (ADR 0046, the sound lane): a `.lwf`, the sets the game plays by name, each naming
// the waves it picks from (formats/lwf; docs/audio/lwf-dbf-sound-re.md). Its rows are the bank's
// two tables in the file's order, the waves first, the sets after them:
// - a wave is one of the bank's singles: the name the bank and a mission's dialog lines find it by
//   (the first of the name, without case) [orig: SoundBank_FindEntryByName @ 0x75bba0], the `.wav`
//   the game loads for it from the archives [orig: sub_75BC20 @ 0x75bc20, the path the filename
//   table gives slot by slot @ 0x75c700], and the volume a dialog line plays it at [orig:
//   Dialog_LoadAudioClip @ 0x44dd10 -> sub_75BE10 @ 0x75be10, the entry's byte +33];
// - a set is a Multi, found by its name across the loaded banks [orig: SoundBank_FindTriggerByName
//   @ 0x75be90]: its pitch and pitch jitter, the range a positional play of it is heard within, its
//   view gate, and its layers (at most 8 [orig: @ 0x75c648]), each a playlist that plays one of its
//   members (at most 8 [orig: @ 0x75c61b]) by its selection flags, each member a sndparm naming a
//   wave of the bank with its pitch, jitter, volume and volume ceiling.
// A member names its wave by the wave's name (ReferenceKind::BankWave, a symbol of this file), which
// the save turns into the index the format stores: the first wave of the name, as the bank's own
// lookups find one [orig: SoundBank_FindEntryByName @ 0x75bba0]. Every shipped bank names its waves
// uniquely and points each member at the first of its name, so a retail bank reads into the
// document and back with every member on the same wave. serialize() writes the bank from scratch
// through the engine's writer (lwf::encode_lwf, ADR 0003): the waves' file names as the filename
// table, no trigger record (no consumer reads one [orig: SoundBank_OpenFile @ 0x75cb7a]), every
// scratch word zero.

enum class SoundBankKind : NodeKind { Wave = 0, Set = 1, Layer = 2, Member = 3 };
constexpr NodeKind node_kind(SoundBankKind kind) { return static_cast<NodeKind>(kind); }

// The native records the bank's table describes (sound_bank_table in the .cpp).
struct BankMember {
	std::string wave;                                 // the wave's name in this bank
	uint32_t pitch = lwf::kPitchUnityQ16;             // Q16, 0x10000 = as recorded
	uint32_t pitch_jitter = 0;                        // Q16, added: (range * rand8) >> 8
	uint32_t volume = 255;                            // 0..255
	uint32_t ceiling = 255;                           // 0..255, the distance-scaled volume's ceiling
};
struct BankLayer {
	uint16_t falloff = 100;      // the radius the volume reaches nothing at, whole units
	uint16_t min_distance = 0;   // the proximity fade radius, whole units (0: none)
	uint32_t flags = lwf::kFlagHeading | lwf::kFlagInternal | lwf::kFlagExternal | lwf::kFlagRandom;
	std::vector<BankMember> members;
};
struct BankSet {
	std::string name;
	uint32_t pitch = lwf::kAuthoredSetPitchBase; // Q16; the tools write 0xFFFF
	uint32_t pitch_jitter = 0;
	uint32_t range = 10000;  // Multi dword 18: the positional cull range, whole units (every shipped set: 10000)
	uint32_t flags = 0;      // bit 0: a layer's view bit must match the listener's
	std::vector<BankLayer> layers;
};
struct BankWave {
	std::string name;
	std::string file;
	uint32_t volume = 255; // the entry's byte +33
};

// A row: a wave or a set by its kind (the other member unused).
struct SoundBankRow : TableRow {
	BankWave wave;
	BankSet set;

	explicit SoundBankRow(NodeKind kind);
	std::shared_ptr<Node> clone() const override { return std::make_shared<SoundBankRow>(*this); }
	std::string name() const override;
	RecordHandle record() const override;
	size_t footprint() const override;
};

const RecordTable &sound_bank_table();

class SoundBankDocument : public TableDocument {
public:
	const RecordTable &table() const override { return sound_bank_table(); }
	// A kind's fields without a document (DocumentType::fields, S13 V3).
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return sound_bank_table().fields(kind); }
	// A wave by its name and file, a set by its name, a layer by how it picks and how many members it
	// holds, a member by the wave it plays.
	std::string record_title(const NodeAddress &address) const override;
	SerializeResult serialize() const override;
	std::string save_words() const override;
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<SoundBankDocument>(*this); }

	// The bank as the engine reads it, made from the rows (what serialize() writes; the preview plays
	// a set from it). False, with the issues that keep it from being written (a member naming no wave
	// of the bank), when it cannot be made.
	bool bank(lwf::File &out, std::vector<SourceIssue> *issues = nullptr) const;
	// The rows of a kind, in order.
	std::vector<const SoundBankRow *> rows_of(SoundBankKind kind) const;
	// The set of `name` (without case, the first of the name, as the game finds one), null for none.
	const SoundBankRow *find_set(const std::string &name) const;

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	// A new wave (a name no wave has, no file yet) or a new set (a name no set has, no layer yet: one
	// batch adds the set, its layer and the layer's members, each naming what the one before it made).
	// A new layer is heard in both views and picks at random (BankLayer's defaults); a new member plays
	// the bank's first wave until one is named.
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	// The waves before the sets, as the file holds them: a wave goes among the waves, a set among the
	// sets, wherever it is asked to.
	size_t row_position(const Node &row, const std::vector<std::shared_ptr<const Node>> &rows,
	                    size_t position) const override;
	// A copy of a row named anew (the game finds the first of two of one name).
	void prepare_duplicate(Node &copy, const Node &original,
	                       const std::vector<std::shared_ptr<const Node>> &rows) const override;
	// A wave's name and a member's wave live in this bank (the scope its file's name gives), and so does a
	// set's name: a lookup naming no bank finds it as the game's search does, a menu's SOUND in its bank.
	void refine_field(const NodeAddress &address, FieldUse &use) const override;
	// A new member plays the bank's first wave.
	void after_add(Node &row, const ListChange &change, const RecordHandle &made) override;
};

bool is_sound_bank_kind(AssetKind kind);

// The scope a bank's own names live in (its waves'): its file name, upper case ("GAME.LWF").
std::string sound_bank_scope(const std::string &path);

// How a layer's flags pick its member, in words [orig: SoundBank_PlayTriggerEntries @ 0x75cd5c..0x75cdfc]:
// "in order" (0x10), "a random start, then in order" (0x80), else "at random" (0x08 never read).
const char *layer_selection_words(uint32_t flags);

// The sound bank type's validator over one bank (DocumentType::validate_file): its source findings
// (input the save drops); two waves of one name (the bank's lookups find the first); two sets of one
// name (the game finds the first); a wave whose file name the archives cannot hold; a member naming
// no wave of the bank (the save refuses it); a layer heard in neither view, which no play admits
// [orig: SoundBank_PlayTriggerEntries @ 0x75cd54]; a set with no member to play.
std::vector<Diagnostic> validate_sound_bank_file(const DocumentBase &document);
// A sound set no bank the game searches has, added to a bank (ADR 0046 DI-15, DocumentType::define_symbol): a set
// as Add set makes one, named as referenced; for a menu SOUND's set, only its own bank.
bool define_sound_set(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out);

enum class SoundBankFinding {
	InvalidInput,
	IgnoredInput,
	WaveNameRepeated,
	SetNameRepeated,
	WaveNoFile,
	WaveFileName,
	Unserializable,
	LayerUnheard,
	SetSilent,
	kCount
};
const FindingCodeRow &finding_code(SoundBankFinding code);
FindingTable sound_bank_finding_codes();

} // namespace opennova::editor
