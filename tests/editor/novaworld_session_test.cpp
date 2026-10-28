// NovaWorld's part of the editor's session (engine/editor/session/novaworld_controller.h, ADR
// 0069), driven through the session's requests as the NovaWorld window and the editor MCP send
// them, over the in-memory service (novaworld_fake_service.h) and accounts kept in memory: every
// NovaWorld request refused with no transport; a sign-in (a person's, refused over the wire)
// keeping the admin's session and listing the projects, a wrong password refused; a new project put
// on NovaWorld as the empty project there (its key written in its project file, the folder its
// working copy at revision 0, its files counted as changes) and pushed as its first; a get into an
// empty folder, the project opened there a working copy of the project its file names, and a folder
// that holds files refused; a change pushed from one and pulled into the other, its file read
// again; a push on a stale base refused until a pull; a conflict stopping a pull, settled by taking
// theirs, the pull going on, its standing counted again by the next check; a get given up on taking
// a waiting pull's conflicts with it; the history gone with the project; an admin's release of a
// revision, published on the admin's Products page; what a put makes of a project found as a check
// asks; a project made for a NovaWorld project (Set up...) set up as its working copy as it opens, at
// no revision where its files are not the head's, its push waiting for a pull; the store reset under
// a working copy, set up again by the check that found it; a player's new project put on NovaWorld
// (a game, a mod of a game it names; public unless asked private) and pushed, released by its owner
// (submitted for approval on its page under Projects, each refusal of the service's said), a mod whose name the
// service's rule refuses refused up front, one whose short name another mod holds refused by the
// service, an empty project put on refused where it has revisions, is another's, or is no game or mod
// as the project is, of its game and short name; a player's push and release of another's project
// refused; a project file naming a key the service shows the account none of, said so; a mod got
// whose base game's project is a retail game's or unreadable, its base left to the game install; an
// account change giving a put up; a get of a project with no revision yet refused; and the novaworld
// section's JSON, which holds no session.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <editor/session/base_setup.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <base/project/project_manifest.h>
#include <editor/novaworld/novaworld_account.h>
#include <editor/novaworld/novaworld_jobs.h>
#include <editor/novaworld/novaworld_state.h>
#include <editor/project/project_document.h>
#include <editor/run/null_process_platform.h>
#include <editor/session/novaworld_json.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_operation.h>
#include <editor/session/view/session_view.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/minted_install.h"
#include "editor/novaworld_fake_service.h"

namespace editor = opennova::editor;
namespace request = opennova::editor::request;
using namespace novaworld_fake;

namespace {

struct Session {
	editor::NullProcessPlatform platform;
	editor::MemoryPreferencesStore preferences;
	editor::MemoryNovaWorldAccountStore accounts;
	editor::ProjectSession session{ platform, preferences };

	explicit Session(FakeService *service) {
		session.set_novaworld_transport(service);
		session.set_novaworld_accounts(&accounts);
	}
	// A request served and what it starts run to its end, as the command line sends one.
	void send(const editor::EditorRequest &r) {
		session.handle(r);
		session.run_operations();
	}
	const editor::NovaWorldView &novaworld() const { return session.view().novaworld; }
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
	// Whether Output says `words`.
	bool said(const std::string &words) const {
		for (const std::string &line : session.view().activity.output)
			if (line.find(words) != std::string::npos)
				return true;
		return false;
	}
};

// The folder's working copy on the fake service, false for none.
bool copy_at(const std::string &folder, editor::NovaWorldWorkingCopy &out) {
	std::string error;
	return editor::read_novaworld_state(
			editor::NovaWorldPaths::for_project(editor::ProjectPaths::for_root(folder), kServer),
			out, error);
}

// A new project made for the NovaWorld project `key` (the welcome page's Set up...).
editor::EditorRequest new_project_for(
		const std::string &dir, const std::string &title, const std::string &key) {
	editor::EditorRequest made = request::new_project(dir, title);
	made.project_key = key;
	return made;
}

// `bytes` as a file of the service's MOD at its next revision, with the files before it.
void push_to_mod(
		FakeService &service, const std::vector<std::pair<const char *, std::string>> &files) {
	std::vector<ProjectFile> listed;
	for (const auto &[path, bytes] : files) {
		const std::string sha = opennova::io::sha256_hex(bytes.data(), bytes.size());
		service.blobs[sha] = bytes;
		listed.push_back(ProjectFile{ path, sha, bytes.size() });
	}
	service.manifests[++service.head] = opennova::project::encode_manifest(listed);
}

} // namespace

static int test_offline() {
	editor::NullProcessPlatform platform;
	editor::MemoryPreferencesStore preferences;
	editor::ProjectSession session(platform, preferences);
	TEST_EXPECT(!session.view().novaworld.available);
	for (const editor::EditorRequest &r : { request::novaworld_check(), request::novaworld_pull(),
				 request::novaworld_push("x"), request::novaworld_get("MOD", "somewhere"),
				 request::novaworld_put_empty("MOD") }) {
		session.handle(r);
		bool refused = false;
		for (const editor::Diagnostic &d : session.outcome().findings)
			refused = refused || d.code() == "novaworld.refused";
		TEST_EXPECT(refused);
	}
	std::printf("offline: every NovaWorld request refused with no transport\n");
	return 0;
}

static int test_sign_in() {
	FakeService service;
	Session s(&service);
	TEST_EXPECT(s.novaworld().available && !s.novaworld().signed_in);
	// Over the wire, refused by its kind: a password is a person's.
	opennova::io::JsonValue wire;
	std::string error;
	TEST_EXPECT(opennova::io::json_parse(
			"{\"kind\": \"novaworld_sign_in\", \"username\": \"taylor\", \"password\": \"pw\"}", wire,
			error));
	const opennova::io::JsonValue answer = s.session.handle_json(wire);
	TEST_EXPECT(answer.get_string("error", "").find("person") != std::string::npos &&
			!s.novaworld().signed_in);
	// A wrong password, then the right one.
	s.send(request::novaworld_sign_in(kServer, "taylor", "wrong"));
	TEST_EXPECT(!s.novaworld().signed_in && !s.novaworld().calling);
	s.send(request::novaworld_sign_in(kServer, "taylor", "pw"));
	TEST_EXPECT(s.novaworld().signed_in && s.novaworld().username == "taylor" &&
			s.novaworld().role == "admin");
	TEST_EXPECT(s.accounts.load().cookie == kAdminCookie && s.novaworld().projects.size() == 1 &&
			s.novaworld().projects[0].key == "MOD" && s.novaworld().projects[0].kind == "game" &&
			s.novaworld().projects[0].can_push);
	// The view's JSON says who is signed in, and never the session.
	const std::string text = opennova::io::json_write(editor::novaworld_section(s.session.view()));
	TEST_EXPECT(text.find("taylor") != std::string::npos &&
			text.find("admin") != std::string::npos &&
			text.find("opennova_session") == std::string::npos);
	// A page of the site opened in the browser (the open_externally view event).
	s.send(request::novaworld_open_site("register"));
	const auto held = s.session.view().events.held();
	TEST_EXPECT(!held.empty() && held.back().kind == editor::ViewEventKind::OpenExternally &&
			held.back().path == std::string(kServer) + "/register");
	s.send(request::novaworld_sign_out());
	TEST_EXPECT(!s.novaworld().signed_in && s.accounts.load().cookie.empty() &&
			s.accounts.load().server == kServer);
	// A session the service ended (401 on any call, here the lists): signed out, said so.
	s.send(request::novaworld_sign_in(kServer, "taylor", "pw"));
	service.sessions_ended = true;
	s.send(request::novaworld_check());
	TEST_EXPECT(!s.novaworld().signed_in && s.accounts.load().cookie.empty() &&
			s.novaworld().last == "Your NovaWorld session ended: sign in again.");
	service.sessions_ended = false;
	std::printf("sign-in: a person's, the admin's session kept and the projects listed, signed out "
				"again\n");
	return 0;
}

static int test_share_through_sessions() {
	test_temp::TempDir dir("opennova_novaworld_session");
	FakeService service;
	Session first(&service), second(&service);
	first.send(request::novaworld_sign_in(kServer, "taylor", "pw"));
	second.send(request::novaworld_sign_in(kServer, "alex", "pw"));

	// A new project, put on NovaWorld as the empty MOD: its key in its project file, the folder its
	// working copy at revision 0, every file a change, "First revision" pushed. What a put makes of
	// it is found as a check asks (the Put dialog's, as it opens), not on every reply.
	first.send(request::new_project(dir.file("First"), "Mod"));
	TEST_EXPECT(first.session.view().project.open && first.novaworld().key.empty() &&
			first.novaworld().put.kind.empty());
	first.send(request::novaworld_check());
	TEST_EXPECT(first.novaworld().put.kind == "game" && first.novaworld().put.game == "jo");
	TEST_EXPECT(put(dir.file("First") + "/defs/a.def", "a1"));
	first.send(request::novaworld_put_empty("MOD"));
	TEST_EXPECT(
			first.read("project.opennova").find("\"novaworld\": \"MOD\"") != std::string::npos &&
			first.session.view().project.document->novaworld == "MOD");
	TEST_EXPECT(first.novaworld().key == "MOD" && first.novaworld().tracked &&
			first.novaworld().revision == 0 && first.novaworld().remote_known &&
			first.novaworld().remote.can_push);
	TEST_EXPECT(first.novaworld().checked && first.novaworld().head_known &&
			first.novaworld().head == 0 &&
			first.novaworld().changes.size() >= 2); // the project file and defs/a.def at least
	first.send(request::novaworld_push("First revision"));
	TEST_EXPECT(
			service.head == 1 && first.novaworld().revision == 1 && first.novaworld().changes.empty());
	// Put on again: refused, it is on NovaWorld already.
	first.send(request::novaworld_put_empty("MOD"));
	TEST_EXPECT(first.refused("novaworld.refused"));

	// Got into an empty folder, opened there, its project file naming MOD: a working copy at
	// revision 1; a folder that holds files refused.
	TEST_EXPECT(put(dir.file("Full") + "/x.txt", "x"));
	second.send(request::novaworld_get("MOD", dir.file("Full")));
	TEST_EXPECT(second.refused("novaworld.refused"));
	second.send(request::novaworld_get("MOD", dir.file("Second")));
	TEST_EXPECT(second.session.view().project.open);
	TEST_EXPECT(second.novaworld().key == "MOD" && second.novaworld().tracked &&
			second.novaworld().revision == 1 && second.read("defs/a.def") == "a1");

	// A change pushed from the first, pulled into the second.
	TEST_EXPECT(put(dir.file("First") + "/defs/a.def", "a2"));
	first.send(request::novaworld_push("a2"));
	TEST_EXPECT(service.head == 2);
	second.send(request::novaworld_pull());
	TEST_EXPECT(second.novaworld().revision == 2 && second.read("defs/a.def") == "a2");

	// The second's push on its base, then the first's on a stale one: refused until it pulls.
	TEST_EXPECT(put(dir.file("Second") + "/defs/b.def", "b"));
	second.send(request::novaworld_push("b"));
	TEST_EXPECT(service.head == 3);
	TEST_EXPECT(put(dir.file("First") + "/defs/c.def", "c"));
	first.send(request::novaworld_push("c"));
	// The push's answer comes back as its operation finishes, in the view.
	TEST_EXPECT(service.head == 3 && first.novaworld().head == 3 &&
			first.novaworld().last.find("pull, then push") != std::string::npos);
	first.send(request::novaworld_pull());
	TEST_EXPECT(first.novaworld().revision == 3 && first.read("defs/b.def") == "b" &&
			first.read("defs/c.def") == "c");
	first.send(request::novaworld_push("c"));
	TEST_EXPECT(service.head == 4);

	// Both change one file: the pull stops on it, theirs is taken, and the pull goes on.
	TEST_EXPECT(put(dir.file("First") + "/defs/a.def", "a-first"));
	first.send(request::novaworld_push("a first"));
	TEST_EXPECT(put(dir.file("Second") + "/defs/a.def", "a-second"));
	second.send(request::novaworld_check());
	TEST_EXPECT(second.novaworld().checked && second.novaworld().changes.size() == 1);
	// The files read again (a save, an import, a refresh): the changes counted again by the next check.
	second.send(request::rescan());
	second.session.poll(); // the editor's next frame
	TEST_EXPECT(!second.novaworld().checked);
	second.send(request::novaworld_pull());
	TEST_EXPECT(second.novaworld().conflicts.size() == 1 &&
			second.novaworld().conflicts[0].path == "defs/a.def" &&
			second.novaworld().last.find("stopped") != std::string::npos);
	second.send(request::novaworld_pull());
	TEST_EXPECT(second.refused("novaworld.refused") && second.novaworld().conflicts.size() == 1);
	// A get given up on (a project the store lacks) took the waiting pull's place: its conflicts go
	// with it, and a pull plans again.
	second.send(request::novaworld_get("NOPE", dir.file("Third")));
	TEST_EXPECT(second.novaworld().conflicts.empty());
	second.send(request::novaworld_pull());
	TEST_EXPECT(second.novaworld().conflicts.size() == 1);
	second.send(request::novaworld_resolve("defs/a.def", editor::Resolution::TakeTheirs));
	// The last conflict settled, the pull goes on by itself.
	TEST_EXPECT(second.novaworld().conflicts.empty() && second.novaworld().revision == 5);
	second.send(request::novaworld_pull());
	TEST_EXPECT(second.novaworld().revision == 5 && second.read("defs/a.def") == "a-first" &&
			second.read("defs/c.def") == "c");
	// The check's changes went with the pull that took theirs: counted again by the next check.
	TEST_EXPECT(!second.novaworld().checked && second.novaworld().changes.empty());
	// The JSON's working copy, project and pull.
	const opennova::io::JsonValue section = editor::novaworld_section(second.session.view());
	TEST_EXPECT(section.get("working_copy")->get_string("key", "") == "MOD" &&
			section.get("working_copy")->get_bool("tracked", false) &&
			int(section.get("working_copy")->get_number("revision", 0)) == 5 &&
			section.get("project")->get_bool("can_push", false) &&
			section.get("pull")->get("conflicts")->array.empty());
	// A release of the second's revision, built on the service, an admin's published on the admin's
	// Products page; a version that is none refused.
	TEST_EXPECT(second.novaworld().remote.can_release);
	second.send(request::novaworld_release("x.y"));
	TEST_EXPECT(second.refused("novaworld.refused") && service.release_version.empty());
	second.send(request::novaworld_release("0.2.0"));
	TEST_EXPECT(service.release_key == "MOD" && service.release_version == "0.2.0" &&
			service.release_revision == 5 &&
			second.novaworld().last.find("MOD 0.2.0") != std::string::npos &&
			second.novaworld().last.find("the admin's Products page") != std::string::npos &&
			second.novaworld().last.find("https://novaworld.test/admin/products/MOD") !=
					std::string::npos);
	// A change discarded, then the file back as revision 1 has it: a change to push.
	TEST_EXPECT(put(dir.file("Second") + "/defs/a.def", "a-mine"));
	second.send(request::novaworld_restore("defs/a.def"));
	TEST_EXPECT(second.read("defs/a.def") == "a-first" &&
			second.novaworld().last.find("Discarded the changes to defs/a.def") != std::string::npos);
	second.send(request::novaworld_restore("defs/a.def", 1));
	TEST_EXPECT(second.read("defs/a.def") == "a1" && second.novaworld().revision == 5 &&
			second.novaworld().last.find("back as revision 1") != std::string::npos);
	second.send(request::novaworld_restore("../outside.def"));
	TEST_EXPECT(second.refused("novaworld.refused"));
	// The history, newest first.
	second.send(request::novaworld_history());
	TEST_EXPECT(second.novaworld().history.size() == 5 && second.novaworld().history[0].number == 5 &&
			second.novaworld().history[4].message == "r1");
	// Another service's state beside it is that service's: the working copy here stays this one's.
	const editor::NovaWorldPaths elsewhere = editor::NovaWorldPaths::for_project(
			editor::ProjectPaths::for_root(dir.file("Second")), "https://elsewhere.test");
	std::string error;
	TEST_EXPECT(editor::write_novaworld_state(elsewhere, { "MOD", 99 }, error));
	second.send(request::novaworld_pull());
	TEST_EXPECT(second.novaworld().revision == 5 &&
			second.novaworld().last.find("at the head") != std::string::npos);
	// And the history goes with the project as it closes.
	second.send(request::close_project());
	TEST_EXPECT(!second.session.view().project.open && second.novaworld().history.empty() &&
			second.novaworld().key.empty() && !second.novaworld().tracked);
	std::printf("share: put on and pushed, got and opened, pulled, a stale push refused, a "
				"conflict taken, a release asked for, a change discarded and a file restored, the "
				"history\n");
	return 0;
}

// A project made for a NovaWorld project (the welcome page's Set up..., its form remembering the
// key): the key in its project file, and the folder set up as its working copy as it opens, the
// form's key gone with the project made; a key that is none refused, nothing made. Its files (a new
// project's) are not the head's, so it holds no revision: a push waits for a pull, which brings the
// head's files in beside its own. Then the store reset under it: the check that finds it says so,
// sets the folder up again at the new head, and counts every file as a change.
static int test_set_up_and_reset() {
	test_temp::TempDir dir("opennova_novaworld_session_set_up");
	FakeService service;
	push_to_mod(service, { { "defs/a.def", "a1" } });
	push_to_mod(service, { { "defs/a.def", "a2" } });
	Session s(&service);
	s.send(request::novaworld_sign_in(kServer, "taylor", "pw"));
	opennova::io::JsonValue form = opennova::io::JsonValue::make_object();
	form.set("novaworld", opennova::io::json_string("MOD"));
	opennova::io::JsonValue change = opennova::io::JsonValue::make_object();
	change.set("new_project", form);
	s.send(request::set_workspace(opennova::io::json_write(change)));
	TEST_EXPECT(s.session.view().workspace.new_project.novaworld == "MOD");
	form.set("novaworld", opennova::io::json_string("mod"));
	change.set("new_project", form);
	s.send(request::set_workspace(opennova::io::json_write(change)));
	TEST_EXPECT(s.session.view().workspace.new_project.novaworld == "MOD"); // refused: no key

	s.send(new_project_for(dir.file("Bad"), "Mod", "not a key"));
	editor::ProjectDocument unmade;
	editor::Diagnostic why;
	TEST_EXPECT(!s.session.view().project.open &&
			!editor::load_project_document(dir.file("Bad") + "/project.opennova", unmade, why));
	s.send(new_project_for(dir.file("Mod"), "Mod", "MOD"));
	TEST_EXPECT(s.session.view().project.open &&
			s.session.view().project.document->novaworld == "MOD" &&
			s.session.view().workspace.new_project.novaworld.empty());
	editor::NovaWorldWorkingCopy copy;
	TEST_EXPECT(copy_at(dir.file("Mod"), copy) && copy.key == "MOD" && copy.revision == 0);
	TEST_EXPECT(s.novaworld().tracked && s.novaworld().revision == 0 &&
			s.novaworld().remote_known && s.novaworld().head == 2 &&
			s.said("working copy on novaworld.test, but its files differ from revision 2"));
	TEST_EXPECT(put(dir.file("Mod") + "/defs/b.def", "b"));
	s.send(request::novaworld_push("Mine"));
	TEST_EXPECT(service.head == 2 && s.novaworld().last.find("pull, then push") != std::string::npos);
	s.send(request::novaworld_pull());
	TEST_EXPECT(s.novaworld().revision == 2 && s.read("defs/a.def") == "a2" &&
			copy_at(dir.file("Mod"), copy) && copy.revision == 2);
	// Its own files (the project file and defs/b.def, which the head lacks) a change to push.
	s.send(request::novaworld_check());
	TEST_EXPECT(s.novaworld().checked && s.novaworld().changes.size() >= 2);

	// The store reset (its revisions gone): the check finds the head before the folder's, says so,
	// and sets the folder up again at none, every file then a change.
	service.head = 0;
	service.manifests.clear();
	s.send(request::novaworld_check());
	TEST_EXPECT(s.said("NovaWorld was reset") && s.novaworld().tracked &&
			s.novaworld().revision == 0 && s.novaworld().checked &&
			s.novaworld().changes.size() >= 2);
	TEST_EXPECT(copy_at(dir.file("Mod"), copy) && copy.revision == 0);
	s.send(request::novaworld_push("First revision"));
	TEST_EXPECT(service.head == 1 && s.novaworld().revision == 1);
	std::printf("set up: a project made for MOD set up as it opens, at no revision (its files not the "
				"head's), its push waiting for a pull; the store reset, set up again by the check that "
				"found it\n");
	return 0;
}

// A player's own projects (ADR 0069: any account makes one and pushes it): a new game put on
// NovaWorld (made as the player's, private unless asked public) and pushed; a mod whose base game
// is found among the games the player may make a mod of, and one named by base_key where two games
// of its game are; a mod whose name the service's code rule refuses refused up front; an empty
// project put on refused where it has revisions, and where it is another's; a push of another's
// project refused; a project naming a key the service does not show the player, said so.
static int test_player_projects() {
	test_temp::TempDir dir("opennova_novaworld_session_player");
	FakeService service;
	Session s(&service);
	s.send(request::novaworld_sign_in(kServer, kPlayer, "player-pw"));
	TEST_EXPECT(s.novaworld().signed_in && s.novaworld().role == "player");
	// The games it may make a mod of: MOD alone, the one of the project's game (jo, no game install
	// named) a mod's base, found there.
	TEST_EXPECT(s.novaworld().games.size() == 1 && s.novaworld().games[0].key == "MOD");
	s.send(request::new_expansion_project(dir.file("Quiet"), "Quiet", "quiet"));
	s.send(request::novaworld_check());
	TEST_EXPECT(s.novaworld().put.kind == "expansion" && s.novaworld().put.refusal.empty() &&
			s.novaworld().put.base == "MOD" && s.novaworld().put.expansion_code == "quiet");
	s.send(request::novaworld_put_new("QUIET", "Quiet", ""));
	TEST_EXPECT(service.made.count("QUIET") && service.made["QUIET"].kind == "expansion" &&
			service.made["QUIET"].base == "MOD" &&
			service.made["QUIET"].expansion_code == "quiet" && service.made["QUIET"].is_public &&
			s.novaworld().tracked);
	// Another mod of the short name (its case aside): the service's refusal said, naming it and where
	// it is changed.
	s.send(request::new_expansion_project(dir.file("Hush"), "Hush", "quiet"));
	s.send(request::novaworld_put_new("HUSH", "Hush", ""));
	TEST_EXPECT(!service.made.count("HUSH") &&
			s.session.view().project.document->novaworld.empty() &&
			s.novaworld().last.find("another mod on NovaWorld uses the short name quiet already") !=
					std::string::npos &&
			s.novaworld().last.find("Short name in Project settings") != std::string::npos);

	// A new game of the player's.
	s.send(request::new_project(dir.file("Game"), "My Game"));
	s.send(request::novaworld_put_new("MYGAME", "", "public"));
	TEST_EXPECT(service.made.count("MYGAME") && service.made["MYGAME"].owner == kPlayer &&
			service.made["MYGAME"].kind == "game" && service.made["MYGAME"].game == "jo" &&
			service.made["MYGAME"].title == "My Game" && service.made["MYGAME"].is_public);
	TEST_EXPECT(s.session.view().project.document->novaworld == "MYGAME" && s.novaworld().tracked &&
			s.novaworld().revision == 0 && s.novaworld().remote.can_push && s.novaworld().checked &&
			!s.novaworld().changes.empty());
	s.send(request::novaworld_push("First revision"));
	TEST_EXPECT(service.made["MYGAME"].head == 1 && s.novaworld().revision == 1);
	// Its owner releases it, published on its own page under Projects; each refusal of the service's
	// said plainly; the release players get, once published, in the view.
	TEST_EXPECT(s.novaworld().remote.can_release && s.novaworld().remote.release_id == 0);
	s.send(request::novaworld_release("0.1.0"));
	TEST_EXPECT(service.release_key == "MYGAME" && service.release_version == "0.1.0" &&
			service.release_revision == 1 &&
			s.novaworld().last.find("Release MYGAME 0.1.0 is building on novaworld.test") !=
					std::string::npos &&
			s.novaworld().last.find("submit it for approval on its page under Projects") !=
					std::string::npos &&
			s.novaworld().last.find("an admin approves it before players get it") != std::string::npos &&
			s.novaworld().last.find("https://novaworld.test/me/projects/MYGAME") != std::string::npos);
	service.quota = 10;
	for (const auto &[code, words] : { std::pair<const char *, const char *>{ "release_in_progress",
											   "building already" },
				 { "too_many_releases", "release again tomorrow" },
				 { "quota_exceeded", "this release would pass it" } }) {
		service.refuse_release = code;
		s.send(request::novaworld_release("0.1.1"));
		TEST_EXPECT(s.novaworld().last.find("The release was refused: ") != std::string::npos &&
				s.novaworld().last.find(words) != std::string::npos &&
				service.release_version == "0.1.0");
	}
	service.refuse_release.clear();
	service.quota = 0;
	service.published["MYGAME"] = "0.1.0";
	s.send(request::novaworld_check());
	TEST_EXPECT(s.novaworld().remote.release_id == 7 && s.novaworld().remote.release_version == "0.1.0");
	// The same key again from another project: the service's refusal said.
	s.send(request::new_project(dir.file("Again"), "Again"));
	s.send(request::novaworld_put_new("MYGAME", "", ""));
	TEST_EXPECT(s.novaworld().last.find("has a project MYGAME already") != std::string::npos &&
			s.session.view().project.document->novaworld.empty() &&
			service.made["MYGAME"].head == 1);

	// A player's key as the service takes it (up to 11, no "QQ"): refused up front, nothing asked; a project inside
	// another's folder refused, nothing made.
	s.send(request::new_project(dir.file("Keys"), "Keys"));
	s.send(request::novaworld_put_new("TWELVECHARSX", "Keys", ""));
	TEST_EXPECT(s.refused("novaworld.refused") && !service.made.count("TWELVECHARSX"));
	s.send(request::novaworld_put_new("AQQB", "Keys", ""));
	TEST_EXPECT(s.refused("novaworld.refused") && !service.made.count("AQQB"));
	s.send(request::new_project(dir.file("Keys/Inner"), "Inner"));
	editor::ProjectDocument inner;
	editor::Diagnostic inner_why;
	TEST_EXPECT(s.session.view().project.refused.find("is inside the project") != std::string::npos &&
			!editor::open_project(dir.file("Keys/Inner"), inner, inner_why));
	// A mod whose name the service's rule refuses: refused up front, nothing asked.
	s.send(request::new_expansion_project(dir.file("Loud"), "Loud", "LoudMod"));
	s.send(request::novaworld_check());
	TEST_EXPECT(s.session.view().project.open && s.novaworld().put.kind == "expansion" &&
			!s.novaworld().put.refusal.empty());
	s.send(request::novaworld_put_new("LOUD", "", ""));
	TEST_EXPECT(s.refused("novaworld.refused") && !service.made.count("LOUD"));
	// A mod once MYGAME is a game of jo too (listed again after its put): no one base game found,
	// so base_key names it.
	TEST_EXPECT(s.novaworld().games.size() == 2);
	s.send(request::new_expansion_project(dir.file("Other"), "Other", "other"));
	s.send(request::novaworld_check());
	TEST_EXPECT(s.novaworld().put.refusal.empty() && s.novaworld().put.base.empty());
	s.send(request::novaworld_put_new("OTHER", "", ""));
	TEST_EXPECT(s.refused("novaworld.refused") && !service.made.count("OTHER"));
	s.send(request::novaworld_put_new("OTHER", "Other", "", "MYGAME"));
	TEST_EXPECT(service.made.count("OTHER") && service.made["OTHER"].base == "MYGAME" &&
			s.novaworld().tracked);

	// An empty project put on: refused where it is another's (MOD, the service's own) and where it
	// has revisions (MYGAME, the player's).
	s.send(request::new_project(dir.file("Taken"), "Taken"));
	s.send(request::novaworld_put_empty("MOD"));
	TEST_EXPECT(
			s.novaworld().last.find(editor::novaworld_push_refusal("MOD")) != std::string::npos &&
			s.session.view().project.document->novaworld.empty());
	s.send(request::novaworld_put_empty("MYGAME"));
	TEST_EXPECT(s.novaworld().last.find("has revisions already") != std::string::npos &&
			s.session.view().project.document->novaworld.empty());
	// An empty project it may push, but not what the project is: refused, nothing written. OTHER is a
	// mod and Taken a game; Shh a mod of another short name. A mod of its short name takes it.
	s.send(request::novaworld_put_empty("OTHER"));
	TEST_EXPECT(s.novaworld().last.find("OTHER is a mod on NovaWorld, and the project is a game") !=
					std::string::npos &&
			s.session.view().project.document->novaworld.empty());
	s.send(request::new_expansion_project(dir.file("Shh"), "Shh", "shh"));
	s.send(request::novaworld_put_empty("OTHER"));
	TEST_EXPECT(s.novaworld().last.find("OTHER is the mod other, and this one is shh") !=
					std::string::npos &&
			s.session.view().project.document->novaworld.empty());
	s.send(request::new_expansion_project(dir.file("Other2"), "Other", "other"));
	s.send(request::novaworld_put_empty("OTHER"));
	TEST_EXPECT(s.session.view().project.document->novaworld == "OTHER" && s.novaworld().tracked);

	// A project of MOD's (made for it): the player reads it but may not push it.
	push_to_mod(service, { { "defs/a.def", "a" } });
	s.send(new_project_for(dir.file("Theirs"), "Theirs", "MOD"));
	TEST_EXPECT(
			s.novaworld().tracked && s.novaworld().remote_known && !s.novaworld().remote.can_push);
	TEST_EXPECT(put(dir.file("Theirs") + "/defs/b.def", "b"));
	s.send(request::novaworld_push("mine"));
	TEST_EXPECT(s.refused("novaworld.refused") && service.head == 1 &&
			s.session.view().activity.status == editor::novaworld_push_refusal("MOD"));
	// Nor release it.
	s.send(request::novaworld_release("9.9"));
	TEST_EXPECT(s.refused("novaworld.refused") && service.release_version == "0.1.0" &&
			s.session.view().activity.status ==
					"Only its owner and NovaWorld's admins release MOD.");
	// Made the player's own: the key out of the project file, the project on no NovaWorld project.
	s.send(request::novaworld_make_own());
	TEST_EXPECT(s.session.view().project.document->novaworld.empty() && s.novaworld().key.empty() &&
			s.read("project.opennova").find("\"novaworld\"") == std::string::npos);

	// A project naming a key the service shows the player none of: said so, no working copy.
	s.send(new_project_for(dir.file("Nope"), "Nope", "NOPE"));
	const std::string unreadable = "NOPE is not on novaworld.test, or is private: sign in with an "
								   "account that may read it, or on the NovaWorld server it is on.";
	TEST_EXPECT(s.session.view().project.open && !s.novaworld().tracked &&
			s.novaworld().head_error == unreadable);
	s.send(request::novaworld_pull());
	TEST_EXPECT(s.refused("novaworld.refused") &&
			s.session.view().activity.status == s.novaworld().head_error);
	std::printf("player: a game and a mod put on NovaWorld and pushed, a mod's name refused up "
				"front, another's project read-only, a key not shown said so\n");
	return 0;
}

// An expansion's project got: its base game's project comes along where its project file names it
// (base_project "../MOD"), each folder a working copy of its own project.
static int test_get_expansion() {
	test_temp::TempDir dir("opennova_novaworld_session_expansion");
	FakeService service;
	const auto file_of = [&](const char *path, const std::string &bytes) {
		const std::string sha = opennova::io::sha256_hex(bytes.data(), bytes.size());
		service.blobs[sha] = bytes;
		return ProjectFile{ path, sha, bytes.size() };
	};
	service.manifests[1] = opennova::project::encode_manifest({ file_of("defs/a.def", "base") });
	service.head = 1;
	service.expansion = true;
	service.expansion_manifests[1] = opennova::project::encode_manifest(
			{ file_of("project.opennova", "{\"expansion\": {\"base_project\": \"../MOD\"}}"),
					file_of("defs/e.def", "expansion") });
	service.expansion_head = 1;
	Session s(&service);
	s.send(request::novaworld_sign_in(kServer, "taylor", "pw"));
	TEST_EXPECT(s.novaworld().projects.size() == 2 && s.novaworld().projects[1].base == "MOD");
	s.send(request::novaworld_get("EXP", dir.file("work/EXP")));
	std::string text, error;
	TEST_EXPECT(opennova::io::read_file_text(dir.file("work/EXP/defs/e.def"), text, error) &&
			text == "expansion");
	TEST_EXPECT(opennova::io::read_file_text(dir.file("work/MOD/defs/a.def"), text, error) &&
			text == "base");
	for (const auto &[folder, key] : { std::pair<const char *, const char *>{ "work/EXP", "EXP" },
				 { "work/MOD", "MOD" } }) {
		editor::NovaWorldWorkingCopy copy;
		TEST_EXPECT(copy_at(dir.file(folder), copy) && copy.key == key && copy.revision == 1);
	}
	std::printf("expansion: got with its base game's project beside it\n");
	return 0;
}

// A mod got whose base game's project is a retail game's (its files the game install's, private on
// NovaWorld) or one the account may not read: no get of the base is tried, its project is made from
// the game install, and with no install of its game the mod's own working copy is got alone, Output
// saying where to make the base game's project.
static int test_get_mod_base_not_got() {
	test_temp::TempDir dir("opennova_novaworld_session_base_not_got");
	FakeService service;
	const auto file_of = [&](const char *path, const std::string &bytes) {
		const std::string sha = opennova::io::sha256_hex(bytes.data(), bytes.size());
		service.blobs[sha] = bytes;
		return ProjectFile{ path, sha, bytes.size() };
	};
	service.manifests[1] = opennova::project::encode_manifest({ file_of("defs/a.def", "base") });
	service.head = 1;
	service.expansion = true;
	service.expansion_manifests[1] = opennova::project::encode_manifest(
			{ file_of("project.opennova", "{\"expansion\": {\"base_project\": \"../MOD\"}}"),
					file_of("defs/e.def", "expansion") });
	service.expansion_head = 1;
	const auto got = [&](const std::string &folder) {
		editor::NovaWorldWorkingCopy copy;
		std::string text, error;
		return copy_at(folder, copy) ||
				opennova::io::read_file_text(folder + "/defs/a.def", text, error);
	};
	service.retail = true; // the admin reads it, a retail game's
	{
		Session s(&service);
		s.send(request::novaworld_sign_in(kServer, "taylor", "pw"));
		s.send(request::novaworld_get("EXP", dir.file("retail/EXP")));
		TEST_EXPECT(got(dir.file("retail/EXP")) && !got(dir.file("retail/MOD")) &&
				service.downloads == 2 &&
				s.said("Its base game's project, MOD, is not got: A mod's project is made from a Joint "
					   "Operations install, and the editor's game install is none.") &&
				s.said("retail/MOD"));
	}
	service.retail = false;
	service.is_public = false; // the service's own and private: the player reads it not
	{
		Session s(&service);
		s.send(request::novaworld_sign_in(kServer, kPlayer, "player-pw"));
		s.send(request::novaworld_get("EXP", dir.file("private/EXP")));
		TEST_EXPECT(got(dir.file("private/EXP")) && !got(dir.file("private/MOD")) &&
				s.said("Its base game's project, MOD, is not got: MOD's project is made from a Joint "
					   "Operations install"));
	}
	std::printf("base not got: a mod's retail or unreadable base game's project left to the game "
				"install, said so\n");
	return 0;
}

// A mod made of one of NovaWorld's games (the New project form's A mod of, new_project's base_key): its base game's
// project got from NovaWorld beside it (MOD, a game the account may get), the mod made on it there and opened; a
// second mod beside the first made on the same project (one naming the game's key, on this computer already), nothing
// got again; a retail game's (MOD as one) imported from the game install the request names, as a game you own is
// made, the mod made on it; refused signed out, for a game NovaWorld does not list, and for a retail game with no
// install of its game.
static int test_new_mod() {
	test_temp::TempDir dir("opennova_novaworld_session_new_mod");
	FakeService service;
	// MOD's first revision: a game's project, naming its key.
	editor::ProjectDocument made;
	editor::Diagnostic why;
	TEST_EXPECT(editor::create_project(dir.file("source"), "A mod", "jo", made, why, editor::ProjectExpansion(), "MOD"));
	std::string project, error;
	TEST_EXPECT(opennova::io::read_file_text(dir.file("source/project.opennova"), project, error));
	push_to_mod(service, { { "project.opennova", project }, { "defs/a.def", "a1" } });
	const auto mod = [&](const char *folder, const char *title, const char *code) {
		editor::EditorRequest r = request::new_expansion_project(dir.file(folder), title, code);
		r.base_key = "MOD";
		return r;
	};
	Session s(&service);
	s.send(mod("got/Night", "Night", "night"));
	TEST_EXPECT(s.refused("novaworld.refused") && !s.session.view().project.open); // signed out
	s.send(request::novaworld_sign_in(kServer, kPlayer, "player-pw"));
	editor::EditorRequest unlisted = mod("got/Nope", "Nope", "nope");
	unlisted.base_key = "NOPE";
	s.send(unlisted);
	TEST_EXPECT(s.refused("novaworld.refused") &&
			s.session.view().activity.status.find("lists no game NOPE") != std::string::npos);

	// Got beside it, the mod made on it and opened; a base the editor left half-made there (its setup's mark) made
	// again.
	TEST_EXPECT(put(dir.file("got/MOD/.opennova/base_setup"), "MOD") && put(dir.file("got/MOD/half.def"), "half"));
	s.send(mod("got/Night", "Night", "night"));
	std::string half;
	TEST_EXPECT(!opennova::io::read_file_text(dir.file("got/MOD/half.def"), half, error) &&
			!editor::base_setup_half_made(dir.file("got/MOD")) && s.said("left half-made"));
	editor::NovaWorldWorkingCopy copy;
	TEST_EXPECT(copy_at(dir.file("got/MOD"), copy) && copy.key == "MOD" && copy.revision == 1);
	TEST_EXPECT(s.session.view().project.open && s.session.view().project.document->title == "Night" &&
			s.session.view().project.document->expansion.name == "night" &&
			s.session.view().project.document->expansion.base_project == "../MOD" &&
			s.said("A mod's project is in"));
	// A second beside it: the same project, got by none again.
	const size_t downloads = service.downloads;
	s.send(mod("got/Dawn", "Dawn", "dawn"));
	TEST_EXPECT(s.session.view().project.open && s.session.view().project.document->title == "Dawn" &&
			s.session.view().project.document->expansion.base_project == "../MOD" &&
			service.downloads == downloads);

	// A retail game's: none without an install of its game; imported from one. (The project got above, among the
	// recent projects now, gone: the one found first otherwise.)
	std::error_code removed;
	std::filesystem::remove_all(opennova::io::os_path(dir.file("got")), removed);
	service.retail = true;
	s.send(request::novaworld_check()); // listed again: a retail game's now
	s.send(mod("retail/Night", "Night", "night"));
	TEST_EXPECT(s.refused("novaworld.refused") &&
			s.session.view().activity.status.find("is made from a Joint Operations install") != std::string::npos);
	std::vector<minted_install::File> game;
	std::vector<minted_install::File> expansion;
	TEST_EXPECT(minted_install::base_game(game, error) && minted_install::expansion_game(expansion, error) &&
			minted_install::write_install(
					dir.file("install"), game, minted_install::kExpansion, expansion, error));
	editor::EditorRequest installed = mod("retail/Night", "Night", "night");
	installed.game_install = dir.file("install");
	s.send(installed);
	editor::ProjectDocument base;
	TEST_EXPECT(editor::open_project(dir.file("retail/MOD"), base, why) && base.expansion.standalone() &&
			base.target_game == "jo" && !base.archives.empty() && base.novaworld.empty());
	TEST_EXPECT(s.session.view().project.open && s.session.view().project.root == dir.file("retail/Night") &&
			s.session.view().project.document->expansion.base_project == "../MOD");
	// A game you own of the install's expansion (New project's retail expansion: base_key its base game): made on
	// the base game's project beside it, the one just made, every file of the expansion imported.
	editor::EditorRequest owned = request::new_stock_project(
			dir.file("retail/Nifty"), "Nifty", dir.file("install"), minted_install::kExpansion);
	owned.base_key = "MOD";
	s.send(owned);
	TEST_EXPECT(s.session.view().project.open && s.session.view().project.root == dir.file("retail/Nifty") &&
			s.session.view().project.document->expansion.name == minted_install::kExpansion &&
			s.session.view().project.document->expansion.base_project == "../MOD" &&
			s.said("A mod's project in"));
	// A game you own stays the player's: never put on NovaWorld by one.
	s.send(request::open_project(dir.file("retail/MOD")));
	s.send(request::novaworld_put_new("MINE", "Mine", ""));
	TEST_EXPECT(s.refused("novaworld.refused") && !service.made.count("MINE") &&
			s.session.view().activity.status.find("A game you own holds the game's own files") != std::string::npos);
	std::printf("new mod: of a game got from NovaWorld beside it, of one on this computer already, of a retail "
				"game's imported from the game install; a game you own's expansion on its base game's project\n");
	return 0;
}

// What NovaWorld is doing, where the window and the status bar read it: a get's operation stepped a poll at a time
// says its phase in words, "Downloading N of 4 files (...)" with its bytes counted, and can be cancelled; a put and a
// release asked of NovaWorld say so while they wait (the view's waiting), nothing once answered.
static int test_progress() {
	test_temp::TempDir dir("opennova_novaworld_session_progress");
	FakeService service;
	push_to_mod(service, { { "defs/a.def", "a" }, { "defs/b.def", "bb" }, { "defs/c.def", "ccc" },
								 { "defs/d.def", "dddd" } });
	Session s(&service);
	s.send(request::novaworld_sign_in(kServer, "taylor", "pw"));
	s.session.set_poll_budget(editor::PollBudget{ 0, 1 });
	TEST_EXPECT(s.session.handle(request::novaworld_get("MOD", dir.file("Got"))));
	std::vector<std::string> labels;
	bool counted = false;
	for (int i = 0; i < 200 && s.session.view().activity.operation.running(); ++i) {
		const editor::OperationStatus &running = s.session.view().activity.operation;
		if (labels.empty() || labels.back() != running.label)
			labels.push_back(running.label);
		if (running.label.rfind("Downloading ", 0) == 0) {
			TEST_EXPECT(running.label.find(" of 4 files (") != std::string::npos && running.total == 10 &&
					running.done <= running.total && running.unit == editor::OperationUnit::Bytes &&
					running.cancellable);
			counted = true;
		}
		s.session.poll();
	}
	s.session.run_operations();
	TEST_EXPECT(counted && !labels.empty() && labels.front().find("Asking NovaWorld for MOD") == 0);
	TEST_EXPECT(std::find(labels.begin(), labels.end(), "Downloading 4 of 4 files (10 bytes of 10 bytes)") !=
					labels.end() ||
			std::any_of(labels.begin(), labels.end(),
					[](const std::string &label) { return label.rfind("Writing files", 0) == 0; }));
	std::string got, error;
	TEST_EXPECT(opennova::io::read_file_text(dir.file("Got/defs/d.def"), got, error) && got == "dddd");
	// A put and a release while they wait on NovaWorld.
	s.send(request::new_project(dir.file("Put"), "Put"));
	s.session.handle(request::novaworld_put_new("PUTME", "Put", ""));
	TEST_EXPECT(s.novaworld().calling && s.novaworld().waiting == "Putting PUTME on NovaWorld...");
	s.session.run_operations();
	TEST_EXPECT(!s.novaworld().calling && s.novaworld().waiting.empty());
	TEST_EXPECT(put(dir.file("Put") + "/defs/a.def", "a"));
	s.send(request::novaworld_push("First revision"));
	s.session.handle(request::novaworld_release("1.0"));
	TEST_EXPECT(s.novaworld().waiting == "Asking NovaWorld to build PUTME 1.0...");
	s.session.run_operations();
	TEST_EXPECT(s.novaworld().waiting.empty() && service.release_key == "PUTME");
	std::printf("progress: a get's phases in words, its downloads counted; a put and a release waiting said\n");
	return 0;
}

// An account change while a put is asked gives the put up (the last account's), said so; signed in
// again, the project goes on NovaWorld as the empty project the service made for it.
static int test_account_change_gives_up_put() {
	test_temp::TempDir dir("opennova_novaworld_session_put_given_up");
	FakeService service;
	Session s(&service);
	s.send(request::novaworld_sign_in(kServer, kPlayer, "player-pw"));
	s.send(request::new_project(dir.file("Gone"), "Gone"));
	s.session.handle(request::novaworld_put_new("GONE", "Gone", "")); // asked, not yet answered
	s.send(request::novaworld_sign_out());
	editor::NovaWorldWorkingCopy copy;
	TEST_EXPECT(service.made.count("GONE") && s.session.view().project.document->novaworld.empty() &&
			!s.novaworld().tracked && !copy_at(dir.file("Gone"), copy) &&
			s.novaworld().last.find("The put of GONE on NovaWorld was given up") != std::string::npos &&
			s.novaworld().last.find("as the empty project GONE") != std::string::npos);
	s.send(request::novaworld_sign_in(kServer, kPlayer, "player-pw"));
	s.send(request::novaworld_put_empty("GONE"));
	TEST_EXPECT(s.session.view().project.document->novaworld == "GONE" && s.novaworld().tracked &&
			copy_at(dir.file("Gone"), copy) && copy.key == "GONE" && copy.revision == 0);
	std::printf("put given up: an account change gives a put up, the empty project taken after\n");
	return 0;
}

// A get of a project with no revision yet gets nothing, by the head its pull reads (the list may be
// older than the project: test_share_through_sessions gets one listed at none and pushed since):
// refused whether the list holds it or not (as over the MCP), no working copy and no project
// opened; an expansion whose base game's project has none still opens, its base folder no working
// copy (F-4).
static int test_get_without_revision() {
	test_temp::TempDir dir("opennova_novaworld_session_no_revision");
	const auto tracked = [&](const std::string &folder) {
		editor::NovaWorldWorkingCopy copy;
		return copy_at(folder, copy);
	};
	{
		FakeService service; // MOD at no revision
		Session s(&service);
		s.send(request::novaworld_sign_in(kServer, "taylor", "pw"));
		TEST_EXPECT(s.novaworld().projects.size() == 1 && s.novaworld().projects[0].head == 0);
		s.send(request::novaworld_get("MOD", dir.file("Listed")));
		TEST_EXPECT(!s.session.view().project.open && !tracked(dir.file("Listed")) &&
				s.novaworld().last.find("MOD has no revision yet") != std::string::npos);
	}
	{
		FakeService service;
		Session s(&service);
		s.send(request::novaworld_sign_in(kServer, "taylor", "pw"));
		service.expansion = true; // EXP at no revision, not in the session's list
		s.send(request::novaworld_get("EXP", dir.file("Unlisted")));
		TEST_EXPECT(!s.session.view().project.open && !tracked(dir.file("Unlisted")) &&
				s.novaworld().last.find("no revision yet") != std::string::npos);
	}
	{
		FakeService service; // MOD, the base, at no revision; EXP at revision 1
		const std::string project = "{\"expansion\": {\"base_project\": \"../MOD\"}}";
		const std::string sha = opennova::io::sha256_hex(project.data(), project.size());
		service.blobs[sha] = project;
		service.expansion = true;
		service.expansion_manifests[1] = opennova::project::encode_manifest(
				{ ProjectFile{ "project.opennova", sha, project.size() } });
		service.expansion_head = 1;
		Session s(&service);
		s.send(request::novaworld_sign_in(kServer, "taylor", "pw"));
		s.send(request::novaworld_get("EXP", dir.file("work/EXP")));
		TEST_EXPECT(tracked(dir.file("work/EXP")) && !tracked(dir.file("work/MOD")) &&
				s.novaworld().last.find("MOD has no revision yet") != std::string::npos);
	}
	std::printf("get: a project with no revision yet gets nothing, listed or not; an expansion "
				"over a base with none opens\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_offline();
	failures += test_sign_in();
	failures += test_share_through_sessions();
	failures += test_set_up_and_reset();
	failures += test_player_projects();
	failures += test_get_expansion();
	failures += test_get_mod_base_not_got();
	failures += test_new_mod();
	failures += test_account_change_gives_up_put();
	failures += test_progress();
	failures += test_get_without_revision();
	if (failures == 0)
		std::printf("novaworld_session: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
