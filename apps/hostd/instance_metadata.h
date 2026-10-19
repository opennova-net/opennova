#pragma once

// The machine's image, from the EC2 instance metadata service (IMDSv2: a
// session token PUT, then the GET with it), which the agent reads once at its
// start and reports in every sync (agent_protocol.h image_id), so the service
// can tell a machine on an older image from one on the image the launches
// use. Off EC2 nothing answers, and the image is "". Platform plumbing, not a
// port.

#include <string>

namespace opennova::hostd {

// The instance metadata service, as every EC2 machine reaches it.
inline constexpr const char *kInstanceMetadataUrl = "http://169.254.169.254";

// The machine's AMI id (/latest/meta-data/ami-id) from the metadata service at
// `base_url`, each call at most `timeout_ms`: false with `error` when it does
// not answer or answers no image id.
bool read_image_id(
		const std::string &base_url, std::string &out, std::string &error, int timeout_ms = 2000);

} // namespace opennova::hostd
