#pragma once

// The deployment's CloudWatch alarms that fire now, for the admin's attention list
// (attention_routes.h): cloudwatch:DescribeAlarms (a Query API: the form in, XML
// back) with web's task role through aws_api.h, the metric alarms in ALARM whose
// names start with the deployment's prefix (ONNET_INFRA_ALARM_PREFIX,
// "opennova-<deployment>-": every alarm infra/aws/observability.tf makes), every
// page of them (kPages at most), the last read kept kCacheMs however many admins
// look. Without AWS (the local stack) nothing is called and the read says alarms
// are not read here. Service policy, not a port.

#include "aws_api.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace opennova::novaworld_server::infra {

struct FiringAlarm {
	std::string name;
	int64_t state_since = 0; // Unix seconds, its move into ALARM (0: not known)
	std::string description;
};

struct AlarmRead {
	bool read = false; // the alarms were read now (or within kCacheMs)
	std::vector<FiringAlarm> alarms; // by name
	std::string unavailable; // why AWS is not read here; "" when it is
	std::string error; // why the read failed (AWS's words); "" when it did not
};

struct AlarmConfig {
	AwsApiConfig aws;
	std::string prefix; // ONNET_INFRA_ALARM_PREFIX
};

// The infrastructure view's AWS settings (infra_status.h) and
// ONNET_INFRA_ALARM_PREFIX.
AlarmConfig alarm_config_from_env(const std::string &scratch_dir);

class AlarmStore {
public:
	static constexpr int64_t kCacheMs = 20 * 1000;
	// DescribeAlarms pages one read follows at most (100 alarms each).
	static constexpr int kPages = 5;

	// Milliseconds on a steady clock (the cache).
	using Clock = std::function<int64_t()>;

	// `runner` must outlive the store.
	AlarmStore(proc::CommandRunner &runner, AlarmConfig config, Clock steady_ms = {});
	AlarmStore(const AlarmStore &) = delete;
	AlarmStore &operator=(const AlarmStore &) = delete;

	// "" when the alarms are read here, else why not (no region, task role or
	// prefix: the local stack's).
	const std::string &unavailable() const { return unavailable_; }

	// The alarms that fire now; the last read within kCacheMs answered again (a
	// failed one is not kept).
	AlarmRead read();

private:
	AlarmRead read_uncached();

	const AlarmConfig config_;
	const std::string unavailable_;
	AwsApi api_;
	Clock steady_;
	std::mutex mu_;
	int64_t cached_at_ = 0;
	bool cached_ = false;
	AlarmRead cache_;
};

} // namespace opennova::novaworld_server::infra
