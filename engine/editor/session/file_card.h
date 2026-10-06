#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/asset_kind.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/value.h>

namespace opennova::editor {

struct AssetScan;
struct GraphEdge;
struct SessionView;

// A project file as a modder asks after it (the UX round's project lane): Files' card for a file the editor
// opens no document of (a wave, a sound bank, a particle file, an AI profile) and for any file's References,
// and the editor MCP's file_card query. What it is (its kind's words, AssetKindRow::about), where a build puts
// it, where it came from, what it names (each reference with whether the project resolves it and the file it
// does) and who names it, each a Go to; a wave's sound as the game decodes it (lwf::wav_decode_pcm16: its
// channels, rate and length), which the Shell plays (play_sound).
struct FileCard {
	bool found = false;
	std::string path; // project-relative
	std::string name; // logical
	AssetKind kind = AssetKind::Unknown;
	std::string kind_label;
	std::string about;
	uint64_t size = 0;
	std::string build; // where a build puts it, in words
	std::string imported_from;
	bool opens = false; // the editor opens a document of it
	// A wave: what the game's decode makes of it, read once for the file's size and last write (a card made
	// again over the same file takes it as it is), and not for a file past kWaveCardBytes.
	struct Sound {
		bool decoded = false;
		std::string error; // why it does not decode
		uint32_t rate = 0;
		uint16_t channels = 0;
		double seconds = 0.0;
		uint64_t size = 0;     // the file's, as read
		int64_t modified = 0;  // its last-write ticks, as read (0: unknown, read again)
	};
	bool wave = false;
	Sound sound;
	// What it names: the field's words, the record's, the value, whether the project has it and the file it
	// resolves to ("" for none, or a name no file is: a symbol), that file a wave.
	struct Named {
		std::string field;
		std::string record;
		std::string value;
		ReferenceStatus status = ReferenceStatus::NotAReference;
		std::string file;
		bool wave = false;
		ReferenceTarget target;
	};
	std::vector<Named> names;
	// Who names it, or what it defines: the naming file, its record and field's words.
	struct User {
		std::string file;
		std::string record;
		std::string field;
		ReferenceTarget target;
	};
	std::vector<User> named_by;
	// The project's references are being read (a validation runs, or none has read them yet): what it
	// names and who names it are the graph's as far as it has read, and may grow (the demo round's bug 9:
	// "Named by (0)" while an import's files were read).
	bool reading = false;
};

// Who names a project file, in one look (the deep-integration plan's DI-05): the card's Named by, made
// without a card (no wave read, nothing it names), for the Inspector's view of a file with nothing selected,
// the document toolbars' Used by chip and the wire's used_by query. Its usages (graph/reference_queries'
// file_uses: the records naming it, then those naming what it defines) grouped by the file they are in, in
// the graph's order, each a place in words (edge_place_words: the record in its type's words, the field by
// its label) with where Go to takes it (usage_target); and one hop further where the naming record is a
// definition itself (oncrate1.3di named by the item Wooden supply crate in items.def, which onjo_m1.bms
// places six times), those uses grouped by their file too.
struct FileUsers {
	bool found = false;
	std::string path; // project-relative
	std::string name; // logical
	// A place that names it (or names what names it).
	struct Line {
		std::string file;  // the naming file, project-relative
		std::string words; // the record and its field, in words ("Wooden supply crate - Graphic")
		std::string field; // the field's id
		ReferenceTarget target;
	};
	struct Group {
		std::string file;
		std::string name; // the file's logical name
		std::vector<Line> lines;
	};
	struct Use {
		Line line;
		std::vector<Group> further; // one hop further, by file
		size_t further_count = 0;
	};
	struct UseGroup {
		std::string file;
		std::string name;
		std::vector<Use> uses;
	};
	std::vector<UseGroup> groups;
	size_t count = 0;         // its uses (the first hop's lines)
	size_t further_count = 0; // the lines one hop further
	// The project's references are being read (FileCard::reading): the lists are the graph's as far as it has
	// read, and may grow.
	bool reading = false;
};

// Who names `path` (a project-relative path or a logical name); found false for a file the project lacks.
FileUsers file_users(const SessionView &view, const std::string &path);
// How many uses it has (FileUsers::count, AssetGraph::usages_of), nothing worded: the toolbars' chip.
size_t file_use_count(const SessionView &view, const std::string &path);
// Its wire form, the used_by query's: {found, path, name, count, further_count, reading?, files [{file, name,
// uses [{words, field, file, locator?, editable, further [{file, name, uses [{words, field, file, locator?,
// editable}]}]}]}]}; a use's file and locator are where Go to opens it (open_document's path and locator, or
// show_in_files's path where `editable` is false).
io::JsonValue file_users_json(const FileUsers &users);

// The most of a wave a card reads to say what it is (the game's own are a few hundred KB): a larger file is
// said to be too large, nothing read.
inline constexpr uint64_t kWaveCardBytes = uint64_t(32) << 20;

// The card of the project file `path` (a project-relative path or a logical name); found false for none.
// `known`, a card's sound read before: taken as it is while the file's size and last write are those it was
// read at, else read again.
FileCard file_card(const SessionView &view, const std::string &path, const FileCard::Sound *known = nullptr);
// Its wire form: {found, path, name, kind, kind_label, about, size, build, imported_from, opens, sound?
// {decoded, error?, rate, channels, seconds}, names [{field, record, value, status, file, wave}], named_by
// [{file, record, field}], reading? (true while the project's references are being read)}.
io::JsonValue file_card_json(const FileCard &card);

// A graph edge's field by the name the Inspector shows it under (its type's schema, which no file's
// content changes), its id where the file's kind has no document type or the schema no such field.
std::string edge_field_words(const AssetScan &scan, const GraphEdge &edge);

// A reference's state in words: "found in the project", "missing", "not checked".
const char *reference_status_words(ReferenceStatus status);

} // namespace opennova::editor
