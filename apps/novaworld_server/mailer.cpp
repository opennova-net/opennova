#include "mailer.h"

#include "auth.h"
#include "hosted/owner_file.h"
#include "loggable.h"
#include "template_engine.h"

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <base/os_random/os_random.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <system_error>
#include <utility>

namespace opennova::novaworld_server {

namespace {

namespace fs = std::filesystem;

// A recipient for a log line.
std::string recipient(const MailMessage &message) { return loggable(message.to, kEmailMaxBytes); }

void log_result(const MailMessage &message, const MailResult &result) {
	if (result.sent) {
		std::printf("[mail] sent %s to '%s' (message id %s)\n", message.kind.c_str(), recipient(message).c_str(),
				loggable(result.message_id, 120).c_str());
	} else {
		std::fprintf(stderr, "[mail] WARN %s to '%s' not sent: %s\n", message.kind.c_str(),
				recipient(message).c_str(), result.error.c_str());
	}
}

bool is_region_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }

io::JsonValue utf8_content(const std::string &data) {
	io::JsonValue v = io::JsonValue::make_object();
	v.set("Data", io::json_string(data));
	v.set("Charset", io::json_string("UTF-8"));
	return v;
}

// Why a CLI call failed, for the log: its outcome and the start of its
// standard error (SES's error code and message).
std::string cli_failure(const proc::CommandResult &result) {
	const std::string what = "aws sesv2 send-email";
	switch (result.outcome) {
		case proc::CommandResult::Outcome::NotStarted:
			return what + " did not start: " + loggable(result.error, 200);
		case proc::CommandResult::Outcome::TimedOut:
			return what + " timed out";
		case proc::CommandResult::Outcome::Exited:
			break;
	}
	std::string out = what + " exited with code " + std::to_string(result.exit_code);
	const std::string err(strutil::trim_view(result.err));
	if (!err.empty())
		out += ": " + loggable(err, 300);
	return out;
}

// The page a mail ends with.
std::string html_page(const std::string &body) {
	return "<!doctype html>\n<html><body style=\"font-family:sans-serif;line-height:1.5\">\n" + body +
			"</body></html>\n";
}

std::string html_link(const std::string &url) {
	const std::string escaped = html_escape(url);
	return "<a href=\"" + escaped + "\">" + escaped + "</a>";
}

} // namespace

MailResult NullMailer::send(const MailMessage &) {
	MailResult out;
	out.error = "mail is off (no ONNET_MAIL_FROM)";
	return out;
}

SesMailer::SesMailer(SesMailerConfig config, proc::CommandRunner &runner) :
		config_(std::move(config)), runner_(runner) {}

std::string SesMailer::check_config(const SesMailerConfig &c) {
	if (c.aws_cli.empty() || c.aws_cli[0] != '/')
		return "the AWS CLI's path is not absolute";
	if (c.region.empty() || c.region.size() > 32 || !std::all_of(c.region.begin(), c.region.end(), is_region_char))
		return "ONNET_MAIL_REGION is no region name";
	const auto from = normalize_email(c.from);
	if (!from || *from != c.from)
		return "ONNET_MAIL_FROM is not an email address in lower case";
	if (c.scratch_dir.empty())
		return "no scratch directory";
	if (c.timeout_ms <= 0)
		return "no timeout";
	return {};
}

std::string SesMailer::request_json(const SesMailerConfig &config, const MailMessage &message) {
	io::JsonValue to = io::JsonValue::make_array();
	to.push(io::json_string(message.to));
	io::JsonValue destination = io::JsonValue::make_object();
	destination.set("ToAddresses", std::move(to));
	io::JsonValue body = io::JsonValue::make_object();
	body.set("Text", utf8_content(message.text));
	body.set("Html", utf8_content(message.html));
	io::JsonValue simple = io::JsonValue::make_object();
	simple.set("Subject", utf8_content(message.subject));
	simple.set("Body", std::move(body));
	io::JsonValue content = io::JsonValue::make_object();
	content.set("Simple", std::move(simple));
	io::JsonValue request = io::JsonValue::make_object();
	request.set("FromEmailAddress", io::json_string(config.from_name + " <" + config.from + ">"));
	request.set("Destination", std::move(destination));
	request.set("Content", std::move(content));
	return io::json_write(request);
}

std::vector<std::string> SesMailer::command(const SesMailerConfig &config, const std::string &request_file) {
	return { config.aws_cli, "sesv2", "send-email", "--cli-input-json", "file://" + request_file, "--region",
		config.region, "--output", "json", "--no-cli-pager", "--cli-connect-timeout", "10", "--cli-read-timeout",
		"20" };
}

MailResult SesMailer::send(const MailMessage &message) {
	MailResult out;
	std::array<uint8_t, 8> raw{};
	os_random_bytes(raw.data(), raw.size());
	const fs::path file =
			fs::u8path(config_.scratch_dir) / ("ses-" + strutil::bytes_to_hex(raw.data(), raw.size()) + ".json");
	std::string error;
	std::error_code ec;
	if (!hosted::write_owner_file(file, request_json(config_, message), true, error)) {
		fs::remove(file, ec);
		out.error = "the request file: " + error;
		return out;
	}
	const proc::CommandResult result = runner_.run(command(config_, file.u8string()), config_.timeout_ms);
	fs::remove(file, ec);
	if (!result.ok()) {
		out.error = cli_failure(result);
		return out;
	}
	io::JsonValue reply;
	std::string parse_error;
	if (!io::json_parse(result.out, reply, parse_error) || !reply.is_object() ||
			reply.get_string("MessageId", "").empty()) {
		out.error = "aws sesv2 send-email answered no MessageId";
		return out;
	}
	out.sent = true;
	out.message_id = reply.get_string("MessageId", "");
	return out;
}

MailOutbox::MailOutbox(Mailer &mailer) : mailer_(mailer) { worker_ = std::thread([this] { run(); }); }

MailOutbox::~MailOutbox() { stop(); }

bool MailOutbox::post(MailMessage message) {
	bool stopped = false;
	{
		std::lock_guard<std::mutex> lock(mu_);
		stopped = stopping_;
		if (!stopped && queue_.size() < kMaxQueued) {
			queue_.push_back(std::move(message));
			cv_.notify_all();
			return true;
		}
	}
	std::fprintf(stderr, "[mail] WARN %s to '%s' dropped: the outbox is %s\n", message.kind.c_str(),
			recipient(message).c_str(), stopped ? "stopped" : "full");
	return false;
}

void MailOutbox::stop() {
	{
		std::lock_guard<std::mutex> lock(mu_);
		stopping_ = true;
	}
	cv_.notify_all();
	if (worker_.joinable())
		worker_.join();
}

void MailOutbox::drain() {
	std::unique_lock<std::mutex> lock(mu_);
	cv_.wait(lock, [this] { return (queue_.empty() && !busy_) || stopping_; });
}

void MailOutbox::run() {
	std::unique_lock<std::mutex> lock(mu_);
	for (;;) {
		cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
		if (stopping_)
			break;
		MailMessage message = std::move(queue_.front());
		queue_.pop_front();
		busy_ = true;
		lock.unlock();
		MailResult result;
		try {
			result = mailer_.send(message);
		} catch (const std::exception &e) {
			result.error = e.what();
		}
		log_result(message, result);
		lock.lock();
		busy_ = false;
		cv_.notify_all();
	}
	for (const MailMessage &message : queue_)
		std::fprintf(stderr, "[mail] WARN %s to '%s' dropped: the service is stopping\n", message.kind.c_str(),
				recipient(message).c_str());
	queue_.clear();
	cv_.notify_all();
}

void post_mail(MailOutbox *outbox, MailMessage message) {
	if (outbox != nullptr) {
		outbox->post(std::move(message));
		return;
	}
	std::fprintf(stderr, "[mail] WARN %s to '%s' not sent: mail is off\n", message.kind.c_str(),
			recipient(message).c_str());
}

std::string password_reset_url(const std::string &site_url, const std::string &token) {
	return site_url + "/reset?token=" + token;
}

MailMessage welcome_mail(const std::string &to, const std::string &username, const std::string &site_url) {
	const std::string home = site_url + "/";
	MailMessage m;
	m.kind = "welcome";
	m.to = to;
	m.subject = "Welcome to OpenNova";
	m.text = "Hello " + username + ",\n\n"
			"Your OpenNova account " + username + " is ready.\n\n"
			"OpenNova is an open-source, pre-1.0 reimplementation of NovaLogic's game engine. "
			"Its NovaWorld service runs online play for Joint Operations and newer NovaLogic "
			"titles: log in from the game with this username and password, or on the site to "
			"host servers and manage your account:\n\n" +
			home + "\n\n"
			"If you did not make this account, reply to this mail and we will remove it.\n\n"
			"OpenNova\n";
	const std::string name = html_escape(username);
	m.html = html_page("<p>Hello " + name + ",</p>\n<p>Your OpenNova account <strong>" + name +
			"</strong> is ready.</p>\n"
			"<p>OpenNova is an open-source, pre-1.0 reimplementation of NovaLogic's game engine. "
			"Its NovaWorld service runs online play for Joint Operations and newer NovaLogic "
			"titles: log in from the game with this username and password, or on the site to "
			"host servers and manage your account:</p>\n<p>" +
			html_link(home) +
			"</p>\n<p>If you did not make this account, reply to this mail and we will remove it.</p>\n"
			"<p>OpenNova</p>\n");
	return m;
}

MailMessage password_reset_mail(const std::string &to, const std::string &username, const std::string &reset_url) {
	MailMessage m;
	m.kind = "password_reset";
	m.to = to;
	m.subject = "Reset your OpenNova password";
	m.text = "Hello " + username + ",\n\n"
			"Someone asked to reset the password of your OpenNova account " + username +
			". To choose a new one, open this link within the hour:\n\n" + reset_url +
			"\n\n"
			"The link works once. If you did not ask for it, ignore this mail: your password "
			"stays as it is.\n\n"
			"OpenNova\n";
	const std::string name = html_escape(username);
	m.html = html_page("<p>Hello " + name + ",</p>\n<p>Someone asked to reset the password of your OpenNova account <strong>" +
			name + "</strong>. To choose a new one, open this link within the hour:</p>\n<p>" + html_link(reset_url) +
			"</p>\n<p>The link works once. If you did not ask for it, ignore this mail: your password stays as it "
			"is.</p>\n<p>OpenNova</p>\n");
	return m;
}

} // namespace opennova::novaworld_server
