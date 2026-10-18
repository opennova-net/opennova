# opennova_novaworld_common: the NovaWorld service's shared library, what more
# than one of its six programs uses (README.md "Programs"): the shared boot and
# main loop (program/program.*), the config, the health (the watchdog, the heartbeat,
# the /livez listener), the address an ECS task finds from its own metadata,
# the games catalogue; with the HTTP layer, the accounts and sessions, the
# Redis pool, the mail, the background jobs' queue, pool and scheduler, the
# hosted servers' and billing's logic, and what several programs build alike
# from the same settings (parts.*). Each program's own code and its main()
# live in apps/novaworld_<program> (gate, lobby, web, fleet, worker, data).
set(NOVAWORLD_COMMON_SOURCES
    program/program.cpp
    # Each program's own minute of use (its program_samples rows), and where
    # the logs are (ONNET_LOG_STORE) with each line's copy into a local store.
    program/program_usage.cpp
    core/program_log.cpp
    program/server_config.cpp
    program/catalog_repository.cpp
    # The stall watchdog and the heartbeat line (core/health.h), GET /livez on
    # ONNET_HEALTH_PORT for the programs with no HTTP of their own
    # (core/health_listener.h), and an ECS task's own address (core/ecs_metadata.h:
    # the fleet's private bind).
    core/health.cpp
    core/health_listener.cpp
    core/ecs_metadata.cpp
    # What a Linux machine or container uses, from the kernel's files (hostd's
    # machine stats, each program's own samples; hostd compiles it in too).
    core/proc_stats.cpp
    # The object store the game data lives in (ADR 0064): the seam, S3 through
    # the AWS CLI, and a directory for a developer's machine and the tests.
    core/object_store.cpp
    core/s3_object_store.cpp
    core/local_object_store.cpp
)

if(BUILD_NOVAWORLD_HTTP)
    list(APPEND NOVAWORLD_COMMON_SOURCES
        accounts/auth.cpp
        accounts/web_session.cpp
        accounts/template_engine.cpp
        # Web's shared state lives in Redis: the client and its pool (the
        # stores over it are web's own, apps/novaworld_web).
        redis_client.cpp
        # Account email: the outbound mail (SES through the AWS CLI, the
        # outbox, the templates) and the password reset links.
        accounts/mailer.cpp
        accounts/password_reset.cpp
        # What several programs build from the same settings: the hosted
        # servers' setup, billing, mail, the periodic jobs.
        parts.cpp
        # The background jobs: the queue in the database, the worker pool, the
        # periodic jobs' scheduler and the prunes.
        jobs/job_queue.cpp
        jobs/maintenance_jobs.cpp
        jobs/scheduler.cpp
        jobs/worker_pool.cpp
        accounts/account_jobs.cpp
        # Hosted servers (ADR 0053): their rows and renders, the admin links'
        # states and the console queue as the web routes read them, the
        # per-server stats (samples, hourly rollups); the fleet (its rows,
        # placement on the machines' slots, its jobs, the EC2 machine driver and
        # the local stack's docker one, the AWS CLI through apps/common's
        # command runner, the machine agents' protocol, its settings, the
        # capacity it sells, its health as findings over its rows); the static
        # addresses (their rows and names, their jobs, the EC2 and docker
        # address drivers, the Route 53 and file zones). The supervisor, the
        # admin links and the stats sampler are the fleet program's own
        # (apps/novaworld_fleet).
        hosted/model/address_jobs.cpp
        hosted/model/address_names.cpp
        hosted/model/address_repository.cpp
        hosted/cloud/address_zone.cpp
        hosted/model/admin_link_status.cpp
        hosted/protocol/agent_protocol.cpp
        hosted/model/console_queue.cpp
        data_sets/data_set_files.cpp
        data_sets/data_set_jobs.cpp
        data_sets/data_set_repository.cpp
        hosted/cloud/docker_address_driver.cpp
        hosted/cloud/docker_machine_driver.cpp
        hosted/cloud/ec2_address_driver.cpp
        hosted/cloud/ec2_machine_driver.cpp
        hosted/model/fleet_config.cpp
        hosted/model/fleet_health.cpp
        hosted/model/fleet_jobs.cpp
        hosted/model/fleet_repository.cpp
        hosted/model/fleet_roll.cpp
        hosted/model/host_limits.cpp
        hosted/model/hosted_repository.cpp
        hosted/model/hosted_settings.cpp
        hosted/model/limits_hold.cpp
        hosted/model/net_cidr.cpp
        core/owner_file.cpp
        hosted/model/placement.cpp
        hosted/model/server_phase.cpp
        hosted/model/sold_capacity.cpp
        hosted/model/stats_jobs.cpp
        hosted/model/stats_repository.cpp
        # Billing (monthly support tiers over Stripe Checkout and the Customer
        # Portal): the settings, the billing tables and the hosting entitlement
        # read from them, the sync keeping them in step with Stripe, the curl
        # transport (apps/common/command_runner.h, its config file owner-only
        # through core/owner_file.cpp) and the typed client.
        billing/billing_config.cpp
        billing/billing_jobs.cpp
        billing/billing_repository.cpp
        billing/billing_sync.cpp
        billing/entitlement.cpp
        billing/stripe_api.cpp
        billing/stripe_client.cpp
    )
endif()

add_library(opennova_novaworld_common STATIC ${NOVAWORLD_COMMON_SOURCES})

target_include_directories(opennova_novaworld_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# The service target (the one libpq consumer), the wire group (gate/session
# legs, npwire, napi, novacrypto — opennova_net), and the runtime group for
# the in-match host session the JO listener hosts peers through
# (runtime/inmatch, ADR 0043 d4).
target_link_libraries(opennova_novaworld_common
    PUBLIC
        opennova_novaworld_service
        opennova_runtime
        opennova_net
        opennova_app_common
        opennova_bcrypt
)

# PUBLIC so the programs' HTTP layer sees Crow and OPENNOVA_HTTP_ENABLED.
if(BUILD_NOVAWORLD_HTTP)
    target_link_libraries(opennova_novaworld_common PUBLIC Crow::Crow)
    target_compile_definitions(opennova_novaworld_common PUBLIC
        OPENNOVA_HTTP_ENABLED
    )

    # The Redis client (redis_client.h): the system hiredis and hiredis_ssl
    # (libhiredis-dev to build, libhiredis1.1.0 to run) and OpenSSL
    # (libssl-dev) for rediss://, behind one interface target the containment
    # lint can name (scripts/lint/link_graph_check.py): only this library
    # links opennova_redis, and whatever reaches it links this library
    # directly, as libpq rides opennova_novaworld_service alone.
    find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
    find_library(HIREDIS_LIBRARY hiredis)
    find_library(HIREDIS_SSL_LIBRARY hiredis_ssl)
    if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY OR NOT HIREDIS_SSL_LIBRARY)
        message(FATAL_ERROR "The NovaWorld service's HTTP layer needs hiredis and hiredis_ssl "
            "(Debian/Ubuntu: apt install libhiredis-dev)")
    endif()
    find_package(OpenSSL REQUIRED)
    add_library(opennova_redis INTERFACE)
    target_include_directories(opennova_redis INTERFACE ${HIREDIS_INCLUDE_DIR})
    target_link_libraries(opennova_redis INTERFACE
        ${HIREDIS_SSL_LIBRARY} ${HIREDIS_LIBRARY} OpenSSL::SSL OpenSSL::Crypto)
    target_link_libraries(opennova_novaworld_common PRIVATE opennova_redis)
endif()

# Crow uses some C++17 (filesystem) and some 20-flavored bits Crow's
# CMakeLists asks for. Match Crow's request to avoid mismatches.
target_compile_features(opennova_novaworld_common PUBLIC cxx_std_17)

if(WIN32)
    # advapi32: the hosted servers' owner-only file DACLs (SetSecurityInfo,
    # core/owner_file.cpp).
    target_link_libraries(opennova_novaworld_common PUBLIC ws2_32 advapi32)
endif()
