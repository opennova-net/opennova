#include "mail_stack.h"

#include <cstdio>
#include <string>
#include <utility>

namespace opennova::novaworld_server {

MailStack make_mail(const ServerConfig &config) {
	MailStack s;
	const auto off = [&s](bool warn, const std::string &why) {
		std::fprintf(warn ? stderr : stdout, "[boot] %smail: %s; mail off\n", warn ? "WARN " : "",
				why.c_str());
		s.mailer = std::make_unique<NullMailer>();
	};
	if (config.mail_from.empty()) {
		off(false, "no ONNET_MAIL_FROM");
		return s;
	}
	if (config.site_url.empty()) {
		off(true, "ONNET_SITE_URL is not set, so no link could be sent");
		return s;
	}
	std::string error;
	ScratchDir scratch = ScratchDir::make("opennova-mail-", error);
	if (scratch.empty()) {
		off(true,
				"no scratch directory for the SES calls" +
						(error.empty() ? std::string() : ": " + error));
		return s;
	}
	SesMailerConfig ses;
	ses.region = config.mail_region;
	ses.from = config.mail_from;
	ses.scratch_dir = scratch.path().u8string();
	ses.aws_cli = config.aws_cli;
	if (const std::string why = SesMailer::check_config(ses); !why.empty()) {
		off(true, why);
		return s;
	}
	s.scratch = std::move(scratch);
	s.runner = std::make_unique<proc::ProcessCommandRunner>(
			proc::ProcessCommandRunner::service_environment(), ses.scratch_dir);
	s.mailer = std::make_unique<SesMailer>(ses, *s.runner);
	s.on = true;
	std::printf("[boot] mail: on, SES in %s, from %s, links to %s\n", ses.region.c_str(),
			ses.from.c_str(), config.site_url.c_str());
	return s;
}

} // namespace opennova::novaworld_server
