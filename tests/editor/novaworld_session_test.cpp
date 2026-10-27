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
// a waiting pull's conflicts with it; the history gone with the project; a project made for a
// NovaWorld project (Set up...) set up as its working copy as it opens; the store reset under a
// working copy, set up again by the check that found it; a player's new project put on NovaWorld (a
// game, a mod of a game it names) and pushed, a mod whose name the service refuses refused up
// front, an empty project put on refused where it has revisions or is another's; a player's push of
// another's project refused; a project file naming a key the service shows the account none of,
// said so; a get of a project with no revision yet refused; and the novaworld section's JSON, which
// holds no session.

#include <cstdio>
#include <string>
#include <vector>

#include <base/io/file_io.h>
#include <base/io/json.h>
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
#include <editor/session/view/session_view.h>

#include "common/test_expect.h"
#include "editor/novaworld_fake_service.h"
#include "editor/editor_test_support.h"

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
	s.send(request::novaworld_sign_out());
	TEST_EXPECT(!s.novaworld().signed_in && s.accounts.load().cookie.empty() &&
			s.accounts.load().server == kServer);
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
	// working copy at revision 0, every file a change, "First revision" pushed.
	first.send(request::new_project(dir.file("First"), "Mod"));
	TEST_EXPECT(first.session.view().project.open && first.novaworld().key.empty() &&
			first.novaworld().put.kind == "game" && first.novaworld().put.game == "jo");
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
	TEST_EXPECT(second.novaworld().conflicts.empty());
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
	// A release of the second's revision, built on the service; a version that is none refused.
	second.send(request::novaworld_release("x.y"));
	TEST_EXPECT(second.refused("novaworld.refused") && service.release_version.empty());
	second.send(request::novaworld_release("0.2.0"));
	TEST_EXPECT(service.release_version == "0.2.0" && service.release_revision == 5 &&
			second.novaworld().last.find("MOD 0.2.0") != std::string::npos);
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
// key): the key in its project file, and the folder set up as its working copy as it opens, at the
// project's head, the form's key gone with the project made; a key that is none refused, nothing
// made. Then the store reset under it: the check that finds it says so, sets the folder up again at
// the new head, and counts every file as a change.
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
	TEST_EXPECT(copy_at(dir.file("Mod"), copy) && copy.key == "MOD" && copy.revision == 2);
	TEST_EXPECT(s.novaworld().tracked && s.novaworld().revision == 2 &&
			s.novaworld().remote_known && s.novaworld().head == 2 &&
			s.said("working copy on novaworld.test, at revision 2"));
	// Its files (the project file the head lacks) a change to push.
	TEST_EXPECT(put(dir.file("Mod") + "/defs/b.def", "b"));
	s.send(request::novaworld_check());
	TEST_EXPECT(s.novaworld().checked && !s.novaworld().changes.empty());

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
	std::printf("set up: a project made for MOD set up at its head as it opens; the store reset, "
				"set up again by the check that found it\n");
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
	TEST_EXPECT(s.novaworld().put.kind == "expansion" && s.novaworld().put.refusal.empty() &&
			s.novaworld().put.base == "MOD" && s.novaworld().put.expansion_code == "quiet");
	s.send(request::novaworld_put_new("QUIET", "Quiet", ""));
	TEST_EXPECT(service.made.count("QUIET") && service.made["QUIET"].kind == "expansion" &&
			service.made["QUIET"].base == "MOD" &&
			service.made["QUIET"].expansion_code == "quiet" && !service.made["QUIET"].is_public &&
			s.novaworld().tracked);

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
	// The same key again from another project: the service's refusal said.
	s.send(request::new_project(dir.file("Again"), "Again"));
	s.send(request::novaworld_put_new("MYGAME", "", ""));
	TEST_EXPECT(s.novaworld().last.find("has a project MYGAME already") != std::string::npos &&
			s.session.view().project.document->novaworld.empty() &&
			service.made["MYGAME"].head == 1);

	// A mod whose name the service's rule refuses: refused up front, nothing asked.
	s.send(request::new_expansion_project(dir.file("Loud"), "Loud", "LoudMod"));
	TEST_EXPECT(s.session.view().project.open && s.novaworld().put.kind == "expansion" &&
			!s.novaworld().put.refusal.empty());
	s.send(request::novaworld_put_new("LOUD", "", ""));
	TEST_EXPECT(s.refused("novaworld.refused") && !service.made.count("LOUD"));
	// A mod once MYGAME is a game of jo too (listed again after its put): no one base game found,
	// so base_key names it.
	TEST_EXPECT(s.novaworld().games.size() == 2);
	s.send(request::new_expansion_project(dir.file("Other"), "Other", "other"));
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

	// A project of MOD's (made for it): the player reads it but may not push it.
	push_to_mod(service, { { "defs/a.def", "a" } });
	s.send(new_project_for(dir.file("Theirs"), "Theirs", "MOD"));
	TEST_EXPECT(
			s.novaworld().tracked && s.novaworld().remote_known && !s.novaworld().remote.can_push);
	TEST_EXPECT(put(dir.file("Theirs") + "/defs/b.def", "b"));
	s.send(request::novaworld_push("mine"));
	TEST_EXPECT(s.refused("novaworld.refused") && service.head == 1 &&
			s.session.view().activity.status == editor::novaworld_push_refusal("MOD"));

	// A project naming a key the service shows the player none of: said so, no working copy.
	s.send(new_project_for(dir.file("Nope"), "Nope", "NOPE"));
	const std::string unreadable = "NOPE is not on novaworld.test, or is private: sign in with an "
								   "account that may read it.";
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
	failures += test_get_without_revision();
	if (failures == 0)
		std::printf("novaworld_session: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
