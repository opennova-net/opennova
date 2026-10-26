// The content store's part of the editor's session (engine/editor/session/content_controller.h, ADR
// 0069), driven through the session's requests as the NovaWorld window and the editor MCP send them,
// over the in-memory service (content_fake_service.h) and accounts kept in memory: every content
// request refused with no transport; a sign-in (a person's, refused over the wire) keeping the
// admin's session and listing the projects, a wrong password refused; a new project bound to the
// project with no revision and pushed as its first; a get into an empty folder, the project opened
// there with its files, and a folder that holds files refused; a change pushed from one and pulled
// into the other, its file read again; a push on a stale base refused until a pull; a conflict
// stopping a pull, settled by taking theirs, the pull going on, its standing counted again by the
// next check; a get given up on taking a waiting pull's conflicts with it; a working copy of
// another service's project refused; the history gone with the project; a check's standing; and the
// content section's JSON, which holds no session.

#include <cstdio>
#include <string>
#include <vector>

#include <base/content/content_manifest.h>
#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/sha256.h>
#include <editor/content_store/content_account.h>
#include <editor/content_store/content_state.h>
#include <editor/run/null_process_platform.h>
#include <editor/session/content_json.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include "common/test_expect.h"
#include "editor/content_fake_service.h"
#include "editor/editor_test_support.h"

namespace editor = opennova::editor;
namespace request = opennova::editor::request;
using namespace content_fake;

namespace {

struct Session {
	editor::NullProcessPlatform platform;
	editor::MemoryPreferencesStore preferences;
	editor::MemoryContentAccountStore accounts;
	editor::ProjectSession session{ platform, preferences };

	explicit Session(FakeService *service) {
		session.set_content_transport(service);
		session.set_content_accounts(&accounts);
	}
	// A request served and what it starts run to its end, as the command line sends one.
	void send(const editor::EditorRequest &r) {
		session.handle(r);
		session.run_operations();
	}
	const editor::ContentView &content() const { return session.view().content; }
	bool refused(const char *code) const {
		for (const editor::Diagnostic &d : session.outcome().findings)
			if (d.code() == code)
				return true;
		return false;
	}
	std::string read(const std::string &relative) const {
		std::string text, error;
		return opennova::io::read_file_text(
					   session.view().project.root + "/" + relative, text, error)
				? text
				: std::string("<missing>");
	}
};

} // namespace

static int test_offline() {
	editor::NullProcessPlatform platform;
	editor::MemoryPreferencesStore preferences;
	editor::ProjectSession session(platform, preferences);
	TEST_EXPECT(!session.view().content.available);
	for (const editor::EditorRequest &r : { request::content_check(), request::content_pull(),
				 request::content_push("x"), request::content_get("MOD", "somewhere") }) {
		session.handle(r);
		bool refused = false;
		for (const editor::Diagnostic &d : session.outcome().findings)
			refused = refused || d.code() == "content.refused";
		TEST_EXPECT(refused);
	}
	std::printf("offline: every content request refused with no transport\n");
	return 0;
}

static int test_sign_in() {
	FakeService service;
	Session s(&service);
	TEST_EXPECT(s.content().available && !s.content().signed_in);
	// Over the wire, refused by its kind: a password is a person's.
	opennova::io::JsonValue wire;
	std::string error;
	TEST_EXPECT(opennova::io::json_parse(
			"{\"kind\": \"content_sign_in\", \"username\": \"taylor\", \"password\": \"pw\"}", wire,
			error));
	const opennova::io::JsonValue answer = s.session.handle_json(wire);
	TEST_EXPECT(answer.get_string("error", "").find("person") != std::string::npos &&
			!s.content().signed_in);
	// A wrong password, then the right one.
	s.send(request::content_sign_in(kServer, "taylor", "wrong"));
	TEST_EXPECT(!s.content().signed_in && !s.content().calling);
	s.send(request::content_sign_in(kServer, "taylor", "pw"));
	TEST_EXPECT(s.content().signed_in && s.content().username == "taylor" &&
			s.content().role == "admin");
	TEST_EXPECT(s.accounts.load().cookie == kAdminCookie && s.content().projects.size() == 1 &&
			s.content().projects[0].key == "MOD");
	// The view's JSON says who is signed in, and never the session.
	const std::string text = opennova::io::json_write(editor::content_section(s.session.view()));
	TEST_EXPECT(text.find("taylor") != std::string::npos &&
			text.find("admin") != std::string::npos &&
			text.find("opennova_session") == std::string::npos);
	s.send(request::content_sign_out());
	TEST_EXPECT(!s.content().signed_in && s.accounts.load().cookie.empty() &&
			s.accounts.load().server == kServer);
	std::printf("sign-in: a person's, the admin's session kept and the projects listed, signed out "
				"again\n");
	return 0;
}

static int test_share_through_sessions() {
	test_temp::TempDir dir("opennova_content_session");
	FakeService service;
	Session first(&service), second(&service);
	first.send(request::content_sign_in(kServer, "taylor", "pw"));
	second.send(request::content_sign_in(kServer, "alex", "pw"));

	// A new project, bound to the project with no revision, pushed as its first.
	first.send(request::new_project(dir.file("First"), "Mod"));
	TEST_EXPECT(first.session.view().project.open);
	TEST_EXPECT(put(dir.file("First") + "/defs/a.def", "a1"));
	first.send(request::content_bind("MOD"));
	TEST_EXPECT(
			first.content().bound && first.content().key == "MOD" && first.content().revision == 0);
	first.send(request::content_check());
	TEST_EXPECT(first.content().checked && first.content().head_known &&
			first.content().head == 0 && !first.content().changes.empty());
	first.send(request::content_push("First"));
	TEST_EXPECT(
			service.head == 1 && first.content().revision == 1 && first.content().changes.empty());

	// Got into an empty folder, opened there; a folder that holds files refused.
	TEST_EXPECT(put(dir.file("Full") + "/x.txt", "x"));
	second.send(request::content_get("MOD", dir.file("Full")));
	TEST_EXPECT(second.refused("content.refused"));
	second.send(request::content_get("MOD", dir.file("Second")));
	TEST_EXPECT(second.session.view().project.open);
	TEST_EXPECT(second.content().bound && second.content().revision == 1 &&
			second.read("defs/a.def") == "a1");

	// A change pushed from the first, pulled into the second.
	TEST_EXPECT(put(dir.file("First") + "/defs/a.def", "a2"));
	first.send(request::content_push("a2"));
	TEST_EXPECT(service.head == 2);
	second.send(request::content_pull());
	TEST_EXPECT(second.content().revision == 2 && second.read("defs/a.def") == "a2");

	// The second's push on its base, then the first's on a stale one: refused until it pulls.
	TEST_EXPECT(put(dir.file("Second") + "/defs/b.def", "b"));
	second.send(request::content_push("b"));
	TEST_EXPECT(service.head == 3);
	TEST_EXPECT(put(dir.file("First") + "/defs/c.def", "c"));
	first.send(request::content_push("c"));
	// The push's answer comes back as its operation finishes, in the view.
	TEST_EXPECT(service.head == 3 && first.content().head == 3 &&
			first.content().last.find("pull, then push") != std::string::npos);
	first.send(request::content_pull());
	TEST_EXPECT(first.content().revision == 3 && first.read("defs/b.def") == "b" &&
			first.read("defs/c.def") == "c");
	first.send(request::content_push("c"));
	TEST_EXPECT(service.head == 4);

	// Both change one file: the pull stops on it, theirs is taken, and the pull goes on.
	TEST_EXPECT(put(dir.file("First") + "/defs/a.def", "a-first"));
	first.send(request::content_push("a first"));
	TEST_EXPECT(put(dir.file("Second") + "/defs/a.def", "a-second"));
	second.send(request::content_check());
	TEST_EXPECT(second.content().checked && second.content().changes.size() == 1);
	second.send(request::content_pull());
	TEST_EXPECT(second.content().conflicts.size() == 1 &&
			second.content().conflicts[0].path == "defs/a.def" &&
			second.content().last.find("stopped") != std::string::npos);
	second.send(request::content_pull());
	TEST_EXPECT(second.refused("content.refused") && second.content().conflicts.size() == 1);
	// A get given up on (a project the store lacks) took the waiting pull's place: its conflicts go
	// with it, and a pull plans again.
	second.send(request::content_get("NOPE", dir.file("Third")));
	TEST_EXPECT(second.content().conflicts.empty());
	second.send(request::content_pull());
	TEST_EXPECT(second.content().conflicts.size() == 1);
	second.send(request::content_resolve("defs/a.def", editor::Resolution::TakeTheirs));
	TEST_EXPECT(second.content().conflicts.empty());
	second.send(request::content_pull());
	TEST_EXPECT(second.content().revision == 5 && second.read("defs/a.def") == "a-first" &&
			second.read("defs/c.def") == "c");
	// The check's changes went with the pull that took theirs: counted again by the next check.
	TEST_EXPECT(!second.content().checked && second.content().changes.empty());
	// The JSON's standing and pull.
	const opennova::io::JsonValue section = editor::content_section(second.session.view());
	TEST_EXPECT(section.get("binding")->get_string("key", "") == "MOD" &&
			int(section.get("binding")->get_number("revision", 0)) == 5 &&
			section.get("pull")->get("conflicts")->array.empty());
	// A release of the second's revision, built on the service; a version that is none refused.
	second.send(request::content_release("x.y"));
	TEST_EXPECT(second.refused("content.refused") && service.release_version.empty());
	second.send(request::content_release("0.2.0"));
	TEST_EXPECT(service.release_version == "0.2.0" && service.release_revision == 5 &&
			second.content().last.find("MOD 0.2.0") != std::string::npos);
	// A change discarded, then the file back as revision 1 has it: a change to push.
	TEST_EXPECT(put(dir.file("Second") + "/defs/a.def", "a-mine"));
	second.send(request::content_restore("defs/a.def"));
	TEST_EXPECT(second.read("defs/a.def") == "a-first" &&
			second.content().last.find("Discarded the changes to defs/a.def") != std::string::npos);
	second.send(request::content_restore("defs/a.def", 1));
	TEST_EXPECT(second.read("defs/a.def") == "a1" && second.content().revision == 5 &&
			second.content().last.find("back as revision 1") != std::string::npos);
	second.send(request::content_restore("../outside.def"));
	TEST_EXPECT(second.refused("content.refused"));
	// The history, newest first.
	second.send(request::content_history());
	TEST_EXPECT(second.content().history.size() == 5 && second.content().history[0].number == 5 &&
			second.content().history[4].message == "r1");
	// A working copy of another service's project (a release build's folder opened in a debug
	// build): refused; and the history goes with the project as it closes.
	const editor::ContentPaths second_state =
			editor::ContentPaths::for_project(editor::ProjectPaths::for_root(dir.file("Second")));
	editor::ContentBinding elsewhere;
	std::string error;
	TEST_EXPECT(editor::read_content_binding(second_state, elsewhere, error));
	elsewhere.server = "https://elsewhere.test";
	TEST_EXPECT(editor::write_content_binding(second_state, elsewhere, error));
	second.send(request::content_pull());
	TEST_EXPECT(second.refused("content.refused") && second.content().revision == 5);
	second.send(request::close_project());
	TEST_EXPECT(!second.session.view().project.open && second.content().history.empty());
	std::printf("share: bound and pushed, got and opened, pulled, a stale push refused, a conflict "
				"taken, a release asked for, a change discarded and a file restored, the history\n");
	return 0;
}

// An expansion's project got: its base game's project comes along where its project file names it
// (base_project "../MOD"), each folder a working copy of its own project.
static int test_get_expansion() {
	test_temp::TempDir dir("opennova_content_session_expansion");
	FakeService service;
	const auto file_of = [&](const char *path, const std::string &bytes) {
		const std::string sha = opennova::io::sha256_hex(bytes.data(), bytes.size());
		service.blobs[sha] = bytes;
		return ContentFile{ path, sha, bytes.size() };
	};
	service.manifests[1] = opennova::content::encode_manifest({ file_of("defs/a.def", "base") });
	service.head = 1;
	service.expansion = true;
	service.expansion_manifests[1] = opennova::content::encode_manifest(
			{ file_of("project.opennova", "{\"expansion\": {\"base_project\": \"../MOD\"}}"),
					file_of("defs/e.def", "expansion") });
	service.expansion_head = 1;
	Session s(&service);
	s.send(request::content_sign_in(kServer, "taylor", "pw"));
	TEST_EXPECT(s.content().projects.size() == 2 && s.content().projects[1].base == "MOD");
	s.send(request::content_get("EXP", dir.file("work/EXP")));
	std::string text, error;
	TEST_EXPECT(opennova::io::read_file_text(dir.file("work/EXP/defs/e.def"), text, error) &&
			text == "expansion");
	TEST_EXPECT(opennova::io::read_file_text(dir.file("work/MOD/defs/a.def"), text, error) &&
			text == "base");
	for (const auto &[folder, key] : { std::pair<const char *, const char *>{ "work/EXP", "EXP" },
				 { "work/MOD", "MOD" } }) {
		editor::ContentBinding binding;
		TEST_EXPECT(editor::read_content_binding(
							editor::ContentPaths::for_project(
									editor::ProjectPaths::for_root(dir.file(folder))),
							binding, error) &&
				binding.key == key && binding.revision == 1);
	}
	std::printf("expansion: got with its base game's project beside it\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_offline();
	failures += test_sign_in();
	failures += test_share_through_sessions();
	failures += test_get_expansion();
	if (failures == 0)
		std::printf("content_session: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
