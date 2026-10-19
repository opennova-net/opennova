#include "address_jobs.h"

#include "jobs/job_queue.h"

#include <string>

namespace opennova::novaworld_server::hosted {

namespace {

std::string allocate_key(int64_t address_id) {
	return std::string(kJobAddressAllocate) + ":" + std::to_string(address_id);
}

std::string associate_key(int64_t machine_id) {
	return std::string(kJobAddressAssociate) + ":" + std::to_string(machine_id);
}

} // namespace

int64_t enqueue_address_allocate(db::Database &db, int64_t address_id) {
	jobs::NewJob job;
	job.kind = kJobAddressAllocate;
	job.payload.set("address_id", io::json_number(static_cast<double>(address_id)));
	job.dedupe_key = allocate_key(address_id);
	job.max_attempts = 10;
	return jobs::enqueue(db, job);
}

int64_t enqueue_address_associate(db::Database &db, int64_t address_id, int64_t machine_id) {
	jobs::NewJob job;
	job.kind = kJobAddressAssociate;
	job.payload.set("address_id", io::json_number(static_cast<double>(address_id)));
	job.payload.set("machine_id", io::json_number(static_cast<double>(machine_id)));
	job.dedupe_key = associate_key(machine_id);
	// The machine's pending, its previous holder's end: a retry every 5 to 10 s.
	job.max_attempts = 120;
	return jobs::enqueue(db, job);
}

int64_t enqueue_address_release(db::Database &db, int64_t address_id) {
	jobs::NewJob job;
	job.kind = kJobAddressRelease;
	job.payload.set("address_id", io::json_number(static_cast<double>(address_id)));
	job.dedupe_key = std::string(kJobAddressRelease) + ":" + std::to_string(address_id);
	// Its machine's end, the record's sync and TTL: a retry at each wait.
	job.max_attempts = 200;
	return jobs::enqueue(db, job);
}

bool address_allocate_pending(db::Database &db, int64_t address_id) {
	return jobs::job_pending(db, allocate_key(address_id));
}

bool address_associate_pending(db::Database &db, int64_t machine_id) {
	return jobs::job_pending(db, associate_key(machine_id));
}

} // namespace opennova::novaworld_server::hosted
