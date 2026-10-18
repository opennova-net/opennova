#include "address_jobs.h"

#include "address_names.h"
#include "address_repository.h"
#include "billing/billing_repository.h"
#include "billing/entitlement.h"
#include "fleet_jobs.h"
#include "fleet_repository.h"
#include "loggable.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

int64_t member_int(const io::JsonValue &payload, const char *key) {
	const io::JsonValue *v = payload.get(key);
	return v != nullptr && v->is_number() ? static_cast<int64_t>(v->number) : 0;
}

std::string allocate_key(int64_t address_id) {
	return std::string(kJobAddressAllocate) + ":" + std::to_string(address_id);
}

std::string associate_key(int64_t machine_id) {
	return std::string(kJobAddressAssociate) + ":" + std::to_string(machine_id);
}

// The address's step on its own connection.
template <typename Step> auto on_db(jobs::JobContext &ctx, const Step &step) {
	auto lease = ctx.pool.acquire();
	return step(*lease);
}

std::optional<HostedAddress> address_of(jobs::JobContext &ctx, int64_t id) {
	return on_db(ctx, [id](db::Database &db) { return find_address(db, id); });
}

void log_address(const HostedAddress &a, const std::string &what) {
	std::printf("[fleet] address %lld (%s): %s\n", static_cast<long long>(a.id), loggable(a.name, 40).c_str(),
			what.c_str());
}

jobs::JobResult allocate(jobs::JobContext &ctx, AddressDriver &driver, ZoneDriver &zone) {
	const int64_t id = member_int(ctx.job.payload, "address_id");
	std::optional<HostedAddress> a = address_of(ctx, id);
	if (!a || (a->state != AddressState::Allocating && a->state != AddressState::Ready)) return jobs::JobResult::done();
	if (a->allocation_id.empty()) {
		// One a run cut short made already carries the row's tag: it is taken,
		// never a second one.
		AddressResult found = driver.find_tagged(id);
		if (found.call != AddressCall::Done) return jobs::JobResult::retry(found.error);
		if (!found.address) {
			const std::vector<std::string> held = on_db(ctx, [](db::Database &db) {
				std::vector<std::string> ips;
				for (const HostedAddress &h : list_addresses(db)) {
					if (address_is_held(h.state) && !h.public_ip.empty()) ips.push_back(h.public_ip);
				}
				return ips;
			});
			found = driver.allocate(id, a->name, held);
			if (found.call == AddressCall::Refused) {
				on_db(ctx, [&](db::Database &db) { return fail_address(db, id, found.code); });
				std::fprintf(stderr, "[fleet] WARN address %lld (%s): the cloud refused it: %s\n",
						static_cast<long long>(id), loggable(a->name, 40).c_str(), found.error.c_str());
				return jobs::JobResult::fail("the cloud refused the address (" + found.code + ")");
			}
			if (found.call != AddressCall::Done || !found.address) return jobs::JobResult::retry(found.error);
			log_address(*a, "allocated " + found.address->public_ip + " (" + found.address->allocation_id + ")");
		}
		const CloudAddress held = *found.address;
		if (!on_db(ctx, [&](db::Database &db) {
				return record_allocation(db, id, held.allocation_id, held.public_ip);
			}))
			// The row left allocating meanwhile (an admin's release): its release
			// finds this address by its tag.
			return jobs::JobResult::done();
		a = address_of(ctx, id);
		if (!a) return jobs::JobResult::done();
	}
	// Its record: the name it carried before (a rename) deleted first.
	if (a->dns_name == a->name && a->state == AddressState::Ready) return jobs::JobResult::done();
	if (!a->dns_name.empty() && a->dns_name != a->name) {
		const ZoneChange gone = zone.remove(a->dns_name, a->public_ip);
		if (!gone.ok) return jobs::JobResult::retry(gone.error);
	}
	const ZoneChange written = zone.upsert(a->name, a->public_ip);
	if (!written.ok) return jobs::JobResult::retry(written.error);
	if (on_db(ctx, [&](db::Database &db) { return record_dns_upsert(db, id, a->name, written.change_id); }))
		log_address(*a, "ready at " + a->public_ip + " as " + address_hostname(a->name, zone.zone()));
	return jobs::JobResult::done();
}

jobs::JobResult associate(jobs::JobContext &ctx, AddressDriver &driver) {
	const int64_t id = member_int(ctx.job.payload, "address_id");
	const int64_t machine_id = member_int(ctx.job.payload, "machine_id");
	const std::optional<HostedAddress> a = address_of(ctx, id);
	if (!a || a->state != AddressState::Ready || a->allocation_id.empty()) return jobs::JobResult::done();
	const auto machines = on_db(ctx, [id](db::Database &db) { return live_machines_carrying(db, id); });
	const auto mine = std::find_if(machines.begin(), machines.end(), [&](const FleetMachineRow &m) { return m.id == machine_id; });
	if (mine == machines.end() || mine->cloud_id.empty()) return jobs::JobResult::done();
	// The machine that held it before keeps it until its row ends: no
	// reassociation ever takes it from a running server.
	for (const FleetMachineRow &m : machines) {
		if (m.id != machine_id)
			return jobs::JobResult::retry(
					"machine " + std::to_string(m.id) + " still holds the address", 10 * 1000);
	}
	CloudAddress held;
	held.allocation_id = a->allocation_id;
	held.public_ip = a->public_ip;
	const AddressResult r = driver.associate(held, mine->cloud_id);
	if (r.call == AddressCall::Retry) return jobs::JobResult::retry(r.error, 5 * 1000);
	if (r.call == AddressCall::Refused) {
		std::fprintf(stderr, "[fleet] WARN address %lld to machine %lld: refused: %s\n", static_cast<long long>(id),
				static_cast<long long>(machine_id), r.error.c_str());
		return jobs::JobResult::fail("the cloud refused the association (" + r.code + ")");
	}
	log_address(*a, "associated with machine " + std::to_string(machine_id) + " (" + mine->cloud_id + ")");
	// Only a describe moves the row's public address: the supervisor starts the
	// machine's server once it shows this one.
	on_db(ctx, [&](db::Database &db) { return enqueue_machine_describe(db, machine_id, mine->cloud_id); });
	return jobs::JobResult::done();
}

jobs::JobResult release(jobs::JobContext &ctx, AddressDriver &driver, ZoneDriver &zone) {
	const int64_t id = member_int(ctx.job.payload, "address_id");
	const std::optional<HostedAddress> a = address_of(ctx, id);
	if (!a || a->state != AddressState::Releasing) return jobs::JobResult::done();
	const auto machines = on_db(ctx, [id](db::Database &db) { return live_machines_carrying(db, id); });
	if (!machines.empty())
		return jobs::JobResult::retry("machine " + std::to_string(machines.front().id) + " still carries it", 15 * 1000);
	const int64_t now = billing::unix_now();
	// Its record: deleted, in sync, then its TTL drained.
	if (a->dns_state == "pending") {
		const std::optional<bool> synced = zone.in_sync(a->dns_change_id);
		if (!synced || !*synced) return jobs::JobResult::retry("its record's delete is not in sync yet", 10 * 1000);
		const int64_t after = now + kAddressTtlS + kReleaseMarginS;
		on_db(ctx, [&](db::Database &db) { return record_dns_gone(db, id, after); });
		log_address(*a, "its record is gone; released after its TTL");
		return jobs::JobResult::retry("its record's TTL drains", (after - now) * 1000);
	}
	if (!a->dns_name.empty()) {
		const ZoneChange gone = zone.remove(a->dns_name, a->public_ip);
		if (!gone.ok) return jobs::JobResult::retry(gone.error);
		on_db(ctx, [&](db::Database &db) { return record_dns_delete(db, id, gone.change_id); });
		return jobs::JobResult::retry("its record's delete is not in sync yet", 5 * 1000);
	}
	if (a->release_after == 0) {
		// It never had a record (a failed or unfinished allocation): nothing drains.
		on_db(ctx, [&](db::Database &db) { return record_dns_gone(db, id, now); });
	} else if (now < a->release_after) {
		return jobs::JobResult::retry("its record's TTL drains", (a->release_after - now) * 1000);
	}
	// Its grant, read again: a subscription that grants once more keeps it
	// (but an admin's release stands).
	if (a->source == "subscription" && a->lapse_cause == kLapseGrant) {
		const bool grants = on_db(ctx, [&](db::Database &db) {
			const auto row = billing::find_subscription(db, a->stripe_subscription_id);
			return row && billing::subscription_grants(*row, now);
		});
		if (grants) {
			on_db(ctx, [&](db::Database &db) {
				db::Transaction tx(db);
				const bool restored = restore_address(db, id);
				if (restored) enqueue_address_allocate(db, id);
				tx.commit();
				return restored;
			});
			log_address(*a, "its subscription grants again: kept, ready");
			return jobs::JobResult::done();
		}
	}
	// The cloud's address: the row's, or one a cut-short allocation left.
	std::string allocation = a->allocation_id;
	if (allocation.empty()) {
		const AddressResult found = driver.find_tagged(id);
		if (found.call != AddressCall::Done) return jobs::JobResult::retry(found.error);
		if (found.address) allocation = found.address->allocation_id;
	}
	if (!allocation.empty()) {
		const AddressResult seen = driver.describe(allocation);
		if (seen.call != AddressCall::Done) return jobs::JobResult::retry(seen.error);
		if (seen.address && !seen.address->association_id.empty()) {
			// Associated still (with no live machine of ours: one ending).
			const AddressResult undone = driver.disassociate(seen.address->association_id);
			if (undone.call != AddressCall::Done) return jobs::JobResult::retry(undone.error);
			return jobs::JobResult::retry("disassociated; released next", 5 * 1000);
		}
		if (seen.address) {
			const AddressResult released = driver.release(allocation);
			if (released.call != AddressCall::Done) return jobs::JobResult::retry(released.error, 15 * 1000);
		}
	}
	if (on_db(ctx, [&](db::Database &db) { return mark_released(db, id); })) log_address(*a, "released");
	return jobs::JobResult::done();
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

bool address_associate_pending(db::Database &db, int64_t machine_id) {
	return jobs::job_pending(db, associate_key(machine_id));
}

void register_address_jobs(jobs::JobRegistry &registry, AddressDriver &driver, ZoneDriver &zone) {
	jobs::JobKind allocate_kind{ kJobAddressAllocate,
		[&driver, &zone](jobs::JobContext &ctx) { return allocate(ctx, driver, zone); }, 1 };
	allocate_kind.retry_base_ms = 5 * 1000;
	allocate_kind.retry_max_ms = 5 * 60 * 1000;
	registry.add(std::move(allocate_kind));
	jobs::JobKind associate_kind{ kJobAddressAssociate,
		[&driver](jobs::JobContext &ctx) { return associate(ctx, driver); }, 4 };
	associate_kind.retry_base_ms = 5 * 1000;
	associate_kind.retry_max_ms = 30 * 1000;
	registry.add(std::move(associate_kind));
	jobs::JobKind release_kind{ kJobAddressRelease,
		[&driver, &zone](jobs::JobContext &ctx) { return release(ctx, driver, zone); }, 2 };
	release_kind.retry_base_ms = 10 * 1000;
	release_kind.retry_max_ms = 60 * 1000;
	registry.add(std::move(release_kind));
}

bool sweep_addresses(db::ConnectionPool &pool, AddressDriver &driver) {
	std::vector<CloudAddress> listed;
	try {
		listed = driver.list_tagged();
	} catch (const std::runtime_error &e) {
		std::fprintf(stderr, "[fleet] WARN sweep: %s; no address orphan recorded\n", e.what());
		return false;
	}
	auto lease = pool.acquire();
	db::Database &db = *lease;
	std::vector<AddressOrphan> orphans;
	for (const CloudAddress &c : listed) {
		const auto row = c.address_id > 0 ? find_address(db, c.address_id) : std::nullopt;
		// A held row's own (or one it is still allocating): no orphan.
		if (row && address_is_held(row->state) &&
				(row->allocation_id.empty() || row->allocation_id == c.allocation_id))
			continue;
		AddressOrphan o;
		o.allocation_id = c.allocation_id;
		o.public_ip = c.public_ip;
		o.address_id = c.address_id;
		orphans.push_back(std::move(o));
		std::fprintf(stderr, "[fleet] WARN sweep: address %s (%s, row %lld) is no held address's; left for an admin\n",
				loggable(c.allocation_id, 64).c_str(), loggable(c.public_ip, 40).c_str(),
				static_cast<long long>(c.address_id));
	}
	replace_address_orphans(db, orphans);
	return true;
}

} // namespace opennova::novaworld_server::hosted
