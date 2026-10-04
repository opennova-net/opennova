#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/model/change_set.h>
#include <editor/model/diagnostic.h>
#include <editor/model/edit.h>
#include <editor/model/value.h>

namespace opennova::editor {

class Document;
class TextDocument;

// An editable file (ADR 0046 d9, S13 D6): the lifecycle every document shares, whatever its
// content. It is read from the project's file (or from a file's bytes, which an import reads
// before anything is written), decoded as the game's loader decodes a stored file and
// fingerprinted, so a Save refuses a file changed outside the editor (the conflict check) and
// writes it atomically; it keeps what the source held that its content cannot carry (issues:
// input the game ignores, or blocking input that stops editing and saving until the source is
// corrected); it is edited through a history of its own, which says whether it is dirty and
// numbers its states (revision); it serializes to the bytes its Save writes; and another thread
// reads a snapshot of it (model/document.h's thread confinement). The session, the windows and
// the shell hold every open document through this interface.
//
// What a document holds is its kind's. The record document (Document, model/document.h) holds
// rows of records that every edit addresses by identity and field (as_records() is it; null for
// any other kind of document); the text document (TextDocument, model/text_document.h, S13 D9)
// holds a text whose spans its edits replace (as_text()); a raster (a terrain's depth map) will
// hold its own content. A document of another kind than records takes its changes as Apply edits
// whose payload its type made (EditPayload), implementing apply_edits, the history, serialize and
// read_source. The base keeps the rules every kind obeys: a snapshot and a blocked document take
// no edit, undo or redo, and a blocked one no save.
class DocumentBase {
public:
	virtual ~DocumentBase() = default;

	// The file at `absolute_path` read, then load_bytes: the document keeps the path and what
	// the file held, which Save writes over and checks against.
	bool load(const std::string &absolute_path, const std::string &relative_path, AssetKind kind,
	          const std::string &game, Diagnostic &error);
	// A file's bytes as stored (the game's loader decodes them), with the name it goes by in
	// the project: what an import reads before anything is written (the import plan follows
	// its references). A document loaded this way has no file: Save refuses it
	// (document.no_file). A load gives the document a new load_generation(); one that fails
	// leaves the document as it was (its path, kind and game too).
	bool load_bytes(const std::vector<uint8_t> &bytes, const std::string &relative_path,
	                AssetKind kind, const std::string &game, Diagnostic &error);
	// Writes serialize()'s text over the file. The file's source findings are then those of
	// the text written (the input the rewrite dropped is no longer reported); the content and
	// the history stay, and the saved checkpoint moves to them (on_saved). A blocked document is
	// refused with its first blocking finding (document.unserializable).
	bool save(Diagnostic &error);
	// What an explicit Save does with this document when it has no unsaved edits: nothing when
	// it serializes to the bytes the file held when it was loaded or last saved; a write when it
	// serializes to other bytes (the canonical rewrite: the lines the game ignores dropped, the
	// line ends fixed, a table regrouped); a refusal when it does not serialize or is blocked
	// (Save says why, document.unserializable), never "no changes".
	enum class RewriteNeed { None, Rewrite, Unserializable };
	RewriteNeed rewrite_need() const;
	// True while the file holds the bytes this document was loaded from or last saved (the
	// conflict check Save makes); false once it changed outside the editor or is gone.
	bool matches_file() const;
	// True once Save wrote the file since the load: the file then holds bytes serialize()
	// made, not the bytes the document was read from (a type whose writer normalizes what
	// it read tells the two apart by it).
	bool wrote_file() const { return wrote_file_; }

	// --- the edits and the history -------------------------------------------------------------
	// A batch of edits applied as one undoable change, nothing applied when any is refused (the
	// refusal in `error`): a snapshot takes none (document.snapshot), nor does a blocked document
	// (document.parse); else the kind's apply_edits, which refuses an edit it cannot take. What an
	// edit may say is the kind's: the record document takes every operation, another kind of
	// document the Apply edits whose payload its type made.
	bool apply(const std::vector<Edit> &edits, Diagnostic &error);
	// One edit: a batch of one.
	bool apply(const Edit &edit, Diagnostic &error);
	// One step back or forward through the history (the kind's undo_step, redo_step); nothing on
	// a snapshot or a blocked document.
	void undo();
	void redo();
	// The open edit group (a coalesced burst of typing, a gesture) ends: the next edit is a step
	// of its own.
	virtual void end_edit_group() = 0;
	// What each step did, in words (the UX round's problems lane): the session names the step an edit
	// made with the status line's words for it ("Set hp of Drivable Dune Buggy to 120"), a fold of the
	// same group renaming it; what Undo and Redo then say they did, and what the Edit menu's items name.
	// undo_words: the step Undo would take back; redo_words: the step Redo would make again; "" for none,
	// or a step no words were given. A load forgets them.
	void name_step(std::string words);
	std::string undo_words() const;
	std::string redo_words() const;
	// The history, opaque to the lifecycle: whether the document differs from its saved
	// checkpoint (dirty: an undo back to it makes the document clean again), whether a step can
	// be undone or redone, the number of the state it is in (revision: a later state has a
	// larger one, and an undo gives the earlier state's back; a load starts it again at 0), and
	// how many bytes the history keeps (what its budget measures, S13 D7).
	virtual bool dirty() const = 0;
	virtual bool can_undo() const = 0;
	virtual bool can_redo() const = 0;
	virtual uint64_t revision() const = 0;
	virtual size_t history_bytes() const = 0;
	// What changed from the state a caller last read (the load_generation() and revision() it read
	// then) to this one, in the words of the document's kind (ChangeSet, ADR 0046 S13 D7): a record
	// document's rows, a text's spans, a raster's regions; empty when it is that state. False when
	// the document cannot say, and the caller takes everything as changed: another load's state
	// (the document read again in place), a state its history no longer holds (given up to the
	// history's budget, or on a branch an edit after an undo discarded), and, by the base's
	// default, a kind that does not say.
	virtual bool changes_since(uint64_t load_generation, uint64_t revision, ChangeSet &out) const {
		(void)load_generation;
		(void)revision;
		(void)out;
		return false;
	}
	// Whether the batches of the gesture `gesture` (Edit::gesture) alone made the document's state from
	// its state `revision` of this load, nothing else between (no batch of another, no undo, no redo):
	// what a cache held through a gesture's samples asks (the mission canvas's titles, the polish), so
	// it keeps what it made of that state through the gesture's own edits and drops it on any other
	// (one through the wire mid-drag). False for gesture 0.
	bool gesture_alone_since(uint64_t gesture, uint64_t revision) const;

	// --- what it is ----------------------------------------------------------------------------
	const std::string &path() const { return relative_path_; }
	AssetKind kind() const { return kind_; }
	// This instance, distinct from every other document ever made in the process but its
	// snapshots, which share it: a reload is a new document even at the same revision (the
	// graph caches by it).
	uint64_t identity() const { return identity_; }
	// Which load the content came from: a value of a process-wide counter taken anew by every
	// load and load_bytes (0 before the first; a snapshot shares its document's). A load in place
	// starts revision() again at 0, so what a type keeps of its content keys on the two together
	// (a menu's saved image, a stylesheet's evaluated sheet), never on the revision alone.
	uint64_t load_generation() const { return load_generation_; }
	// True while the source holds input the content cannot carry: editing and saving wait for
	// the source to be corrected and reloaded. Lines the game ignores are only reported
	// (issues) and are dropped on save.
	bool blocked() const { return blocked_; }
	size_t ignored_lines() const;
	const std::vector<SourceIssue> &issues() const { return issues_; }
	// The bytes a Save writes, or the issues that keep it from writing (the save refuses it).
	virtual SerializeResult serialize() const = 0;
	// This document as it stands, for another thread (model/document.h's thread confinement): a
	// new instance of its type over the same content, with its identity, load generation, revision,
	// history and saved checkpoint, made through the type's copy constructor over the base's
	// (return std::make_unique<Type>(*this)). Read only: the base refuses an edit, a load and a
	// save of it (document.snapshot) and does nothing on its undo and redo.
	virtual std::unique_ptr<DocumentBase> snapshot() const = 0;
	bool is_snapshot() const { return snapshot_; }
	// The record document this is (model/document.h), null for another kind of document: what a
	// caller that reads rows, records and fields asks first (records_of).
	virtual const Document *as_records() const { return nullptr; }
	virtual Document *as_records() { return nullptr; }
	// The text document this is (model/text_document.h), null for another kind of document: what a
	// caller that reads lines and spans asks first (text_of).
	virtual const TextDocument *as_text() const { return nullptr; }
	virtual TextDocument *as_text() { return nullptr; }
	// Whether it holds an image (ADR 0046 S18: a texture's texels), which its type's own hooks read
	// (documents/document_types.h, DocumentContent::Image).
	virtual bool holds_image() const { return false; }

protected:
	DocumentBase();
	// The copy a snapshot is (snapshot()): the same path, kind, game, identity, load generation,
	// source findings and file fingerprint; read only.
	DocumentBase(const DocumentBase &other);
	DocumentBase &operator=(const DocumentBase &) = delete;

	// The kind's part of apply, undo and redo, which the base calls only for a document that is
	// neither a snapshot nor blocked.
	virtual bool apply_edits(const std::vector<Edit> &edits, Diagnostic &error) = 0;
	virtual void undo_step() = 0;
	virtual void redo_step() = 0;
	// The kind's part of a load and of a save: the source, decoded as the game's loader decodes
	// a stored file (the base decodes it and keeps the fingerprint of the bytes as stored; a kind
	// whose loader takes the SCR form under its own key, a shader, the bytes as stored, which its
	// type reads), read into the kind's content. `adopt`: a load, whose content becomes the document's, its history
	// and saved checkpoint starting again; false: the text a save just wrote, read back for its
	// findings alone, the content staying the document's. `issues` gets the source findings (a
	// blocking one blocks editing and saving). False, with `error` and nothing adopted, when the
	// source does not read.
	virtual bool read_source(const std::vector<uint8_t> &decoded, bool adopt,
	                         std::vector<SourceIssue> &issues, Diagnostic &error) = 0;
	// After a save wrote the file: the history's saved checkpoint moves to the state written, the
	// open edit group ends, and what the kind compares with the saved file moves to it.
	virtual void on_saved() = 0;

	const std::string &absolute_path() const { return absolute_path_; }
	const std::string &game() const { return game_; }

private:
	// The first source finding that blocks (null for none): what a Save of a blocked document says.
	const SourceIssue *first_blocking() const;
	// Stored bytes decoded as the game's loader decodes them (the target game's SCR policy), then
	// read_source.
	bool decode_and_read(std::vector<uint8_t> bytes, bool adopt, std::vector<SourceIssue> &issues,
	                     Diagnostic &error);

	uint64_t identity_ = 0, load_generation_ = 0;
	// The run of batches of one gesture that made the state (gesture_alone_since): its gesture (0: the
	// state was made otherwise), the state it began from, and the revision its first batch made (every
	// state the run made has one from there on, a revision being taken anew by each batch).
	uint64_t run_gesture_ = 0, run_from_ = 0, run_first_ = 0;
	std::string absolute_path_, relative_path_, game_;
	AssetKind kind_ = AssetKind::Unknown;
	std::vector<SourceIssue> issues_;
	bool blocked_ = false, wrote_file_ = false, snapshot_ = false;
	uint64_t file_fingerprint_ = 0;
	// Each step's words by the revision it made (revisions are never given twice within a load), and
	// the revisions Redo goes back to, the next one last.
	std::map<uint64_t, std::string> step_words_;
	std::vector<uint64_t> redo_revisions_;
};

} // namespace opennova::editor
