#include "hosted_policy.h"

#include "address_names.h"
#include "core/loggable.h"
#include "hosted/protocol/agent_protocol.h"

#include <cstdio>
#include <optional>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

// "the s3 store s3://<bucket>/<root>" or "the local store <dir>", for the boot line.
std::string store_text(const ObjectStoreConfig &store) {
	if (store.kind == ObjectStoreConfig::Kind::S3)
		return "the s3 store s3://" + store.bucket + "/" + store.root + " (" + store.region + ")";
	return "the local store " + loggable(store.root, 260);
}

// Whether `host` is no address a machine reaches the gate at: unset, or this
// system's own loopback.
bool loopback_host(const std::string &host) {
	return host.empty() || host == "localhost" || host.rfind("127.", 0) == 0;
}

// The static addresses' policy over a hosting policy that is on: on
// (addresses_on) with ONNET_FLEET_MAX_ADDRESSES above 0 and a zone name in
// ONNET_HOSTED_ADDRESS_ZONE, else none sold (no name: a boot warning).
void address_policy(HostedPolicy &policy) {
	PlacementPolicy &placement = policy.fleet.placement;
	if (placement.max_addresses <= 0)
		return;
	if (!is_zone_name(policy.fleet.address_zone)) {
		std::fprintf(stderr,
				"[boot] WARN static addresses: ONNET_HOSTED_ADDRESS_ZONE is no zone name "
				"(play.<domain>); none sold (ONNET_FLEET_MAX_ADDRESSES %d ignored)\n",
				placement.max_addresses);
		placement.max_addresses = 0;
		return;
	}
	policy.addresses_on = true;
	std::printf("[boot] static addresses: up to %d, named in %s\n", placement.max_addresses,
			policy.fleet.address_zone.c_str());
}

// The hosted servers' policy but the static addresses' (hosted_policy).
HostedPolicy servers_policy(const ServerConfig &config) {
	HostedPolicy setup;
	// The store first, driver or none: web's data set routes run over it
	// whether or not hosting is on.
	if (const auto store = object_store_of(config)) {
		setup.store = *store;
		setup.store_ok = true;
	}
	if (config.hosted_provider == "none")
		return setup;
	if (config.hosted_provider != "docker" && config.hosted_provider != "ec2") {
		std::fprintf(stderr,
				"[boot] WARN hosted servers: unknown ONNET_HOSTED_PROVIDER '%s'; hosting off\n",
				loggable(config.hosted_provider).c_str());
		return setup;
	}
	std::vector<std::string> errors;
	const EnvLookup env = process_env();
	parse_fleet_config(
			[&env, &config](const char *name) -> std::optional<std::string> {
				if (!config.internal_bind.empty() &&
						std::string_view(name) == "ONNET_INTERNAL_HTTP_BIND")
					return config.internal_bind;
				return env(name);
			},
			setup.fleet, errors);
	if (!errors.empty()) {
		for (const std::string &e : errors)
			std::fprintf(stderr, "[boot] WARN hosted servers: %s\n", loggable(e, 300).c_str());
		std::fprintf(stderr,
				"[boot] WARN hosted servers: the fleet's settings do not read; hosting off\n");
		return setup;
	}
	// The data sets' trees live in the store: no store, no hosting (ADR 0064).
	if (!setup.store_ok) {
		std::fprintf(stderr,
				"[boot] WARN hosted servers: no object store (ONNET_OBJECT_STORE_URL, s3://<bucket>/data "
				"with ONNET_OBJECT_STORE_REGION, or file:///<dir>), where the data sets' trees live; "
				"hosting off\n");
		return setup;
	}
	// The servers list through the service's public address, so the listing
	// records each machine's own (ADR 0053); a local stack's machines are
	// containers of their own, which reach the gate there, never at their own
	// loopback.
	if (loopback_host(config.public_host)) {
		std::fprintf(stderr,
				"[boot] WARN hosted servers: %s needs ONNET_PUBLIC_HOST, the public address "
				"the servers list on (not loopback); hosting off\n",
				config.hosted_provider.c_str());
		return setup;
	}
	if (!is_image_reference(setup.fleet.serve_image)) {
		std::fprintf(stderr,
				"[boot] WARN hosted servers: ONNET_HOSTED_SERVE_IMAGE is no image reference; "
				"hosting off\n");
		return setup;
	}
	// What the supervisor knows of the driver's machines needs no driver: the
	// address their admin.cfg admits is the fleet's bind.
	setup.profile = config.hosted_provider == "ec2"
			? ec2_machine_profile(setup.fleet.internal_address)
			: docker_machine_profile(setup.fleet.internal_address);
	setup.provider = config.hosted_provider;
	setup.on = true;
	std::printf("[boot] hosted servers: %s, serve image %s, the data sets' trees in %s\n",
			setup.provider.c_str(), loggable(setup.fleet.serve_image, 255).c_str(),
			store_text(setup.store).c_str());
	return setup;
}

} // namespace

HostedPolicy hosted_policy(const ServerConfig &config) {
	HostedPolicy policy = servers_policy(config);
	// Hosting off sells no static address, whatever ONNET_FLEET_MAX_ADDRESSES says.
	if (policy.on)
		address_policy(policy);
	else
		policy.fleet.placement.max_addresses = 0;
	return policy;
}

} // namespace opennova::novaworld_server::hosted
