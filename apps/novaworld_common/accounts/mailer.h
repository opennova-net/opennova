#pragma once

// The service's outbound mail (README "Account email and password resets"):
// the welcome mail POST /api/register sends and the password reset link POST
// /api/auth/forgot sends. Service policy, not a port.
//
// Mailer is the seam: SesMailer in production, NullMailer while mail is off
// (no ONNET_MAIL_FROM), a fake that records each message in the tests. The
// routes never send in their request: they enqueue a mail.send job (the
// service's queue, jobs/job_queue.h), so a mail outlives a restart of the
// process whose route asked for it; a worker sends it and logs its outcome as
// one line (the template, the recipient, SES's message id or the error; never
// the body, which may hold a reset link). The job's payload names the template
// and the recipient alone; the subject and the bodies are its secret, gone
// from the row once the job ends. A mail is sent at most once: one SES refused
// is logged and its job failed, never tried again. So a reply's timing never
// tells whether a mail went out (POST /api/auth/forgot answers an address with
// no account the same way), and no request waits on SES.
//
// SesMailer runs the AWS CLI v2's `sesv2 send-email` through a
// proc::CommandRunner (apps/common/command_runner.h), its credentials the
// worker task's IAM role, as the EC2 machine driver does: the request goes
// in a JSON file of the call's own (--cli-input-json), written owner-only into
// the service's owner-only scratch directory and deleted once the CLI ends,
// so no address or link is on a command line another account could list.

#include "command_runner.h"
#include "jobs/worker_pool.h"

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <string>
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

inline constexpr const char *kJobMailSend = "mail.send";
// The most mail.send jobs running at once, across every worker.
inline constexpr int kMailSendCap = 4;

// `message` queued as a mail.send job on `db` (in its open transaction, when
// one is); its job id.
int64_t enqueue_mail(db::Database &db, const MailMessage &message);
// A route's send: `message` queued on the route's connection while mail is on
// (logged, never thrown, when the queue cannot take it: a mail's failure is
// never the request's), else logged as not sent.
void queue_mail(db::Database &db, bool mail_on, const MailMessage &message);
// The mail.send handler over `mailer`, which must outlive the workers.
void register_mail_jobs(jobs::JobRegistry &registry, Mailer &mailer);

// The templates: plain text and a minimal HTML, each naming the account.
// `site_url` is the site's origin (ONNET_SITE_URL, no trailing '/').
MailMessage welcome_mail(const std::string &to, const std::string &username, const std::string &site_url);
MailMessage password_reset_mail(const std::string &to, const std::string &username, const std::string &reset_url);
// The link a reset mail carries: the site's /reset page with the token.
std::string password_reset_url(const std::string &site_url, const std::string &token);

} // namespace opennova::novaworld_server
