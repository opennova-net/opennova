#pragma once

// An ECS task's metadata endpoint (version 4) and what EC2 answers about its
// interface, for the tests of what a task finds from its own metadata
// (apps/novaworld_common/ecs_metadata.h): ecs_metadata_test drives the
// functions with them, programs_test boots the real lobby against them.
//
// - kTask: GET <uri>/task's reply for a Fargate task (awsvpc): its interface's
//   private IPv4 10.44.1.37 and MAC, the region in its task ARN (us-east-1).
// - kInterfaces: `aws ec2 describe-network-interfaces --output json`'s answer
//   for that interface, its public IPv4 3.91.20.7.
// - FakeMetadata: the endpoint, a test HTTP server answering every request
//   with one body (common/http_reply_server.h), uri() the value
//   ECS_CONTAINER_METADATA_URI_V4 would hold.

#include "common/http_reply_server.h"

#include <string>

namespace fake_ecs {

inline constexpr const char *kTask = R"({
  "Cluster": "arn:aws:ecs:us-east-1:111122223333:cluster/opennova-prod",
  "TaskARN": "arn:aws:ecs:us-east-1:111122223333:task/opennova-prod/0123456789abcdef",
  "Family": "opennova-prod-lobby",
  "Containers": [
    {
      "Name": "lobby",
      "Networks": [
        {
          "NetworkMode": "awsvpc",
          "IPv4Addresses": ["10.44.1.37"],
          "AttachmentIndex": 0,
          "MACAddress": "0e:9e:32:c7:48:85",
          "IPv4SubnetCIDRBlock": "10.44.1.0/24",
          "PrivateDNSName": "ip-10-44-1-37.ec2.internal",
          "SubnetGatewayIpv4Address": "10.44.1.1/24"
        }
      ]
    }
  ],
  "AvailabilityZone": "us-east-1b",
  "LaunchType": "FARGATE"
})";

inline constexpr const char *kInterfaces = R"({
  "NetworkInterfaces": [
    {
      "Association": {
        "IpOwnerId": "amazon",
        "PublicDnsName": "ec2-3-91-20-7.compute-1.amazonaws.com",
        "PublicIp": "3.91.20.7"
      },
      "NetworkInterfaceId": "eni-0123456789abcdef0",
      "PrivateIpAddress": "10.44.1.37"
    }
  ]
})";

// The task metadata endpoint: uri() is what ECS_CONTAINER_METADATA_URI_V4 would
// hold, its GET <uri>/task answered with the canned body.
class FakeMetadata : public test_http::ReplyServer {
public:
	using ReplyServer::ReplyServer;
	std::string uri() const { return base_url() + "/v4/abc-123"; }
};

} // namespace fake_ecs
