#pragma once

// The service's outbound mail (README "Account email and password resets"):
// the welcome mail POST /api/register sends and the password reset link POST
// /api/auth/forgot sends. Service policy, not a port.
//
// Mailer is the seam: SesMailer in production, NullMailer while mail is off
// (no ONNET_MAIL_FROM), a fake that records each message in the tests. The
// routes never send in their request: they post to the MailOutbox, whose
// worker thread sends one message at a time and logs each outcome as one line
// (the template, the recipient, SES's message id or the error; never the
// body, which may hold a reset link). So a reply's timing never tells whether
// a mail went out (POST /api/auth/forgot answers an address with no account
// the same way), and no request waits on SES.
//
// SesMailer runs the AWS CLI v2's `sesv2 send-email` through a
// proc::CommandRunner (apps/common/command_runner.h), its credentials the
// service instance's role, as the EC2 machine driver does: the request goes
// in a JSON file of the call's own (--cli-input-json), written owner-only into
// the service's owner-only scratch directory and deleted once the CLI ends,
// so no address or link is on a command line another account could list.

#include "command_runner.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace opennova::novaworld_server {

struct MailMessage {
	std::string kind; // the template ("welcome", "password_reset"): the log line's
	std::string to;   // the recipient, as normalize_email stores it
	std::string subject;
	std::string text; // the plain-text body
	std::string html; // the HTML body
};

struct MailResult {
	bool sent = false;
	std::string message_id; // SES's MessageId
	std::string error;      // why not, for the log: never the body
};

class Mailer {
public:
	virtual ~Mailer() = default;
	virtual MailResult send(const MailMessage &message) = 0;
};

// Mail off: sends nothing, and says so.
class NullMailer final : public Mailer {
public:
	MailResult send(const MailMessage &message) override;
};

struct SesMailerConfig {
	std::string aws_cli = "/usr/local/bin/aws"; // absolute, run as given (the image's)
	std::string region;                          // ONNET_MAIL_REGION
	std::string from;                            // ONNET_MAIL_FROM: the sender's address
	std::string from_name = "OpenNova";          // the From header's display name
	std::string scratch_dir;                     // owner-only: each call's request file
	// The CLI's bound, its own connect and read timeouts under it.
	int timeout_ms = 30 * 1000;
};

class SesMailer final : public Mailer {
public:
	// `runner` must outlive the mailer.
	SesMailer(SesMailerConfig config, proc::CommandRunner &runner);

	// "" when `config` can send, else why not (a boot warning; mail off).
	static std::string check_config(const SesMailerConfig &config);
	// The SendEmail request `message` is, as --cli-input-json reads it: the
	// From header (the display name and the address), the one recipient, the
	// subject and both bodies, each UTF-8.
	static std::string request_json(const SesMailerConfig &config, const MailMessage &message);
	// argv: the CLI, `sesv2 send-email`, the request file, then the region,
	// JSON output, no pager and the CLI's connect and read timeouts.
	static std::vector<std::string> command(const SesMailerConfig &config, const std::string &request_file);

	MailResult send(const MailMessage &message) override;

private:
	SesMailerConfig config_;
	proc::CommandRunner &runner_;
};

// The queue between the routes and a Mailer: post() hands a message over and
// returns at once; one worker thread sends them in order, logging each.
// Thread-safe.
class MailOutbox {
public:
	// The most messages waiting at once; a post past it is dropped (logged).
	static constexpr size_t kMaxQueued = 100;

	// Starts the worker. `mailer` must outlive the outbox.
	explicit MailOutbox(Mailer &mailer);
	~MailOutbox();

	MailOutbox(const MailOutbox &) = delete;
	MailOutbox &operator=(const MailOutbox &) = delete;

	// Queues `message`; false (logged) when the queue is full or stopped.
	bool post(MailMessage message);
	// Ends the worker once the message in flight is sent; the ones still
	// queued are dropped, each logged. Idempotent.
	void stop();
	// Returns once nothing is queued or in flight (a test's seam).
	void drain();

private:
	void run();

	Mailer &mailer_;
	std::mutex mu_;
	std::condition_variable cv_;
	std::deque<MailMessage> queue_;
	bool busy_ = false;
	bool stopping_ = false;
	std::thread worker_;
};

// Posts `message` to `outbox`, or, with none (mail off in an embedder that set
// none), logs that it was not sent.
void post_mail(MailOutbox *outbox, MailMessage message);

// The templates: plain text and a minimal HTML, each naming the account.
// `site_url` is the site's origin (ONNET_SITE_URL, no trailing '/').
MailMessage welcome_mail(const std::string &to, const std::string &username, const std::string &site_url);
MailMessage password_reset_mail(const std::string &to, const std::string &username, const std::string &reset_url);
// The link a reset mail carries: the site's /reset page with the token.
std::string password_reset_url(const std::string &site_url, const std::string &token);

} // namespace opennova::novaworld_server
