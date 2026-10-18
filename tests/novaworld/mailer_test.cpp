// The service's outbound mail (apps/novaworld_common/mailer.h) and the address
// rule registration and the password reset share (auth.h normalize_email):
// normalize_email's accepted and refused shapes; SesMailer's settings check,
// its argv (no address or link on it) and its SendEmail request on a stub
// CommandRunner (the request file owner-only while the CLI runs and deleted
// after a success and after a failure, SES's MessageId read back, the CLI's
// failures named); and the templates (the account's name escaped in the HTML,
// the reset link, no em dash). The mail.send jobs (their queue, their secret,
// their send) are jobs_test's. Database-free; links the server core
// (BUILD_NOVAWORLD_HTTP).

#include "auth.h"
#include "command_runner.h"
#include "core/owner_file.h"
#include "mailer.h"
#include "novaworld/fake_mailer.h"

#include <base/io/json.h>

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace nws = opennova::novaworld_server;
namespace proc = opennova::proc;
namespace fs = std::filesystem;
using proc::CommandResult;

namespace {

// What the stub saw of one CLI run: the argv, and the request file as it was
// while the CLI ran.
struct CliRun {
	std::vector<std::string> argv;
	int timeout_ms = 0;
	bool file_existed = false;
	std::string request;
	unsigned mode = 0;
};

class CliStub : public proc::CommandRunner {
public:
	CommandResult run(const std::vector<std::string> &argv, int timeout_ms) override {
		CliRun run;
		run.argv = argv;
		run.timeout_ms = timeout_ms;
		const std::string prefix = "file://";
		if (argv.size() > 4 && argv[4].compare(0, prefix.size(), prefix) == 0) {
			const std::string file = argv[4].substr(prefix.size());
			files.push_back(file);
			run.file_existed = fs::exists(fs::u8path(file));
			std::ifstream in(fs::u8path(file), std::ios::binary);
			run.request.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
#ifndef _WIN32
			struct stat st {};
			if (::stat(file.c_str(), &st) == 0) run.mode = static_cast<unsigned>(st.st_mode & 0777);
#endif
		}
		runs.push_back(run);
		CommandResult reply = next;
		return reply;
	}

	static CommandResult exited(uint32_t code, const std::string &out, const std::string &err = {}) {
		CommandResult r;
		r.outcome = CommandResult::Outcome::Exited;
		r.exit_code = code;
		r.out = out;
		r.err = err;
		return r;
	}

	CommandResult next;
	std::vector<CliRun> runs;
	std::vector<std::string> files;
};

nws::SesMailerConfig ses_config(const std::string &scratch) {
	nws::SesMailerConfig c;
	c.region = "us-east-1";
	c.from = "support@example.com";
	c.scratch_dir = scratch;
	return c;
}

nws::MailMessage sample() {
	return nws::password_reset_mail("player@example.org", "Player <One>",
			nws::password_reset_url("https://example.com", std::string(64, 'a')));
}

// normalize_email: lowercased when the shape is an address, else refused.
int test_normalize_email() {
	TEST_EXPECT(nws::normalize_email("Player.One+jo@Example.COM") == std::string("player.one+jo@example.com"));
	TEST_EXPECT(nws::normalize_email("a@b.co") == std::string("a@b.co"));
	TEST_EXPECT(nws::normalize_email("player@example.net") == std::string("player@example.net"));
	// 254 bytes is the most.
	const std::string domain = "@example.com";
	const std::string longest = std::string(254 - domain.size(), 'x') + domain;
	TEST_EXPECT(nws::normalize_email(longest) == longest);
	for (const std::string &refused : { std::string(), std::string("plain"), std::string("@example.com"),
				 std::string("a@"), std::string("a@b"), std::string("a@b@c.com"), std::string("a@.example.com"),
				 std::string("a@example.com."), std::string("a@example..com"), std::string("a b@example.com"),
				 std::string("a@exa\tmple.com"), std::string("a\n@example.com"), std::string("a@example.com\x7f"),
				 longest + "x" }) {
		if (nws::normalize_email(refused)) {
			std::fprintf(stderr, "  normalize_email took '%s'\n", refused.c_str());
			return 1;
		}
	}
	return 0;
}

// The settings check: a region, a lower-case sender address, an absolute CLI,
// a scratch directory.
int test_ses_config(const std::string &scratch) {
	TEST_EXPECT(nws::SesMailer::check_config(ses_config(scratch)).empty());
	auto c = ses_config(scratch);
	c.region = "";
	TEST_EXPECT(!nws::SesMailer::check_config(c).empty());
	c = ses_config(scratch);
	c.region = "us east 1";
	TEST_EXPECT(!nws::SesMailer::check_config(c).empty());
	c = ses_config(scratch);
	c.from = "support";
	TEST_EXPECT(nws::SesMailer::check_config(c).find("ONNET_MAIL_FROM") != std::string::npos);
	c.from = "Support@example.com";
	TEST_EXPECT(!nws::SesMailer::check_config(c).empty());
	c = ses_config(scratch);
	c.aws_cli = "aws";
	TEST_EXPECT(!nws::SesMailer::check_config(c).empty());
	c = ses_config(scratch);
	c.scratch_dir = "";
	TEST_EXPECT(!nws::SesMailer::check_config(c).empty());
	return 0;
}

// The SendEmail request: From with its display name, the one recipient, the
// subject and both bodies in UTF-8.
int test_ses_request(const std::string &scratch) {
	const nws::MailMessage m = sample();
	opennova::io::JsonValue request;
	std::string error;
	TEST_EXPECT(opennova::io::json_parse(nws::SesMailer::request_json(ses_config(scratch), m), request, error));
	TEST_EXPECT(request.get_string("FromEmailAddress", "") == "OpenNova <support@example.com>");
	const auto *to = request.get("Destination") ? request.get("Destination")->get("ToAddresses") : nullptr;
	TEST_EXPECT(to != nullptr && to->is_array() && to->array.size() == 1 && to->array[0].string == m.to);
	const auto *simple = request.get("Content") ? request.get("Content")->get("Simple") : nullptr;
	TEST_EXPECT(simple != nullptr);
	TEST_EXPECT(simple->get("Subject")->get_string("Data", "") == m.subject);
	TEST_EXPECT(simple->get("Subject")->get_string("Charset", "") == "UTF-8");
	const auto *body = simple->get("Body");
	TEST_EXPECT(body != nullptr && body->get("Text")->get_string("Data", "") == m.text);
	TEST_EXPECT(body->get("Html")->get_string("Data", "") == m.html);
	TEST_EXPECT(body->get("Html")->get_string("Charset", "") == "UTF-8");
	return 0;
}

// A send: the argv exactly (the request in a file, never on the command line),
// the file owner-only while the CLI runs and gone after, SES's MessageId kept.
int test_ses_send(const std::string &scratch) {
	CliStub cli;
	cli.next = CliStub::exited(0, "{\n    \"MessageId\": \"0100018f-abcd-1234\"\n}\n");
	const auto config = ses_config(scratch);
	nws::SesMailer mailer(config, cli);
	const nws::MailMessage m = sample();
	const nws::MailResult result = mailer.send(m);
	TEST_EXPECT(result.sent && result.message_id == "0100018f-abcd-1234" && result.error.empty());
	TEST_EXPECT(cli.runs.size() == 1 && cli.files.size() == 1);
	const CliRun &run = cli.runs[0];
	TEST_EXPECT(run.argv == nws::SesMailer::command(config, cli.files[0]));
	const std::vector<std::string> want = { "/usr/local/bin/aws", "sesv2", "send-email", "--cli-input-json",
		"file://" + cli.files[0], "--region", "us-east-1", "--output", "json", "--no-cli-pager",
		"--cli-connect-timeout", "10", "--cli-read-timeout", "20" };
	TEST_EXPECT(run.argv == want);
	for (const std::string &arg : run.argv) {
		TEST_EXPECT(arg.find("player@example.org") == std::string::npos);
		TEST_EXPECT(arg.find(std::string(64, 'a')) == std::string::npos);
	}
	TEST_EXPECT(run.timeout_ms == config.timeout_ms);
	TEST_EXPECT(run.file_existed && run.request == nws::SesMailer::request_json(config, m));
	TEST_EXPECT(fs::u8path(cli.files[0]).parent_path() == fs::u8path(scratch));
#ifndef _WIN32
	TEST_EXPECT(run.mode == 0600);
#endif
	TEST_EXPECT(!fs::exists(fs::u8path(cli.files[0])));
	return 0;
}

// The CLI's failures: SES's refusal (its error on stderr), a timeout, a CLI
// that never ran, a reply with no MessageId. Each is not sent, named, and the
// request file is gone.
int test_ses_failures(const std::string &scratch) {
	CliStub cli;
	nws::SesMailer mailer(ses_config(scratch), cli);
	cli.next = CliStub::exited(254, "",
			"\nAn error occurred (MessageRejected) when calling the SendEmail operation: Email address is not "
			"verified.\n");
	nws::MailResult result = mailer.send(sample());
	TEST_EXPECT(!result.sent && result.message_id.empty());
	TEST_EXPECT(result.error.find("exited with code 254") != std::string::npos);
	TEST_EXPECT(result.error.find("MessageRejected") != std::string::npos);
	cli.next = CommandResult{};
	cli.next.outcome = CommandResult::Outcome::TimedOut;
	result = mailer.send(sample());
	TEST_EXPECT(!result.sent && result.error.find("timed out") != std::string::npos);
	cli.next = CommandResult{};
	cli.next.error = "no such file";
	result = mailer.send(sample());
	TEST_EXPECT(!result.sent && result.error.find("did not start") != std::string::npos);
	cli.next = CliStub::exited(0, "{}");
	result = mailer.send(sample());
	TEST_EXPECT(!result.sent && result.error.find("MessageId") != std::string::npos);
	TEST_EXPECT(cli.files.size() == 4);
	for (const std::string &file : cli.files) TEST_EXPECT(!fs::exists(fs::u8path(file)));
	// Not one body leaks into an error.
	TEST_EXPECT(result.error.find("reset") == std::string::npos);
	return 0;
}

// The templates: the account's name, the site's link and the reset link in
// both bodies; the name escaped in the HTML; plain words (no em dash).
int test_templates() {
	const auto welcome = nws::welcome_mail("player@example.org", "Player <One>", "https://example.com");
	TEST_EXPECT(welcome.kind == "welcome" && welcome.to == "player@example.org");
	TEST_EXPECT(welcome.subject == "Welcome to OpenNova");
	TEST_EXPECT(welcome.text.find("Player <One>") != std::string::npos);
	TEST_EXPECT(welcome.text.find("https://example.com/") != std::string::npos);
	TEST_EXPECT(welcome.html.find("Player &lt;One&gt;") != std::string::npos);
	TEST_EXPECT(welcome.html.find("Player <One>") == std::string::npos);
	TEST_EXPECT(welcome.html.find("<a href=\"https://example.com/\">") != std::string::npos);

	const std::string url = nws::password_reset_url("https://example.com", "00ff");
	TEST_EXPECT(url == "https://example.com/reset?token=00ff");
	const auto reset = nws::password_reset_mail("player@example.org", "Player <One>", url);
	TEST_EXPECT(reset.kind == "password_reset" && reset.subject == "Reset your OpenNova password");
	TEST_EXPECT(reset.text.find(url) != std::string::npos);
	TEST_EXPECT(reset.text.find("within the hour") != std::string::npos);
	TEST_EXPECT(reset.html.find("<a href=\"" + url + "\">" + url + "</a>") != std::string::npos);
	TEST_EXPECT(reset.html.find("Player &lt;One&gt;") != std::string::npos);
	for (const auto *m : { &welcome, &reset }) {
		for (const std::string *text : { &m->text, &m->html, &m->subject })
			TEST_EXPECT(text->find("\xE2\x80\x94") == std::string::npos);
	}
	return 0;
}

} // namespace

int main() {
	test_temp::TempDir temp("mailer");
	const fs::path scratch_path = temp.path / "scratch";
	std::string error;
	if (!nws::hosted::make_owner_directory(scratch_path, error)) {
		std::fprintf(stderr, "cannot make the scratch directory: %s\n", error.c_str());
		return 1;
	}
	const std::string scratch = scratch_path.u8string();
	if (test_normalize_email() != 0) return 1;
	if (test_ses_config(scratch) != 0) return 1;
	if (test_ses_request(scratch) != 0) return 1;
	if (test_ses_send(scratch) != 0) return 1;
	if (test_ses_failures(scratch) != 0)
		return 1;
	if (test_templates() != 0) return 1;
	std::printf("OK: mailer (normalize_email, SES config, request, send, failures, templates)\n");
	return 0;
}
