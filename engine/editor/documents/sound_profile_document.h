#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/table_document.h>
#include <runtime/audio/sound_profile.h>

namespace opennova::editor {

// The sound profiles (ADR 0046, the sound lane): `SndProf.def`, the per-item table of the sounds a
// body, a vehicle or an object makes (docs/audio/lwf-dbf-sound-re.md "The sound-profile system").
// Its rows are its profiles in the file's order, each the game's 2152-byte profile [orig:
// SoundProfile_LoadAll @ 0x527490; SoundProfile_ParseLineCallback @ 0x526fc0]: its name, which an
// item's sound_profile binds by (the first of the name, without case; a name no profile has binds
// the first profile [orig: SoundProfile_FindSlotByName @ 0x526e30]), its twelve loop fade and pitch
// percents, and its 51 slots, a fixed list in the engine's slot order (the keyword table @ 0x82F3B0,
// audio::sound_profile_slot_keyword), each naming the sound set the game plays for it (resolved by
// name across the loaded banks at mission start [orig: SoundProfile_ResolveAllTriggers @ 0x528210])
// with the slot's three numbers. The document reads the file through the game's walk
// (audio::SoundProfileTable::parse, the columns a short line takes from an earlier one included,
// D-ITEMDEF-8) and writes it from scratch through audio::write_sound_profiles, every slot line with
// its five columns.

enum class SoundProfileKind : NodeKind { Profile = 0, Slot = 1 };
constexpr NodeKind node_kind(SoundProfileKind kind) { return static_cast<NodeKind>(kind); }

// A slot of a profile as the table holds it: its index (its keyword), the set and the three numbers.
struct ProfileSlot {
	int slot = 0;
	std::string set;
	int32_t param2_q16 = 0;
	int32_t param3_q16 = 0;
	int32_t param4 = 0;
};

// A profile: its name, its percents and its 51 slots, each at its index.
struct ProfileRecord {
	std::string name;
	std::array<int32_t, 12> loop_params{};
	std::vector<ProfileSlot> slots; // always 51, slot i at i
};

struct SoundProfileRow : TableRow {
	ProfileRecord profile;

	SoundProfileRow();
	std::shared_ptr<Node> clone() const override { return std::make_shared<SoundProfileRow>(*this); }
	std::string name() const override { return profile.name; }
	RecordHandle record() const override;
	size_t footprint() const override;
};

const RecordTable &sound_profile_table();

class SoundProfileDocument : public TableDocument {
public:
	const RecordTable &table() const override { return sound_profile_table(); }
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return sound_profile_table().fields(kind); }
	// A slot by what it is for and the set it plays ("left foot on the ground: FSP_DIRT_L").
	std::string record_title(const NodeAddress &address) const override;
	SerializeResult serialize() const override;
	std::string save_words() const override;
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<SoundProfileDocument>(*this); }

	// The profiles as the game holds them, from the rows (what serialize() writes).
	std::vector<audio::SoundProfile> profiles() const;
	// The profile the game binds `name` to: the first of the name without case, else the first
	// profile [orig: SoundProfile_FindSlotByName @ 0x526e30]; null with none.
	const SoundProfileRow *find_profile(const std::string &name) const;

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	// A new profile: "default" where no profile has the name (what every item binds without a
	// sound_profile key), else a name no profile has; every slot silent.
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	void prepare_duplicate(Node &copy, const Node &original,
	                       const std::vector<std::shared_ptr<const Node>> &rows) const override;
};

bool is_sound_profile_kind(AssetKind kind);

// A slot's keyword and what it is for, in words ("SSLFootGND", "left foot on the ground"); the family
// heading it stands under ("Footsteps").
const char *sound_profile_slot_words(int slot);
const char *sound_profile_slot_family(int slot);
// The slot a keyword names (without case), -1 for none.
int sound_profile_slot_of(const std::string &keyword);

// The validator over one SndProf.def: a profile name an earlier profile has (the game binds the
// first), no profile named "default" (every item without a sound_profile key binds the first
// profile then), and a profile the writer cannot carry (the file does not serialize).
std::vector<Diagnostic> validate_sound_profiles_file(const DocumentBase &document);
// A sound profile an item names that SndProf.def has none of, added to it (ADR 0046 DI-15,
// DocumentType::define_symbol): a profile as Add sound profile makes one, named as the item names it.
bool define_sound_profile(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out);

enum class SoundProfileFinding { NameRepeated, NoDefault, Unserializable, kCount };
const FindingCodeRow &finding_code(SoundProfileFinding code);
FindingTable sound_profile_finding_codes();

} // namespace opennova::editor
