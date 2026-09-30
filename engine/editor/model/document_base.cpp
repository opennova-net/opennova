#include "document_base.h"

#include <base/gameprofile/gameprofile.h>
#include <base/io/hash.h>
#include <base/vfs/vfs_decode.h>
#include <editor/project/project_files.h>
#include <editor/session/finding_codes.h>

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
	return apply_edits(edits, error);
}

bool DocumentBase::apply(const Edit &edit, Diagnostic &error) {
	return apply(std::vector<Edit>{edit}, error);
}

void DocumentBase::undo() {
	if (!snapshot_ && !blocked_) undo_step();
}

void DocumentBase::redo() {
	if (!snapshot_ && !blocked_) redo_step();
}

size_t DocumentBase::ignored_lines() const {
	size_t count = 0;
	for (const auto &issue : issues_) if (!issue.blocks) ++count;
	return count;
}

bool DocumentBase::decode_and_read(std::vector<uint8_t> bytes, bool adopt,
                                   std::vector<SourceIssue> &issues, Diagnostic &error) {
	if (!opennova::vfs_decode_payload(bytes, gameprofile::gameprofile_scr_policy_for_code(game_.c_str())))
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
