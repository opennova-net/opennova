#pragma once

// Signed reads of AWS's APIs as web's task role, for the admin's
// infrastructure view (infra_status.h). AwsApi runs curl through a
// proc::CommandRunner, as billing's CurlStripeApi does
// (billing/stripe_api.h), and has curl sign each request itself (--aws-sigv4,
// Signature Version 4): a POST to the service's regional endpoint
// (https://<service>.<region>.amazonaws.com/), or to a global service's one
// endpoint signed for its one region (Cost Explorer, Budgets:
// global_json_call), a JSON body with its X-Amz-Target for the JSON APIs (ECS,
// SSM, Cost Explorer, Budgets), a form for the Query APIs (Elastic Load
// Balancing, RDS, ElastiCache, EC2), whose replies are XML (aws_xml.h).
// Not the AWS CLI the worker runs for the fleet: a CLI run spends about a
// second of CPU starting its Python, and a refresh of the view makes a dozen
// calls on web's half vCPU, which serves the retail routes too; a curl run
// costs a few milliseconds.
//
// The credentials are the task role's, which the ECS agent serves each task at
// http://169.254.170.2 plus AWS_CONTAINER_CREDENTIALS_RELATIVE_URI: read for
// each refresh (credentials()), kept no longer, never logged. The key id, the
// secret and the session token travel in the call's own curl config file,
// written owner-only into the owner-only scratch directory and deleted once
// curl ends, never on a command line: argv is exactly {curl, "-q", "--config",
// <file>} (-q first, so no ~/.curlrc is read). HTTPS alone to AWS, plain HTTP to
// the agent's link-local address alone; a 5 s connect and 12 s in all, the
// runner's timeout over that. No error text ever holds a credential.
// Thread-safe: a call keeps no state but its own file. Platform plumbing, not a
// port.

#include "command_runner.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::infra {

struct AwsApiConfig {
	std::string curl = "/usr/bin/curl"; // an absolute path, run as given
	std::string region; // ONNET_INFRA_REGION
	// AWS_CONTAINER_CREDENTIALS_RELATIVE_URI, which the ECS agent sets in every
	// container of a task with a role ("/v2/credentials/<id>"); "" off ECS.
	std::string credentials_uri;
	std::string scratch_dir; // owner-only (core/owner_file.h make_owner_directory)
	int timeout_ms = 15000; // the runner's bound on one curl run
};

struct AwsCredentials {
	std::string key_id;
	std::string secret;
	std::string token; // the session token ("" for long-lived keys)
};

struct AwsReply {
	enum class Outcome {
		Answered, // AWS answered: `status` and `body` are its reply
		Unreachable, // no answer (curl failed, timed out, or never ran): `error` says why
	};
	Outcome outcome = Outcome::Unreachable;
	int status = 0;
	std::string body;
	std::string error;

	bool ok() const { return outcome == Outcome::Answered && status >= 200 && status < 300; }
};

class AwsApi {
public:
	// Where the ECS agent serves the task role's credentials.
	static constexpr const char *kCredentialsHost = "http://169.254.170.2";

	// `runner` must outlive the client; its children's environment should be
	// billing::curl_environment() (no AWS_* or service secret).
	AwsApi(proc::CommandRunner &runner, AwsApiConfig config);

	// "" when `config` can make calls, else why not: an absolute curl path, a
	// region name, a credentials path the agent serves, a scratch directory, a
	// positive timeout.
	static std::string check_config(const AwsApiConfig &config);

	// The task role's credentials as the agent serves them now; false with
	// `error` when it does not answer them.
	bool credentials(AwsCredentials &out, std::string &error);

	// A JSON-protocol call (ECS, SSM): `body` POSTed as
	// application/x-amz-json-1.1 with X-Amz-Target `target`.
	AwsReply json_call(const AwsCredentials &credentials, std::string_view service,
			std::string_view target, const std::string &body);
	// A JSON-protocol call to a global service at its one endpoint, whatever the
	// config's region: `host` (Cost Explorer's "ce.us-east-1.amazonaws.com",
	// Budgets' "budgets.amazonaws.com") signed for `signing_region` (the aws
	// partition's us-east-1, for both).
	AwsReply global_json_call(const AwsCredentials &credentials, std::string_view service,
			std::string_view host, std::string_view signing_region, std::string_view target,
			const std::string &body);
	// A Query-protocol call (Elastic Load Balancing, RDS, ElastiCache, EC2):
	// `params` (Action and Version among them) POSTed as a form.
	AwsReply query_call(const AwsCredentials &credentials, std::string_view service,
			const std::vector<std::pair<std::string, std::string>> &params);

	// Why `reply` is no success, for the admin page: AWS's error code and
	// message from a JSON or an XML error body ("ecs answered 400:
	// AccessDeniedException: User ... is not authorized"), or why there was no
	// answer.
	static std::string failure(std::string_view service, const AwsReply &reply);
	// `params` as application/x-www-form-urlencoded text, in order, every byte
	// but RFC 3986's unreserved characters percent-encoded.
	static std::string query_encode(const std::vector<std::pair<std::string, std::string>> &params);

private:
	// A signed POST to `service` at https://<host>/ (signed for `region`) of
	// `body` as `content_type` (with `target` unless it is empty).
	AwsReply signed_call(const AwsCredentials &credentials, std::string_view service,
			std::string_view host, std::string_view region, std::string_view content_type,
			std::string_view target, const std::string &body);
	// One curl run over the config text `text`: the file written, run, and
	// deleted; `secrets` scrubbed from any error.
	AwsReply run(const std::string &text, const std::vector<std::string> &secrets);

	proc::CommandRunner &runner_;
	AwsApiConfig config_;
};

} // namespace opennova::novaworld_server::infra
