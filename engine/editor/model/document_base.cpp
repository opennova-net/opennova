#include "document_base.h"

#include <base/gameprofile/gameprofile.h>
#include <base/io/hash.h>
#include <base/vfs/vfs_decode.h>
#include <editor/assets/asset_kinds.h>
#include <editor/project/project_files.h>

#include <atomic>
#include <utility>

namespace opennova::editor {

EditPayload::~EditPayload() = default;

namespace {

std::atomic<uint64_t> g_next_identity{0};
std::atomic<uint64_t> g_next_load{0};

uint64_t fingerprint(const uint8_t *data, size_t size) { return io::fnv1a64_bytes(io::kFnv1a64Offset, data, size); }
uint64_t fingerprint(const std::vector<uint8_t> &bytes) { return fingerprint(bytes.data(), bytes.size()); }
uint64_t fingerprint(const std::string &text) {
	return fingerprint(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

bool fail(Diagnostic &error, const std::string &path, CoreFinding code, const std::string &message,
          const std::string &field = {}) {
	error = make_finding(code, DiagnosticSeverity::Error, message, path, field);
	return false;
}

} // namespace

DocumentBase::DocumentBase() : identity_(++g_next_identity) {}

DocumentBase::DocumentBase(const DocumentBase &other)
		: identity_(other.identity_), load_generation_(other.load_generation_),
		  absolute_path_(other.absolute_path_), relative_path_(other.relative_path_),
		  game_(other.game_), kind_(other.kind_), issues_(other.issues_), blocked_(other.blocked_),
		  wrote_file_(other.wrote_file_), snapshot_(true),
		  file_fingerprint_(other.file_fingerprint_) {}

bool DocumentBase::apply(const std::vector<Edit> &edits, Diagnostic &error) {
	if (snapshot_)
		return fail(error, path(), CoreFinding::DocumentSnapshot, "A snapshot is read, never edited.");
	if (blocked_)
		return fail(error, path(), CoreFinding::DocumentParse,
		            "Fix the reported source errors and reload this document before editing.");
	const uint64_t before = revision();
	if (!apply_edits(edits, error)) return false;
	// A new step discards the redo branch, and the words of the steps on it with the revisions.
	if (!can_redo()) redo_revisions_.clear();
	if (revision() != before) {
		// The batch's gesture: its edits' one token (0 when they carry none, or several).
		uint64_t gesture = edits.empty() ? 0 : edits.front().gesture;
		for (const Edit &edit : edits) gesture = edit.gesture == gesture ? gesture : 0;
		if (!gesture || gesture != run_gesture_) {
			run_from_ = before;
			run_first_ = revision();
		}
		run_gesture_ = gesture;
	}
	return true;
}

bool DocumentBase::gesture_alone_since(uint64_t gesture, uint64_t revision) const {
	if (!gesture || gesture != run_gesture_) return false;
	return revision == run_from_ || (revision >= run_first_ && revision <= this->revision());
}

bool DocumentBase::apply(const Edit &edit, Diagnostic &error) {
	return apply(std::vector<Edit>{edit}, error);
}

void DocumentBase::undo() {
	if (snapshot_ || blocked_) return;
	const uint64_t before = revision();
	undo_step();
	run_gesture_ = 0;
	if (revision() != before) redo_revisions_.push_back(before);
}

void DocumentBase::redo() {
	if (snapshot_ || blocked_) return;
	const uint64_t before = revision();
	redo_step();
	run_gesture_ = 0;
	if (revision() != before && !redo_revisions_.empty()) redo_revisions_.pop_back();
}

void DocumentBase::name_step(std::string words) {
	if (snapshot_) return;
	// A long session's words for steps the history gave up are let go with the oldest.
	constexpr size_t kKeptWords = 4096;
	step_words_[revision()] = std::move(words);
	while (step_words_.size() > kKeptWords) step_words_.erase(step_words_.begin());
}

std::string DocumentBase::undo_words() const {
	if (!can_undo()) return std::string();
	const auto found = step_words_.find(revision());
	return found == step_words_.end() ? std::string() : found->second;
}

std::string DocumentBase::redo_words() const {
	if (!can_redo() || redo_revisions_.empty()) return std::string();
	const auto found = step_words_.find(redo_revisions_.back());
	return found == step_words_.end() ? std::string() : found->second;
}

size_t DocumentBase::ignored_lines() const {
	size_t count = 0;
	for (const auto &issue : issues_) if (!issue.blocks) ++count;
	return count;
}

std::string DocumentBase::save_words() const {
	const size_t ignored = ignored_lines();
	if (!ignored) return std::string();
	return "Saving leaves out " + std::to_string(ignored) + (ignored == 1 ? " thing" : " things") +
	       " in the file the game skips (Problems lists each); the game reads the rest as before.";
}

bool DocumentBase::decode_and_read(std::vector<uint8_t> bytes, bool adopt,
                                   std::vector<SourceIssue> &issues, Diagnostic &error) {
	// A kind whose loader takes the SCR form under its own key (a shader) is read by its type from
	// the bytes as stored (assets/asset_kinds.h, ScrForm).
	if (asset_kind_row(kind_).scr == ScrForm::Optional &&
			!opennova::vfs_decode_payload(bytes, gameprofile::gameprofile_scr_policy_for_code(game_.c_str())))
		return fail(error, relative_path_, CoreFinding::DocumentDecode, "The file could not be decoded.");
	return read_source(bytes, adopt, issues, error);
}

bool DocumentBase::load(const std::string &absolute, const std::string &relative, AssetKind kind,
                        const std::string &game, Diagnostic &error) {
	if (snapshot_) return fail(error, relative, CoreFinding::DocumentSnapshot, "A snapshot is never loaded.");
	std::vector<uint8_t> bytes;
	std::string message;
	if (!read_file_bytes(absolute, bytes, message)) return fail(error, relative, CoreFinding::DocumentRead, message);
	if (!load_bytes(bytes, relative, kind, game, error)) return false;
	absolute_path_ = absolute;
	return true;
}

bool DocumentBase::load_bytes(const std::vector<uint8_t> &bytes, const std::string &relative,
                              AssetKind kind, const std::string &game, Diagnostic &error) {
	if (snapshot_) return fail(error, relative, CoreFinding::DocumentSnapshot, "A snapshot is never loaded.");
	const uint64_t hash = fingerprint(bytes);
	std::vector<SourceIssue> issues;
	// The kind reads the source as the file it names (its path, kind and game); a source that
	// does not read leaves the document as it was, those too.
	std::string previous_path = relative, previous_game = game;
	AssetKind previous_kind = kind;
	std::swap(relative_path_, previous_path);
	std::swap(kind_, previous_kind);
	std::swap(game_, previous_game);
	if (!decode_and_read(bytes, true, issues, error)) {
		relative_path_ = std::move(previous_path);
		kind_ = previous_kind;
		game_ = std::move(previous_game);
		return false;
	}
	absolute_path_.clear(); // no file until load names one
	issues_ = std::move(issues);
	blocked_ = false;
	for (const auto &issue : issues_) blocked_ = blocked_ || issue.blocks;
	file_fingerprint_ = hash;
	wrote_file_ = false;
	load_generation_ = ++g_next_load;
	// The history starts again, and its revisions with it: no step keeps its words.
	step_words_.clear();
	redo_revisions_.clear();
	run_gesture_ = 0;
	return true;
}

bool DocumentBase::save(Diagnostic &error) {
	if (snapshot_)
		return fail(error, path(), CoreFinding::DocumentSnapshot, "A snapshot is read, never saved.");
	if (absolute_path_.empty())
		return fail(error, path(), CoreFinding::DocumentNoFile, "This document was read from bytes, not from a file: it has no file to save to.");
	if (const SourceIssue *blocking = first_blocking())
		return fail(error, path(), CoreFinding::DocumentUnserializable, blocking->message, blocking->field);
	const SerializeResult output = serialize();
	if (!output.ok())
		return fail(error, path(), CoreFinding::DocumentUnserializable, output.issues.front().message, output.issues.front().field);
	if (!matches_file())
		return fail(error, path(), CoreFinding::DocumentConflict, "This file changed outside the editor. Reload it before saving.");
	std::string message;
	if (!write_file_atomic(absolute_path_, output.text, message)) return fail(error, path(), CoreFinding::DocumentWrite, message);
	file_fingerprint_ = fingerprint(output.text);
	wrote_file_ = true;
	save_notes_ = output.notes;
	on_saved();
	// The source is now the text just written, read the way a reload would: its findings
	// replace the ones of the text it was loaded from, so what the rewrite dropped (the lines
	// the game ignores, a table's grouping) is no longer reported. Only the findings are
	// kept: the content and the history are the document's. (A text that does not read back
	// keeps the findings it had: a reload says why.)
	std::vector<SourceIssue> issues;
	Diagnostic unread;
	std::vector<uint8_t> written(output.text.begin(), output.text.end());
	if (decode_and_read(std::move(written), false, issues, unread)) {
		issues_ = std::move(issues);
		blocked_ = false;
		for (const auto &issue : issues_) blocked_ = blocked_ || issue.blocks;
	}
	return true;
}

const SourceIssue *DocumentBase::first_blocking() const {
	for (const SourceIssue &issue : issues_)
		if (issue.blocks) return &issue;
	return nullptr;
}

DocumentBase::RewriteNeed DocumentBase::rewrite_need() const {
	if (blocked_) return RewriteNeed::Unserializable;
	const SerializeResult output = serialize();
	if (!output.ok()) return RewriteNeed::Unserializable;
	return fingerprint(output.text) != file_fingerprint_ ? RewriteNeed::Rewrite : RewriteNeed::None;
}

bool DocumentBase::matches_file() const {
	std::vector<uint8_t> current;
	std::string message;
	return read_file_bytes(absolute_path_, current, message) && fingerprint(current) == file_fingerprint_;
}

} // namespace opennova::editor
