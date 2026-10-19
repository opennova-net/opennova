#include "object_store_stack.h"

#include "local_object_store.h"
#include "s3_object_store.h"

#include <utility>

namespace opennova::novaworld_server {

ObjectStoreStack make_object_store(
		const ObjectStoreConfig &store, const std::string &aws_cli, std::string &error) {
	ObjectStoreStack s;
	ScratchDir scratch = ScratchDir::make("opennova-store-", error);
	if (scratch.empty()) {
		error = "no scratch directory for the object store" + (error.empty() ? std::string() : ": " + error);
		return s;
	}
	s.scratch = std::move(scratch);
	if (store.kind == ObjectStoreConfig::Kind::Local) {
		s.store = std::make_unique<LocalObjectStore>(store.root);
		return s;
	}
	// S3 through the AWS CLI, as the EC2 driver and the mailer run it: the
	// program's role answers for it (the task role on ECS).
	const std::string scratch_path = s.scratch.path().u8string();
	s.runner = std::make_unique<proc::ProcessCommandRunner>(
			proc::ProcessCommandRunner::service_environment(), scratch_path);
	s.store = std::make_unique<S3ObjectStore>(store, *s.runner, aws_cli, scratch_path);
	return s;
}

} // namespace opennova::novaworld_server
