#include "alarm_store.h"

#include "aws_xml.h"
#include "health.h" // Watchdog::steady_now_ms
#include "infra_status.h"

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace opennova::novaworld_server::infra {

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

// A prefix of an alarm's name: letters, digits, '-' and '_', at most 200 bytes.
bool prefix_ok(const std::string &prefix) {
	if (prefix.empty() || prefix.size() > 200)
		return false;
	return std::all_of(prefix.begin(), prefix.end(), [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
				c == '-' || c == '_';
	});
}

std::string why_unavailable(const AlarmConfig &config) {
	if (const std::string why = AwsApi::check_config(config.aws); !why.empty())
		return "alarms are not read here: " + why;
	if (config.prefix.empty())
		return "alarms are not read here: ONNET_INFRA_ALARM_PREFIX is unset";
	if (!prefix_ok(config.prefix))
		return "ONNET_INFRA_ALARM_PREFIX is no alarm name prefix";
	return {};
}

} // namespace

AlarmConfig alarm_config_from_env(const std::string &scratch_dir) {
	AlarmConfig c;
	c.aws = infra_config_from_env(scratch_dir).aws;
	if (const char *prefix = std::getenv("ONNET_INFRA_ALARM_PREFIX"))
		c.prefix = std::string(strutil::trim_view(prefix));
	return c;
}

AlarmStore::AlarmStore(proc::CommandRunner &runner, AlarmConfig config, Clock steady_ms) :
		config_(std::move(config)),
		unavailable_(why_unavailable(config_)),
		api_(runner, config_.aws),
		steady_(steady_ms ? std::move(steady_ms)
						  : Clock([] { return static_cast<int64_t>(Watchdog::steady_now_ms()); })) {
}

AlarmRead AlarmStore::read() {
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (cached_ && steady_() - cached_at_ < kCacheMs)
			return cache_;
	}
	AlarmRead out = read_uncached();
	if (out.read) {
		std::lock_guard<std::mutex> lock(mu_);
		cache_ = out;
		cached_at_ = steady_();
		cached_ = true;
	}
	return out;
}

AlarmRead AlarmStore::read_uncached() {
	AlarmRead out;
	if (!unavailable_.empty()) {
		out.unavailable = unavailable_;
		return out;
	}
	AwsCredentials creds;
	if (!api_.credentials(creds, out.error))
		return out;
	const Params params = { { "Action", "DescribeAlarms" }, { "Version", "2010-08-01" },
		{ "AlarmNamePrefix", config_.prefix }, { "StateValue", "ALARM" },
		{ "AlarmTypes.member.1", "MetricAlarm" }, { "MaxRecords", "100" } };
	std::string token;
	for (int page = 0; page < kPages; ++page) {
		Params call = params;
		if (!token.empty())
			call.emplace_back("NextToken", token);
		const AwsReply reply = api_.query_call(creds, "monitoring", call);
		if (!reply.ok()) {
			out.error = AwsApi::failure("monitoring", reply);
			out.alarms.clear();
			return out;
		}
		XmlNode root;
		std::string parse_error;
		if (!parse_xml(reply.body, root, parse_error)) {
			out.error = "monitoring answered no XML (" + parse_error + ")";
			out.alarms.clear();
			return out;
		}
		const XmlNode *result = root.child("DescribeAlarmsResult");
		const XmlNode *alarms = result != nullptr ? result->child("MetricAlarms") : nullptr;
		if (alarms != nullptr)
			for (const XmlNode &m : alarms->children) {
				FiringAlarm a;
				a.name = m.child_text("AlarmName");
				if (a.name.empty() || m.child_text("StateValue") != "ALARM")
					continue;
				a.description = m.child_text("AlarmDescription");
				// Its move into ALARM; a reason's update alone moves StateUpdatedTimestamp.
				std::string since = m.child_text("StateTransitionedTimestamp");
				if (since.empty())
					since = m.child_text("StateUpdatedTimestamp");
				a.state_since = std::max<int64_t>(0, iso_seconds(since));
				out.alarms.push_back(std::move(a));
			}
		token = result != nullptr ? result->child_text("NextToken") : std::string();
		if (token.empty())
			break;
	}
	std::sort(out.alarms.begin(), out.alarms.end(),
			[](const FiringAlarm &a, const FiringAlarm &b) { return a.name < b.name; });
	out.read = true;
	return out;
}

} // namespace opennova::novaworld_server::infra
