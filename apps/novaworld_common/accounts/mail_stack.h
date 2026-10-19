#pragma once

// Outbound mail as a program runs it (mailer.h): whether mail is on (web queues
// mail.send jobs only then) and the worker's SES mailer that sends them, each
// decided alike from the same settings. Service policy, not a port.

#include "command_runner.h"
#include "core/scratch_dir.h"
#include "mailer.h"
#include "program/server_config.h"

#include <memory>

namespace opennova::novaworld_server {

// The mailer the mail.send jobs send with (SES through the AWS CLI, with its
// runner and its owner-only scratch directory, removed at the end; a NullMailer
// while mail is off) and whether mail is on.
struct MailStack {
	ScratchDir scratch;
	std::unique_ptr<proc::CommandRunner> runner;
	std::unique_ptr<Mailer> mailer;
	bool on = false;
};

// SES over the config's ONNET_MAIL_FROM, ONNET_MAIL_REGION and ONNET_SITE_URL,
// or mail off, logged: unset (no sender), else a warning naming what is wrong.
MailStack make_mail(const ServerConfig &config);

} // namespace opennova::novaworld_server
