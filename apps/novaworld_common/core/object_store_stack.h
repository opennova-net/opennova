#pragma once

// The object store as a program runs it (web's data set routes and the data
// program's data set jobs, each over a store of its own; the fleet holds none,
// its supervisor gets the store's URL of a tree as a pure function): S3 through
// the AWS CLI, or the local directory. Service policy, not a port.

#include "command_runner.h"
#include "object_store.h"
#include "scratch_dir.h"

#include <memory>
#include <string>

namespace opennova::novaworld_server {

// S3ObjectStore over the AWS CLI (the runner with the environment the EC2
// driver's calls run in, an owner-only scratch directory for what the calls
// read and write), or LocalObjectStore over the directory. The members go in
// reverse: the store first, then its runner, then the scratch directory.
struct ObjectStoreStack {
	ScratchDir scratch;
	std::unique_ptr<proc::CommandRunner> runner;
	std::unique_ptr<ObjectStore> store;
};

// The store `store` names, its S3 calls through the AWS CLI at `aws_cli`, or
// none with `error` (no scratch directory).
ObjectStoreStack make_object_store(
		const ObjectStoreConfig &store, const std::string &aws_cli, std::string &error);

} // namespace opennova::novaworld_server
