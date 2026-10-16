# opennova-novaworld-server: the NovaWorld service. Every source but main.cpp
# builds as opennova_novaworld_server_core (mirroring opennova_serve_core), so
# tests drive the listeners in-process; the exe is main.cpp over it.
set(NOVAWORLD_SERVER_CORE_SOURCES
    server_config.cpp
    catalog_repository.cpp
    gate_listener.cpp
    # The stall watchdog and the heartbeat line (GET /healthz reads the first).
    health.cpp
    nw_udp_listener.cpp
)

if(BUILD_NOVAWORLD_HTTP)
    list(APPEND NOVAWORLD_SERVER_CORE_SOURCES
        http_listener.cpp
        # The admin's user routes (moved out of http_listener.cpp whole).
        admin_user_routes.cpp
        host_routes.cpp
        # CD keys (migration 0015): the keys, the DFX2 activations, their routes.
        cd_keys.cpp
        cd_key_routes.cpp
        template_engine.cpp
        session_store.cpp
        auth.cpp
        rate_limiter.cpp
        web_access.cpp
        web_session.cpp
        # Account email: the outbound mail (SES through the AWS CLI, the
        # outbox, the templates) and the password reset links and routes.
        mailer.cpp
        password_reset.cpp
        password_reset_routes.cpp
        # Hosted servers (ADR 0053, proposed): the supervisor, its rows and
        # renders, the admin link and its console routes, the per-server stats
        # (samples, hourly rollups, their routes); the fleet (its rows,
        # placement on the machines' slots, the local and the EC2 machine
        # drivers, the AWS CLI through apps/common's command runner, the
        # machine agents' routes on the internal listener, its settings).
        hosted/admin_link.cpp
        hosted/agent_protocol.cpp
        hosted/agent_route.cpp
        hosted/console_commands.cpp
        hosted/console_routes.cpp
        hosted/data_sets.cpp
        hosted/ec2_machine_driver.cpp
        hosted/fleet_config.cpp
        hosted/fleet_repository.cpp
        hosted/fleet_routes.cpp
        hosted/hosted_repository.cpp
        hosted/hosted_routes.cpp
        hosted/hosted_settings.cpp
        hosted/internal_listener.cpp
        hosted/local_machine_driver.cpp
        hosted/net_cidr.cpp
        hosted/owner_file.cpp
        hosted/placement.cpp
        hosted/stats_repository.cpp
        hosted/stats_routes.cpp
        hosted/stats_sampler.cpp
        hosted/supervisor.cpp
        # Billing (monthly support tiers over Stripe Checkout and the Customer
        # Portal): the settings, the billing tables and the hosting entitlement
        # read from them, the sync worker keeping them in step with Stripe,
        # the curl transport (apps/common/command_runner.h, its config file
        # owner-only through hosted/owner_file.cpp), the typed client and the
        # webhook signature.
        billing/billing_config.cpp
        billing/billing_repository.cpp
        billing/billing_routes.cpp
        billing/billing_sync.cpp
        billing/entitlement.cpp
        billing/stripe_api.cpp
        billing/stripe_client.cpp
        billing/stripe_signature.cpp
    )
endif()

add_library(opennova_novaworld_server_core STATIC ${NOVAWORLD_SERVER_CORE_SOURCES})

target_include_directories(opennova_novaworld_server_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# The service target (the one libpq consumer), the wire group (gate/session
# legs, npwire, napi, novacrypto — opennova_net), and the runtime group for
# the in-match host session the JO listener hosts peers through
# (runtime/inmatch, ADR 0043 d4).
target_link_libraries(opennova_novaworld_server_core
    PUBLIC
        opennova_novaworld_service
        opennova_runtime
        opennova_net
        opennova_app_common
        opennova_bcrypt
)

# PUBLIC so main.cpp's #ifdef OPENNOVA_HTTP_ENABLED blocks see the HTTP layer.
if(BUILD_NOVAWORLD_HTTP)
    target_link_libraries(opennova_novaworld_server_core PUBLIC Crow::Crow)
    target_compile_definitions(opennova_novaworld_server_core PUBLIC
        OPENNOVA_HTTP_ENABLED
    )
endif()

# Crow uses some C++17 (filesystem) and some 20-flavored bits Crow's
# CMakeLists asks for. Match Crow's request to avoid mismatches.
target_compile_features(opennova_novaworld_server_core PUBLIC cxx_std_17)

if(WIN32)
    # advapi32: the hosted servers' owner-only file DACLs (SetSecurityInfo,
    # hosted/owner_file.cpp).
    target_link_libraries(opennova_novaworld_server_core PUBLIC ws2_32 advapi32)
endif()

add_executable(opennova_novaworld_server main.cpp)

# The service edge is direct as well: the libpq-containment lint
# (scripts/lint/link_graph_check.py) wants every target that reaches libpq to
# list opennova_novaworld_service itself.
target_link_libraries(opennova_novaworld_server
    PRIVATE
        opennova_novaworld_server_core
        opennova_novaworld_service
)

target_compile_features(opennova_novaworld_server PRIVATE cxx_std_17)

set_target_properties(opennova_novaworld_server PROPERTIES
    OUTPUT_NAME "opennova-novaworld-server"
)
