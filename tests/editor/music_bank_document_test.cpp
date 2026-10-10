// Music banks (ADR 0046, round S23 lane A): a .sbf as a record document over the engine's own reader and from-scratch
// writer (sbf_read_bank, sbf_write_bank): the minted synth_gamemus.sbf (tests/fixtures/minimal_sbf_gen.cpp) read and
// written back byte for byte; a stream renamed, added, moved and removed, written and read back; the findings; a
// stream decoded as the preview player sounds it; play_sound of a stream through a session (refused for a bank with
// unsaved changes, a stream it has not); the blank. The retail leg (OPENNOVA_JO_DIR): every bank the install streams
// (its own pair and each expansion's) through the document and back, byte for byte.
#include <editor/blank/blank_factory.h>
#include <editor/documents/document_types.h>
#include <editor/documents/music_bank_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/text_document.h>
#include <editor/session/view/session_view.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <formats/sbf/sbf.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

constexpr NodeKind kBank = node_kind(MusicBankKind::Bank);
constexpr NodeKind kStream = node_kind(MusicBankKind::Stream);

std::vector<uint8_t> synth() {
	return test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/sbf/synth_gamemus.sbf");
}

Edit set_edit(NodeAddress address, const char *field, Value value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

bool has_code(const std::vector<Diagnostic> &findings, const char *code) {
	return std::any_of(findings.begin(), findings.end(), [&](const Diagnostic &d) { return d.code() == code; });
}

NodeAddress stream_at(const MusicBankDocument &bank, size_t place) {
	const MusicBankRow &row = *bank.bank_row();
	return {row.id, kStream, row.ids.lists[0][place].id};
}

bool load(MusicBankDocument &bank, const std::vector<uint8_t> &bytes, Diagnostic &error) {
	return bank.load_bytes(bytes, "music/gamemus.sbf", AssetKind::MusicBank, "jo", error);
}

std::vector<uint8_t> saved(const MusicBankDocument &bank) {
	const SerializeResult result = bank.serialize();
	return result.ok() ? std::vector<uint8_t>(result.text.begin(), result.text.end()) : std::vector<uint8_t>();
}

int test_reads_and_writes_back() {
	const std::vector<uint8_t> bytes = synth();
	TEST_EXPECT(!bytes.empty());
	if (test_io::is_lfs_pointer(bytes)) return 0;
	MusicBankDocument bank;
	Diagnostic error;
	TEST_EXPECT(load(bank, bytes, error));
	TEST_EXPECT(bank.bank_row() && bank.bank_row()->streams.size() == 13 && bank.issues().empty());
	TEST_EXPECT(saved(bank) == bytes);
	TEST_EXPECT(validate_music_bank_file(bank).empty());
	Value value;
	TEST_EXPECT(bank.get(stream_at(bank, 1), "name", value) && std::get<std::string>(value) == "TONE01");
	TEST_EXPECT(bank.get(stream_at(bank, 0), "seconds", value) && std::fabs(std::get<double>(value) - 2216.0 / 2.0 / 22050.0) < 1e-9);
	TEST_EXPECT(bank.get(stream_at(bank, 1), "chunks", value) && std::get<int64_t>(value) == 3);
	TEST_EXPECT(bank.record_title(stream_at(bank, 1)) == "1: TONE01");
	// A stream by its place alone, as the game names one: a name is no place.
	TEST_EXPECT(bank.stream_index("2") == 2 && bank.stream_index("12") == 12 && bank.stream_index("13") == -1 &&
	            bank.stream_index("TONE02") == -1 && bank.stream_index("02") == -1 && bank.stream_index("-1") == -1);
	// A stream as the preview player sounds it: TONE01's 9192 samples, 4596 stereo pairs at 22050 a second.
	lwf::WavPcm pcm;
	std::string message;
	TEST_EXPECT(music_stream_pcm(bytes, 1, pcm, message) && pcm.channels == 2 && pcm.sample_rate == 22050 &&
	            pcm.pcm16.size() == 9192 * 2 && pcm.loader_samples == 4596);
	TEST_EXPECT(!music_stream_pcm(bytes, 13, pcm, message) && !message.empty());
	return 0;
}

int test_edits() {
	MusicBankDocument bank;
	Diagnostic error;
	TEST_EXPECT(load(bank, synth(), error));
	const NodeAddress row{bank.bank_row()->id, kBank, 0};
	TEST_EXPECT(bank.apply(set_edit(stream_at(bank, 2), "name", std::string("RENAMED")), error));
	// A new stream: one chunk of silence, named NEWSTREAM; then moved to the front, and TONE12 removed.
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {row.row, kStream, 0};
	TEST_EXPECT(bank.apply(add, error) && bank.bank_row()->streams.size() == 14 && bank.bank_row()->streams[13].name == "NEWSTREAM");
	Edit move;
	move.operation = EditOperation::Move;
	move.address = stream_at(bank, 13);
	move.position = 0;
	TEST_EXPECT(bank.apply(move, error));
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = stream_at(bank, 13);
	TEST_EXPECT(bank.apply(remove, error));
	const std::vector<uint8_t> bytes = saved(bank);
	sbf::SbfFile read;
	std::string message;
	TEST_EXPECT(!bytes.empty() && sbf::sbf_read_bank(bytes.data(), bytes.size(), read, message));
	TEST_EXPECT(read.streams.size() == 13 && read.streams[0].name == "NEWSTREAM" && read.streams[3].name == "RENAMED" &&
	            read.streams[12].name == "TONE11" && read.streams[0].chunks.size() == 1);
	// A name of 16 characters fills the index's slot; refused: one past 16, one of a byte past plain ASCII; the
	// read-only fields; the bank keeps its row.
	TEST_EXPECT(bank.apply(set_edit(stream_at(bank, 1), "name", std::string(16, 'N')), error));
	const std::vector<uint8_t> sixteen = saved(bank);
	TEST_EXPECT(!sixteen.empty() && sbf::sbf_read_bank(sixteen.data(), sixteen.size(), read, message) &&
	            read.streams[1].name == std::string(16, 'N'));
	TEST_EXPECT(!bank.apply(set_edit(stream_at(bank, 1), "name", std::string(17, 'N')), error));
	TEST_EXPECT(!bank.apply(set_edit(stream_at(bank, 1), "name", std::string("caf\xC3\xA9")), error));
	TEST_EXPECT(!bank.apply(set_edit(stream_at(bank, 1), "chunks", int64_t(2)), error));
	TEST_EXPECT(!bank.apply(set_edit(row, "flags", int64_t(2)), error));
	Edit another;
	another.operation = EditOperation::Add;
	another.address = {0, kBank, 0};
	TEST_EXPECT(!bank.apply(another, error));
	return 0;
}

int test_findings() {
	MusicBankDocument bank;
	Diagnostic error;
	TEST_EXPECT(load(bank, synth(), error));
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {bank.bank_row()->id, kStream, 0};
	// Two streams of one name and a stream of none: no finding (the game never reads a name); a stream of no audio.
	TEST_EXPECT(bank.apply({set_edit(stream_at(bank, 2), "name", std::string("TONE01")), set_edit(stream_at(bank, 3), "name", std::string("")),
	                        add},
	                       error));
	const std::vector<Diagnostic> findings = validate_music_bank_file(bank);
	TEST_EXPECT(has_code(findings, "music_bank.stream_silent") && findings.size() == 1);
	// A name filling its 16 bytes with no terminator, as an encoder may write one: read, written back as it stands,
	// no finding (the game plays the bank).
	std::vector<uint8_t> bytes = synth();
	std::fill(bytes.begin() + 24, bytes.begin() + 40, uint8_t('Q'));
	MusicBankDocument full_name;
	TEST_EXPECT(load(full_name, bytes, error) && validate_music_bank_file(full_name).empty() && saved(full_name) == bytes);
	// Bytes past the last stream: listed, a save leaves them out.
	bytes = synth();
	bytes.push_back(0);
	MusicBankDocument trailing;
	TEST_EXPECT(load(trailing, bytes, error) && has_code(validate_music_bank_file(trailing), "music_bank.ignored_input"));
	TEST_EXPECT(saved(trailing).size() == bytes.size() - 1);
	// A bank the reader refuses: no document.
	bytes[0] = 'X';
	MusicBankDocument refused;
	TEST_EXPECT(!load(refused, bytes, error));
	return 0;
}

// play_sound of a stream through a session: the voice names the bank and the stream's place; refused for a bank with
// unsaved changes (the Shell streams the file) and for a stream it has not.
int test_session_play() {
	editor_test::TempProjectDir dir("opennova_editor_music_bank");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Music"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	const std::string path = "music/synth.sbf";
	TEST_EXPECT(editor_test::write_bytes(root + "/" + path, synth()));
	editor_test::handle_to_end(session, request::rescan());
	ActionOutcome played = editor_test::handle_to_end(session, request::play_stream(path, 2));
	TEST_EXPECT(played.done());
	const WorkspaceView::Sound &sound = session.view().workspace.sound;
	TEST_EXPECT(sound.voices.size() == 1 && sound.voices[0].path == path && sound.voices[0].stream == 2 &&
	            sound.words.find("TONE02") != std::string::npos);
	// A stream it has not, and a name (a stream is named by its place alone).
	TEST_EXPECT(!editor_test::handle_to_end(session, request::play_stream(path, 13)).done());
	EditorRequest by_name = request::play_stream(path, 0);
	by_name.values = {{"stream", "TONE02"}};
	TEST_EXPECT(!editor_test::handle_to_end(session, by_name).done());
	// Open and edited: refused until saved.
	editor_test::handle_to_end(session, request::open_document(path));
	const auto *bank = dynamic_cast<const MusicBankDocument *>(session.document_base_for(path));
	TEST_EXPECT(bank != nullptr);
	if (!bank) return 1;
	editor_test::handle_to_end(session, request::edit_record(path, {set_edit(stream_at(*bank, 2), "name", std::string("LOUDER"))}));
	TEST_EXPECT(!editor_test::handle_to_end(session, request::play_stream(path, 2)).done());
	editor_test::handle_to_end(session, request::save(path));
	played = editor_test::handle_to_end(session, request::play_stream(path, 2));
	TEST_EXPECT(played.done() && session.view().workspace.sound.voices[0].stream == 2);
	// The blank of the shell's music bank: no stream, opened clean.
	BlankRequest request;
	request.logical_name = "MENUMUS.SBF";
	request.role = "menumus_sbf";
	std::vector<uint8_t> blank;
	Diagnostic error;
	TEST_EXPECT(make_blank(request, AssetKind::MusicBank, blank, error));
	MusicBankDocument empty;
	TEST_EXPECT(empty.load_bytes(blank, "music/MENUMUS.SBF", AssetKind::MusicBank, "jo", error) &&
	            empty.bank_row()->streams.empty() && validate_music_bank_file(empty).empty() && saved(empty) == blank);
	return 0;
}

// The streams a music script plays (S23 B): the bank defines each stream by its place, scoped to its file's name; the
// script's plays name them in the bank of its name made .SBF, each present; a play past the bank's streams a warning
// in the game's words (it plays the bank's first).
int test_script_plays_streams() {
	editor_test::TempProjectDir dir("opennova_editor_music_streams");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Music streams"));
	const std::string root = session.view().project.root;
	TEST_EXPECT(editor_test::write_bytes(root + "/gamemus.sbf", synth()));
	TEST_EXPECT(editor_test::write_bytes(root + "/gamemus.bin",
	                                     test_io::read_file(std::string(test_paths_repo_root(__FILE__)) +
	                                                        "/fixtures/mus/synth_gamemus.bin")));
	editor_test::handle_to_end(session, request::rescan());
	const AssetGraph &graph = *session.view().findings.graph;
	TEST_EXPECT(graph.symbols_named(ReferenceKind::MusicStream, "12").size() == 1 &&
	            graph.symbols_named(ReferenceKind::MusicStream, "13").empty());
	size_t plays = 0, present = 0;
	for (const GraphEdge *edge : graph.references_of("gamemus.bin")) {
		if (edge->kind != ReferenceKind::MusicStream) continue;
		++plays;
		present += edge->scope == "GAMEMUS.SBF" && graph.resolve(*edge) == ReferenceStatus::Present;
	}
	TEST_EXPECT(plays == 9 && present == 9);
	// A play past the bank's 13 streams.
	editor_test::handle_to_end(session, request::open_document("gamemus.bin"));
	const TextDocument *text = text_of(*session.document_base_for("gamemus.bin"));
	TEST_EXPECT(text != nullptr);
	if (!text) return 1;
	const size_t at = text->text().find("play sound_1\n");
	TEST_EXPECT(at != std::string::npos);
	if (at == std::string::npos) return 1;
	editor_test::handle_to_end(session, request::edit_record("gamemus.bin",
	                                                         TextDocument::replace(text->span_at(at + 5, 7), "sound_20")));
	bool warned = false;
	for (const Diagnostic &d : session.view().findings.diagnostics)
		warned = warned || (d.code() == "reference.missing" && d.severity == DiagnosticSeverity::Warning &&
		                    d.message.find("first stream") != std::string::npos);
	TEST_EXPECT(warned);
	return 0;
}

// Every bank the install streams through the document and back, byte for byte.
int test_retail() {
	if (!retail::selected()) return 0;
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the music banks the install streams)");
		return 0;
	}
	size_t banks = 0, streams = 0;
	std::error_code ec;
	for (const auto &entry : std::filesystem::recursive_directory_iterator(std::filesystem::u8path(install), ec)) {
		std::string ext = entry.path().extension().u8string();
		for (char &c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
		if (ext != ".sbf") continue;
		const std::vector<uint8_t> bytes = test_io::read_file(entry.path().u8string());
		MusicBankDocument bank;
		Diagnostic error;
		TEST_EXPECT(bank.load_bytes(bytes, entry.path().filename().u8string(), AssetKind::MusicBank, "jo", error));
		TEST_EXPECT(bank.issues().empty() && saved(bank) == bytes);
		TEST_EXPECT(validate_music_bank_file(bank).empty());
		streams += bank.bank_row()->streams.size();
		++banks;
	}
	TEST_EXPECT(banks >= 2);
	std::printf("retail: %zu music banks, %zu streams through the document byte for byte\n", banks, streams);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failed = 0;
	failed += test_reads_and_writes_back();
	failed += test_edits();
	failed += test_findings();
	failed += test_session_play();
	failed += test_script_plays_streams();
	failed += test_retail();
	if (failed == 0) std::printf("editor music bank: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
