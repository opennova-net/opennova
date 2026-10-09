#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/table_document.h>
#include <formats/dbf/dbf.h>

namespace opennova::editor {

// A mission's dialog bank (ADR 0046 DI-32): a `.dbf`, the dialogs a mission's Play dialog actions play
// (formats/dbf; docs/audio/lwf-dbf-sound-re.md "The dialog banks"). Its rows are its dialogs in the
// file's order, each holding its lines:
// - a dialog is found by its name, matched exactly, the first of the name in the bank's order [orig:
//   Dialog_PlayByName @ 0x44d9f0, strcmp @ 0x44da8b]; a mission's Play dialog action and its two Dialog
//   triggers name one by the number that forms "dlg%03i" [orig: Dialog_PlayByIndex @ 0x527ae0;
//   Dialog_ExistsByIndex @ 0x44e170], so a name no number forms is never played;
// - a line names a wave of the bank's sounds, the `.lwf` of the bank's own name (else its `.pwf`) [orig:
//   DialogManager_LoadFromFile @ 0x44e7bb..0x44e807], found there by its name without case and played at
//   that wave's dialog volume [orig: Dialog_LoadAudioClip @ 0x44dcf7..0x44dd15 -> SoundBank_FindEntryByName
//   @ 0x75bba0]; its subtitle is the mission text's [Mission Dialog] entry of the wave's name, else the
//   text's entry whose number follows the last '_' of the line's sequence [orig: @ 0x44ddcd..0x44de3c]; a
//   line after the first waits its delay, in tenths of a second, once the one before it has ended [orig:
//   Dialog_UpdatePlayback @ 0x44e585, 62 * delay / 10 ticks].
// The line's other words (its flags, its def id index, its param and reserved words) and the dialog's id
// list are copied into the play's block and read by no hook [orig: Dialog_LoadAudioClip @ 0x44dd1d..0x44dd37;
// the hooks sub_527560 @ 0x527560, Chat_AddSystemMessageIfValid]: kept as read, never shown. serialize()
// writes the bank from scratch through the engine's writer (dbf::encode_dbf, ADR 0003).

enum class DialogBankKind : NodeKind { Dialog = 0, Line = 1 };
constexpr NodeKind node_kind(DialogBankKind kind) { return static_cast<NodeKind>(kind); }

// The native records the bank's table describes (dialog_bank_table in the .cpp).
struct BankLine {
	std::string wave;          // the wave of the bank's sounds it plays, by name (+4, 24 bytes)
	std::string sequence = "##"; // its subtitle's entry after the last '_' (+28, 24 bytes)
	uint8_t delay = 0;         // tenths of a second it waits after the line before (+53)
	uint32_t flags = 0;        // +0, read by nothing
	uint8_t def_id_index = 0xFF; // +52, read by nothing
	uint32_t param = 0;        // +56, read by nothing
	uint32_t resd0 = 0;        // +60
	uint32_t resd1 = 0;        // +64
};
struct BankDialog {
	std::string name;
	uint32_t idlist_count = 0;          // +32, read by nothing
	std::vector<uint8_t> def_id_indices; // +36, 16 bytes, read by nothing
	std::vector<BankLine> lines;
};

struct DialogBankRow : TableRow {
	BankDialog dialog;

	DialogBankRow() { kind = 0; }
	std::shared_ptr<Node> clone() const override { return std::make_shared<DialogBankRow>(*this); }
	std::string name() const override { return dialog.name; }
	RecordHandle record() const override { return RecordHandle{kind, const_cast<BankDialog *>(&dialog)}; }
	size_t footprint() const override;
};

const RecordTable &dialog_bank_table();

class DialogBankDocument : public TableDocument {
public:
	const RecordTable &table() const override { return dialog_bank_table(); }
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return dialog_bank_table().fields(kind); }
	// A dialog by its name and its lines' waves, a line by its wave (and its wait).
	std::string record_title(const NodeAddress &address) const override;
	SerializeResult serialize() const override;
	std::string save_words() const override;
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<DialogBankDocument>(*this); }

	// The bank as the engine reads it, made from the rows (what serialize() writes; the preview plays a
	// dialog from it).
	dbf::File bank() const;
	std::vector<const DialogBankRow *> dialogs() const;
	// The dialog the game finds by `name`: the first of the name, matched exactly; null for none.
	const DialogBankRow *find_dialog(const std::string &name) const;

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	// A new dialog: the first name dlg%03i forms that no dialog has, no line yet.
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	// A copy of a dialog takes the next free name, as a new one does (the game finds the first of two of one name).
	void prepare_duplicate(Node &copy, const Node &original,
	                       const std::vector<std::shared_ptr<const Node>> &rows) const override;
	// A dialog's name lives in this bank (the scope its file's name gives), a line's wave in the bank's sounds
	// (<base>.LWF, else <base>.PWF).
	void refine_field(const NodeAddress &address, FieldUse &use) const override;
	// A later dialog of a name an earlier one has is never found.
	void refine_symbol(const NodeAddress &address, SymbolFacts &facts) const override;
};

bool is_dialog_bank_kind(AssetKind kind);
// The scope a bank's dialogs live in: its file name, upper case ("01TR.DBF").
std::string dialog_bank_scope(const std::string &path);
// The scope a bank's lines' waves are found in: the bank's sounds, <base>.LWF; and the file the game opens in
// its place where it has none, <base>.PWF [orig: DialogManager_LoadFromFile @ 0x44e7d4..0x44e7f5].
std::string dialog_sounds_scope(const std::string &path);
std::string dialog_sounds_alternate(const std::string &path);
// The number a dialog's name answers to is the engine's audio::dialog_index_of (runtime/audio/dialog_queue.h).

// The dialog bank type's validator over one bank (DocumentType::validate_file): its source findings; a dialog of
// a name an earlier one has (the game plays the first); one whose name no number forms (no Play dialog plays it);
// a dialog with no line; a line naming no wave.
std::vector<Diagnostic> validate_dialog_bank_file(const DocumentBase &document);
// A dialog a mission names and the bank lacks, added to it (DI-15, DocumentType::define_symbol): a dialog of the
// name, no line yet, as Add dialog makes one.
bool define_dialog(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out);

enum class DialogBankFinding {
	InvalidInput,
	IgnoredInput,
	NameRepeated,
	NameUnplayed,
	Silent,
	LineNoWave,
	kCount
};
const FindingCodeRow &finding_code(DialogBankFinding code);
FindingTable dialog_bank_finding_codes();

} // namespace opennova::editor
