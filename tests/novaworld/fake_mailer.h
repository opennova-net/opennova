#pragma once

// A Mailer (apps/novaworld_common/mailer.h) that records every message it is
// handed: the route harness's and mailer_test's stand-in for SES. Each send
// answers sent with a message id of its own, or, with `fail` set, as SES's
// refusal would. Thread-safe: a job worker sends while a test reads.

#include "mailer.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace fake_mail {

class FakeMailer final : public opennova::novaworld_server::Mailer {
public:
	using MailMessage = opennova::novaworld_server::MailMessage;
	using MailResult = opennova::novaworld_server::MailResult;

	MailResult send(const MailMessage &message) override {
		std::lock_guard<std::mutex> lock(mu_);
		sent_.push_back(message);
		MailResult result;
		if (fail.load()) {
			result.error = "MessageRejected: the fake refused it";
			return result;
		}
		result.sent = true;
		result.message_id = "fake-" + std::to_string(sent_.size());
		return result;
	}

	// Every message handed over so far, in order.
	std::vector<MailMessage> messages() const {
		std::lock_guard<std::mutex> lock(mu_);
		return sent_;
	}

	void clear() {
		std::lock_guard<std::mutex> lock(mu_);
		sent_.clear();
	}

	std::atomic<bool> fail{ false };

private:
	mutable std::mutex mu_;
	std::vector<MailMessage> sent_;
};

} // namespace fake_mail
