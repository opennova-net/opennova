#pragma once

// The static addresses' cloud and DNS calls as jobs (jobs/job_queue.h; ADR
// 0066): the routes and the fleet's pass never call an AddressDriver
// (hosted/cloud/address_driver.h) or a ZoneDriver (hosted/cloud/address_zone.h);
// they enqueue these, in the transaction of the row change that causes them,
// and the workers run them (hosted/cloud/address_job_handlers.h). Each is idempotent, and a step that must wait (a machine still
// pending, a record still draining) is a retry at the wait. Service policy,
// not a port.
//
//   address.allocate   an allocating address found by its tag (a run cut
//                      short after its allocation), else allocated, with its
//                      tags; then its A record written (the name it had before
//                      deleted first: an admin's rename): allocating -> ready.
//                      The cloud's refusal fails the row with its error code
//                      (an exhausted quota shows there). With the add-on off
//                      no address is allocated: the run waits. Deduped per
//                      address, one running at a time across the workers. A
//                      ready address's run writes its record again (a rename,
//                      a release undone).
//   address.associate  a ready address associated with the machine launched
//                      to carry it, never taken from another (no
//                      reassociation), and only once the machine that held it
//                      before has ended; retried while the machine is pending;
//                      then a machine.describe, so its row's public address
//                      follows (fleet_repository.h: only a describe moves it).
//                      Deduped per machine.
//   address.release    a releasing address: once no live machine carries it
//                      (an ended machine's association goes with it), its A
//                      record deleted and the change in sync, then the
//                      record's TTL (kAddressTtlS) and kReleaseMarginS waited
//                      out; its grant read again (a subscription granting once
//                      more makes it ready again, unless an admin released it);
//                      any association the cloud still shows ended; then
//                      released (an address the cloud no longer knows is
//                      released already). Deduped per address.
//
// And fleet.sweep's addresses (hosted/cloud/address_job_handlers.h
// sweep_addresses): every address the cloud holds for this deployment that no
// held row names is recorded as an orphan (hosted_address_orphans), never
// released.

#include <net/novaworld/db/database.h>

#include <cstdint>

namespace opennova::novaworld_server::hosted {

inline constexpr const char *kJobAddressAllocate = "address.allocate";
inline constexpr const char *kJobAddressAssociate = "address.associate";
inline constexpr const char *kJobAddressRelease = "address.release";

// The A records' TTL: the wait a released address's record drains for.
inline constexpr int64_t kAddressTtlS = 300;
// The wait past a deleted record's TTL before its address goes.
inline constexpr int64_t kReleaseMarginS = 60;

// Each on `db`, in the caller's transaction; 0 when one is queued or running.
int64_t enqueue_address_allocate(db::Database &db, int64_t address_id);
int64_t enqueue_address_associate(db::Database &db, int64_t address_id, int64_t machine_id);
int64_t enqueue_address_release(db::Database &db, int64_t address_id);
// Whether machine `machine_id`'s association is queued or running.
bool address_associate_pending(db::Database &db, int64_t machine_id);

} // namespace opennova::novaworld_server::hosted
