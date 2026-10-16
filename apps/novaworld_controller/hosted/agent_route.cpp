#include "agent_route.h"

#include "hosted/fleet_config.h"
#include "hosted/fleet_repository.h"
#include "hosted/hosted_settings.h"
#include "loggable.h"

#include <cstdio>
#include <exception>
#include <limits>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

// The RFC 1918 ranges and loopback: a peer outside them is never answered,
// whatever the configured networks say.
const std::vector<Ipv4Cidr> &private_ranges() {
	static const std::vector<Ipv4Cidr> ranges = {
		{ 0x0A000000u, 0xFF000000u }, // 10.0.0.0/8
		{ 0xAC100000u, 0xFFF00000u }, // 172.16.0.0/12
		{ 0xC0A80000u, 0xFFFF0000u }, // 192.168.0.0/16
		{ 0x7F000000u, 0xFF000000u }, // 127.0.0.0/8
	};
	return ranges;
}

std::string bearer_of(const crow::request &req) {
	const std::string auth = req.get_header_value("Authorization");
	return auth.rfind("Bearer ", 0) == 0 ? auth.substr(7) : std::string();
}

// Whether `peer` is the machine's own private address.
bool from_machine(const std::string &peer, const FleetMachineRow &machine) {
	const auto address = parse_ipv4(machine.private_address);
	return address && ipv4_admitted(peer, { Ipv4Cidr{ *address, ~uint32_t{ 0 } } });
}

crow::response json_ok(std::string body) {
	crow::response res(200);
	res.body = std::move(body);
	res.set_header("Content-Type", "application/json");
	res.set_header("Cache-Control", "no-store");
	return res;
}

} // namespace

AgentRoutes::AgentRoutes(
		db::ConnectionPool &pool, FleetDesiredState &desired, AgentRouteConfig config) :
		pool_(pool),
		desired_(desired),
		config_(std::move(config)),
		throttle_(std::make_shared<LogThrottle>()) {
	std::vector<Ipv4Cidr> kept;
	for (const Ipv4Cidr &cidr : config_.admitted) {
		if (agent_cidr_allowed(cidr))
			kept.push_back(cidr);
		else
			std::fprintf(stderr,
					"[fleet] WARN the agent routes ignore %s: neither private nor loopback\n",
					ipv4_cidr_text(cidr).c_str());
	}
	config_.admitted = std::move(kept);
}

crow::response AgentRoutes::not_found() {
	crow::response res(404);
	res.set_header("Cache-Control", "no-store");
	return res;
}

bool AgentRoutes::admits(const std::string &peer) const {
	return ipv4_admitted(peer, private_ranges()) && ipv4_admitted(peer, config_.admitted);
}

crow::response AgentRoutes::refuse(
		const std::string &peer, const char *route, int64_t id, const std::string &why) {
	if (throttle_->admit(peer))
		std::fprintf(stderr, "[fleet] WARN %s for machine %lld from %s refused: %s\n", route,
				static_cast<long long>(id), loggable(peer).c_str(), why.c_str());
	return not_found();
}

void AgentRoutes::register_routes(crow::SimpleApp &app) {
	CROW_ROUTE(app, "/hosted/machines/<uint>/bootstrap")
			.methods("POST"_method)(
					[this](const crow::request &req, uint64_t id) { return bootstrap(req, id); });
	CROW_ROUTE(app, "/hosted/machines/<uint>/sync")
			.methods("POST"_method)(
					[this](const crow::request &req, uint64_t id) { return sync(req, id); });
	CROW_CATCHALL_ROUTE(app)([] { return not_found(); });
}

crow::response AgentRoutes::bootstrap(const crow::request &req, uint64_t raw_id) {
	// The socket's own peer: no header names the client here.
	const std::string &peer = req.remote_ip_address;
	if (!admits(peer) || raw_id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
		return not_found();
	const auto id = static_cast<int64_t>(raw_id);
	if (req.body.size() > kMaxBodyBytes)
		return refuse(peer, "bootstrap", id, "a body past the cap");
	const std::string token = bearer_of(req);
	if (token.empty())
		return refuse(peer, "bootstrap", id, "no bearer token");
	AgentHello hello;
	std::string error;
	if (!decode_agent_hello(req.body, hello, error))
		return refuse(peer, "bootstrap", id, "a hello that does not read: " + error);
	try {
		auto lease = pool_.acquire();
		db::Database &db = *lease;
		const auto machine = find_machine(db, id);
		if (!machine)
			return refuse(peer, "bootstrap", id, "no such machine");
		if (!from_machine(peer, *machine))
			return refuse(
					peer, "bootstrap", id, "a request from an address that is not the machine's");
		MachineBootstrap reply;
		reply.agent_secret = random_secret(kAgentSecretChars);
		reply.serve_image = machine->serve_image;
		reply.slots = machine->slots;
		// The reply must read before its secret is stored.
		const std::string body = encode_machine_bootstrap(reply);
		MachineBootstrap check;
		if (!decode_machine_bootstrap(body, check, error)) {
			std::fprintf(stderr,
					"[fleet] ERROR machine %lld cannot bootstrap: its reply does not read (%s)\n",
					static_cast<long long>(id), error.c_str());
			return not_found();
		}
		std::string why;
		const auto redeemed = redeem_bootstrap_token(
				db, id, token, agent_secret_digest(reply.agent_secret), hello.agent_version, why);
		if (!redeemed)
			return refuse(peer, "bootstrap", id, why);
		std::printf("[fleet] machine %lld bootstrapped from %s (agent %s, %d slots)\n",
				static_cast<long long>(id), loggable(peer).c_str(),
				loggable(hello.agent_version).c_str(), reply.slots);
		return json_ok(body);
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[fleet] ERROR bootstrap for machine %lld failed: %s\n",
				static_cast<long long>(id), e.what());
		return not_found();
	}
}

crow::response AgentRoutes::sync(const crow::request &req, uint64_t raw_id) {
	const std::string &peer = req.remote_ip_address;
	if (!admits(peer) || raw_id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
		return not_found();
	const auto id = static_cast<int64_t>(raw_id);
	if (req.body.size() > kMaxBodyBytes)
		return refuse(peer, "sync", id, "a body past the cap");
	const std::string secret = bearer_of(req);
	if (secret.empty())
		return refuse(peer, "sync", id, "no bearer secret");
	try {
		auto lease = pool_.acquire();
		db::Database &db = *lease;
		std::string why;
		const auto machine = authenticate_agent(db, id, secret, why);
		if (!machine)
			return refuse(peer, "sync", id, why);
		if (!from_machine(peer, *machine))
			return refuse(peer, "sync", id, "a request from an address that is not the machine's");
		const auto protocol = protocol_of(req.body);
		if (protocol && *protocol != kAgentProtocol) {
			const std::string reason = "its agent speaks protocol " + std::to_string(*protocol) +
					", not " + std::to_string(kAgentProtocol);
			if (transition_machine(db, id,
						{ FleetMachineState::Booting, FleetMachineState::Ready,
								FleetMachineState::Lost },
						FleetMachineState::Draining, reason))
				std::fprintf(stderr, "[fleet] WARN machine %lld drains: %s\n",
						static_cast<long long>(id), reason.c_str());
			return not_found();
		}
		SyncRequest request;
		std::string error;
		if (!decode_sync_request(req.body, request, error))
			return refuse(peer, "sync", id, "a report that does not read: " + error);
		// A listing that failed: the agent is heard from, the slots' reports stand,
		// and the answer is one the agent acts on nothing of.
		const std::optional<FleetMachineRow> after = request.listing_failed
				? record_unlisted_sync(db, id, request.agent_version)
				: record_sync(db, id, request.agent_version, request.containers);
		if (!after)
			return refuse(peer, "sync", id, "a machine that no longer syncs");
		if (request.listing_failed && throttle_->admit(peer))
			std::fprintf(stderr, "[fleet] WARN machine %lld: its agent cannot list its containers\n",
					static_cast<long long>(id));
		if (machine->state == FleetMachineState::Booting &&
				after->state == FleetMachineState::Ready)
			std::printf("[fleet] machine %lld is ready (agent %s)\n", static_cast<long long>(id),
					loggable(after->agent_version).c_str());

		const FleetDecision decision = desired_.sync(db, *after, request.containers);
		std::set<std::string> reported;
		for (const ContainerReport &c : request.containers)
			reported.insert(c.name);
		SyncReply reply;
		reply.drain = decision.drain || after->state == FleetMachineState::Draining;
		reply.trees = decision.trees;
		for (DesiredContainer d : decision.desired) {
			if (!slot_valid(d.slot) || d.slot >= after->slots) {
				std::fprintf(stderr,
						"[fleet] ERROR machine %lld: container %s wants slot %d of %d; left out\n",
						static_cast<long long>(id), loggable(d.name).c_str(), d.slot, after->slots);
				continue;
			}
			d.uid = slot_uid(d.slot);
			if (reported.count(d.name) != 0)
				d.files.reset();
			reply.desired.push_back(std::move(d));
		}
		const std::string body = encode_sync_reply(reply);
		SyncReply check;
		if (!decode_sync_reply(body, check, error)) {
			std::fprintf(stderr,
					"[fleet] ERROR machine %lld: its desired containers do not read (%s); "
					"nothing sent\n",
					static_cast<long long>(id), error.c_str());
			return not_found();
		}
		return json_ok(body);
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[fleet] ERROR sync for machine %lld failed: %s\n",
				static_cast<long long>(id), e.what());
		return not_found();
	}
}

} // namespace opennova::novaworld_server::hosted
