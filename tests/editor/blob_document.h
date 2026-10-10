#pragma once

// A document of another kind than records (ADR 0046 S13 D6: DocumentBase alone), for the tests
// of the lifecycle and of what the editor does with such a document: a blob of text whose lines
// starting with '#' the game ignores (each an issue, dropped on save) and whose lines starting
// with '!' block it (input it cannot carry: an issue that blocks editing and saving); a text
// starting with "FAIL" does not read at all. Its one change is a BlobReplace its type makes; its
// history is a list of the blobs it held, each with its revision.

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document_base.h>
#include <editor/model/edit.h>

namespace editor_test {

// A replacement of a blob's whole text made in C++: the one change a BlobDocument takes.
struct BlobReplace : opennova::editor::EditPayload {
	std::string text;
	explicit BlobReplace(std::string bytes) : text(std::move(bytes)) {}
	const char *token() const override { return "blob.replace"; }
};

class BlobDocument : public opennova::editor::DocumentBase {
public:
	void end_edit_group() override {}
	bool dirty() const override { return revision_ != saved_revision_; }
	bool can_undo() const override { return !undo_.empty(); }
	bool can_redo() const override { return !redo_.empty(); }
	uint64_t revision() const override { return revision_; }
	size_t history_bytes() const override {
		size_t bytes = 0;
		for (const std::vector<State> *list : {&undo_, &redo_})
			for (const State &state : *list) bytes += state.blob.size();
		return bytes;
	}
	// A line starting with '%' is no part of the file: a note its save says (SerializeResult::notes).
	opennova::editor::SerializeResult serialize() const override {
		opennova::editor::SerializeResult result;
		std::istringstream in(blob_);
		for (std::string line; std::getline(in, line);) {
			if (line.rfind('%', 0) == 0) result.notes.push_back(line.substr(1));
			else if (line.rfind('#', 0) != 0) result.text += line + "\n";
		}
		return result;
	}
	std::unique_ptr<opennova::editor::DocumentBase> snapshot() const override {
		return std::make_unique<BlobDocument>(*this);
	}
	const std::string &blob() const { return blob_; }
	const std::string &game_name() const { return game(); }

protected:
	// No snapshot or blocked test here: the base refuses those before it calls this.
	bool apply_edits(const std::vector<opennova::editor::Edit> &edits,
	                 opennova::editor::Diagnostic &error) override {
		std::string next = blob_;
		for (const opennova::editor::Edit &edit : edits) {
			const auto *replace = edit.operation == opennova::editor::EditOperation::Apply
			                              ? dynamic_cast<const BlobReplace *>(edit.payload.get())
			                              : nullptr;
			if (!replace)
				return refuse(error, opennova::editor::CoreFinding::DocumentPayload,
				              "A blob takes its own replacements only.");
			next = replace->text;
		}
		if (next == blob_) return true;
		undo_.push_back({blob_, revision_});
		redo_.clear();
		blob_ = std::move(next);
		revision_ = next_revision_++;
		return true;
	}
	void undo_step() override { step(undo_, redo_); }
	void redo_step() override { step(redo_, undo_); }
	bool read_source(const std::vector<uint8_t> &decoded, bool adopt,
	                 std::vector<opennova::editor::SourceIssue> &issues,
	                 opennova::editor::Diagnostic &error) override {
		const std::string text(decoded.begin(), decoded.end());
		if (text.rfind("FAIL", 0) == 0)
			return refuse(error, opennova::editor::CoreFinding::DocumentParse, "The blob does not read.");
		std::istringstream in(text);
		size_t number = 0;
		const auto issue = [&](bool blocking, const char *message) {
			issues.push_back({blocking, number, std::string(), std::string(), message});
		};
		for (std::string line; std::getline(in, line);) {
			++number;
			if (line.rfind('#', 0) == 0)
				issue(false, "The game ignores this line.");
			else if (line.rfind('!', 0) == 0)
				issue(true, "The blob cannot carry this line.");
		}
		if (!adopt) return true;
		blob_ = text;
		undo_.clear();
		redo_.clear();
		revision_ = saved_revision_ = 0;
		next_revision_ = 1;
		return true;
	}
	void on_saved() override { saved_revision_ = revision_; }

private:
	struct State {
		std::string blob;
		uint64_t revision = 0;
	};
	void step(std::vector<State> &from, std::vector<State> &to) {
		if (from.empty()) return;
		to.push_back({blob_, revision_});
		blob_ = from.back().blob;
		revision_ = from.back().revision;
		from.pop_back();
	}
	static bool refuse(opennova::editor::Diagnostic &error, opennova::editor::CoreFinding code,
	                   const char *message) {
		error = opennova::editor::make_finding(code, opennova::editor::DiagnosticSeverity::Error, message);
		return false;
	}
	std::string blob_;
	std::vector<State> undo_, redo_;
	uint64_t revision_ = 0, saved_revision_ = 0, next_revision_ = 1;
};

} // namespace editor_test
