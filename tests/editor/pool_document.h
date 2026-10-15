#pragma once

// A record document of several kinds of row for the tests of what the windows make of them (the
// outline's kinds and listed rows, the Inspector's form over records of kinds alike): crates and
// barrels, two kinds of row over one field table (a name and a weight, as a mission's four entity
// pools are four kinds over one), and notes, a third kind whose fields differ (a name and a body).
// One line per row, "C <name> <weight>", "B <name> <weight>" or "N <name> <body>", in the file's order.

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <editor/model/document.h>

#include "editor/editor_test_support.h"

namespace editor_test {

inline constexpr opennova::editor::NodeKind kPoolCrate = 0, kPoolBarrel = 1, kPoolNote = 2;

struct PoolRow : opennova::editor::Node {
	std::string title;
	int64_t weight = 0;
	std::string body;
	std::shared_ptr<opennova::editor::Node> clone() const override { return std::make_shared<PoolRow>(*this); }
	std::string name() const override { return title; }
	size_t footprint() const override {
		return sizeof(PoolRow) + opennova::editor::footprint_of(title) + opennova::editor::footprint_of(body);
	}
};

class PoolDocument : public opennova::editor::Document {
public:
	using Node = opennova::editor::Node;
	using NodeAddress = opennova::editor::NodeAddress;
	using NodeKind = opennova::editor::NodeKind;
	using FieldSchema = opennova::editor::FieldSchema;
	using FieldType = opennova::editor::FieldType;
	using Value = opennova::editor::Value;

	const std::vector<opennova::editor::RecordKindRow> &kinds() const override {
		static const std::vector<opennova::editor::RecordKindRow> table = {
			{ kPoolCrate, "crate", "Crate", "Add crate", true },
			{ kPoolBarrel, "barrel", "Barrel", "Add barrel", true },
			{ kPoolNote, "note", "Note", "Add note", true },
		};
		return table;
	}
	std::vector<Collection> collections(const Node &, const NodeAddress &) const override { return {}; }
	// A crate's and a barrel's fields are two lists alike (the same ids and types in the same order,
	// apart in storage); a note's differ.
	const std::vector<FieldSchema> &fields(NodeKind kind) const override {
		static const std::vector<FieldSchema> crate = weighed();
		static const std::vector<FieldSchema> barrel = weighed();
		static const std::vector<FieldSchema> note = { FieldSchema{ "name", FieldType::Text, 32 },
			FieldSchema{ "body", FieldType::Text, 64 } };
		static const std::vector<FieldSchema> none;
		return kind == kPoolCrate ? crate : kind == kPoolBarrel ? barrel : kind == kPoolNote ? note : none;
	}
	opennova::editor::SerializeResult serialize() const override {
		opennova::editor::SerializeResult result;
		for (const auto &node : rows()) {
			const auto &row = static_cast<const PoolRow &>(*node);
			if (row.kind == kPoolNote) result.text += "N " + row.title + " " + row.body + "\n";
			else result.text += std::string(row.kind == kPoolCrate ? "C " : "B ") + row.title + " " +
						std::to_string(row.weight) + "\n";
		}
		return result;
	}
	std::unique_ptr<opennova::editor::DocumentBase> snapshot() const override {
		return std::make_unique<PoolDocument>(*this);
	}

protected:
	bool read(const Node &node, const NodeAddress &address, const std::string &field, Value &out) const override {
		const auto &row = static_cast<const PoolRow &>(node);
		if (address.child) return false;
		if (field == "name") out = row.title;
		else if (field == "weight" && row.kind != kPoolNote) out = row.weight;
		else if (field == "body" && row.kind == kPoolNote) out = row.body;
		else return false;
		return true;
	}
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
			std::shared_ptr<const opennova::editor::FileState> &, std::vector<opennova::editor::SourceIssue> &,
			opennova::editor::Diagnostic &error) override {
		std::istringstream in(std::string(bytes.begin(), bytes.end()));
		std::string tag;
		while (in >> tag) {
			auto row = std::make_shared<PoolRow>();
			bool read = false;
			if (tag == "C" || tag == "B") {
				row->kind = tag == "C" ? kPoolCrate : kPoolBarrel;
				read = bool(in >> row->title >> row->weight);
			} else if (tag == "N") {
				row->kind = kPoolNote;
				read = bool(in >> row->title >> row->body);
			}
			if (!read) {
				error = finding_of(opennova::editor::DiagnosticSeverity::Error, "document.parse", "Not a pool row.", path());
				return false;
			}
			rows.push_back(row);
		}
		return true;
	}
	std::shared_ptr<Node> make_node(NodeKind kind, opennova::editor::NodeId,
			const std::vector<std::shared_ptr<const Node>> &, std::string &) override {
		auto row = std::make_shared<PoolRow>();
		row->kind = kind;
		row->title = "new";
		return row;
	}
	bool set_field(Node &node, const NodeAddress &, const std::string &field, const Value &value,
			std::string &error) override {
		auto &row = static_cast<PoolRow &>(node);
		const auto *text = std::get_if<std::string>(&value);
		const auto *number = std::get_if<int64_t>(&value);
		if (field == "name" && text) row.title = *text;
		else if (field == "weight" && number && row.kind != kPoolNote) row.weight = *number;
		else if (field == "body" && text && row.kind == kPoolNote) row.body = *text;
		else {
			error = "Unknown field.";
			return false;
		}
		return true;
	}

private:
	static std::vector<FieldSchema> weighed() {
		return { FieldSchema{ "name", FieldType::Text, 32 }, FieldSchema{ "weight", FieldType::Integer } };
	}
};

// Whether an outline lists a pool row while its "all rows" switch is off: one that weighs something,
// or a note (an OutlineRowListedHook).
inline bool pool_row_listed(const opennova::editor::Document &, const opennova::editor::Node &row) {
	return row.kind == kPoolNote || static_cast<const PoolRow &>(row).weight != 0;
}

} // namespace editor_test
