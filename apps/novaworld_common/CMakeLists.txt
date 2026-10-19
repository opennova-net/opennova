# The NovaWorld service's libraries, what more than one of its six programs
# (README.md "Programs") or opennova-hostd uses: nine layers under this one
# include root ("core/x.h", "hosted/model/x.h"), each a static library that
# includes from and links only the layers below it (README.md "Build layout
# and tests"; scripts/lint/include_graph_check.py's SERVICE LAYERS):
#
#   core              the health (the watchdog, the heartbeat, the /livez
#                     listener), an ECS task's own metadata, the kernel's
#                     files, the logs' store, the object store, the owner-only
#                     files and scratch directories, the random secrets; no
#                     database (hostd links it)
#   hosting_protocol  the machine agents' protocol and the slots (hostd's half
#                     too)
#   program           the shared boot and main loop, the config, each
#                     program's own use, the games catalogue
#   jobs              the background jobs' queue, pool and scheduler, the
#                     service's prunes
#   accounts          the accounts and the website's sessions, the password
#                     reset links, the mail, the website's prunes
#   billing           the support tiers over Stripe and the hosting
#                     entitlement
#   data_sets         the hosted data sets' rows, files and jobs
#   hosting_model     the hosted servers' and the fleet's rows and pure rules,
#                     the hosting policy, the static addresses' rows
#   hosting_cloud     what calls a cloud or a zone: the machine and address
#                     drivers, the zones, the jobs' handlers (the worker's)
#
# Each program's own code and its main() live in apps/novaworld_<program>
# (gate, lobby, web, fleet, worker, data). Every library that reaches the
# database links the service directly (scripts/lint/link_graph_check.py's
# libpq containment). Core and the agents' protocol build with the service or
# on Linux (hostd, which builds without the service); the rest with the
# service alone.
if(NOT OPENNOVA_NOVAWORLD_SERVICE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

# core: no database, no program, nothing above it.
add_library(opennova_novaworld_core STATIC
    # The stall watchdog and the heartbeat line (core/health.h), GET /livez on
    # ONNET_HEALTH_PORT for the programs with no HTTP of their own
    # (core/health_listener.h), and an ECS task's own address and image
    # (core/ecs_metadata.h).
    core/health.cpp
    core/health_listener.cpp
    core/ecs_metadata.cpp
    # What a Linux machine or container uses, from the kernel's files (hostd's
    # machine stats, each program's own samples).
    core/proc_stats.cpp
    # Where the logs are (ONNET_LOG_STORE) with each line's copy into a local
    # store.
    core/program_log.cpp
    # The object store the game data lives in (ADR 0064): the seam, S3 through
    # the AWS CLI, a directory for a developer's machine and the tests, and the
    # store as a program runs it.
    core/object_store.cpp
    core/s3_object_store.cpp
    core/local_object_store.cpp
    core/object_store_stack.cpp
    # The owner-only files and scratch directories the AWS CLI, curl and SES
    # calls use, and the random secrets.
    core/owner_file.cpp
    core/random_secret.cpp
    core/scratch_dir.cpp
)
target_include_directories(opennova_novaworld_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The command runner and the HTTP exchange (apps/common, over the net and base
# groups).
target_link_libraries(opennova_novaworld_core PUBLIC opennova_app_common)
target_compile_features(opennova_novaworld_core PUBLIC cxx_std_17)
if(WIN32)
    # advapi32: the owner-only file DACLs (SetSecurityInfo, core/owner_file.cpp).
    target_link_libraries(opennova_novaworld_core PUBLIC ws2_32 advapi32)
endif()

# hosting_protocol: the machine agents' protocol, the one the service and
# opennova-hostd both build.
add_library(opennova_novaworld_hosting_protocol STATIC hosted/protocol/agent_protocol.cpp)
target_link_libraries(opennova_novaworld_hosting_protocol PUBLIC opennova_novaworld_core)

if(NOT OPENNOVA_NOVAWORLD_SERVICE)
    return()
endif()

# program: the boot every program shares, over the database.
add_library(opennova_novaworld_program STATIC
    program/program.cpp
    program/program_usage.cpp
    program/server_config.cpp
    program/catalog_repository.cpp
)
target_link_libraries(opennova_novaworld_program
    PUBLIC
        opennova_novaworld_core
        opennova_novaworld_service
)

# jobs: the queue in the database, the worker pool, the periodic jobs'
# scheduler and the service's prunes.
add_library(opennova_novaworld_jobs STATIC
    jobs/job_queue.cpp
    jobs/maintenance_jobs.cpp
    jobs/scheduler.cpp
    jobs/worker_pool.cpp
)
target_link_libraries(opennova_novaworld_jobs
    PUBLIC
        opennova_novaworld_core
        opennova_novaworld_service
)

# accounts: the accounts and sessions, the retail logins' rows, the password
# reset links, the outbound mail (SES through the AWS CLI, the outbox, the
# templates) and the website's prunes.
add_library(opennova_novaworld_accounts STATIC
    accounts/auth.cpp
    accounts/web_session.cpp
    accounts/template_engine.cpp
    accounts/mailer.cpp
    accounts/mail_stack.cpp
    accounts/password_reset.cpp
    accounts/account_jobs.cpp
)
target_link_libraries(opennova_novaworld_accounts
    PUBLIC
        opennova_novaworld_core
        opennova_novaworld_program
        opennova_novaworld_jobs
        opennova_novaworld_service
        opennova_bcrypt
)

# billing (monthly support tiers over Stripe Checkout and the Customer
# Portal): the settings, the billing tables and the hosting entitlement read
# from them, the sync keeping them in step with Stripe and its jobs, the curl
# transport (apps/common/command_runner.h, its config file owner-only through
# core/owner_file.cpp), the typed client and the stack a program runs.
add_library(opennova_novaworld_billing STATIC
    billing/billing_config.cpp
    billing/billing_jobs.cpp
    billing/billing_repository.cpp
    billing/billing_stack.cpp
    billing/billing_sync.cpp
    billing/entitlement.cpp
    billing/stripe_api.cpp
    billing/stripe_client.cpp
)
target_link_libraries(opennova_novaworld_billing
    PUBLIC
        opennova_novaworld_core
        opennova_novaworld_jobs
        opennova_novaworld_service
)

# data_sets (ADR 0064): the hosted data sets' rows, the files their trees
# hold and the data program's jobs over the object store (the mission
# catalog a tree's missions are read with is the runtime group's).
add_library(opennova_novaworld_data_sets STATIC
    data_sets/data_set_files.cpp
    data_sets/data_set_jobs.cpp
    data_sets/data_set_repository.cpp
)
target_link_libraries(opennova_novaworld_data_sets
    PUBLIC
        opennova_novaworld_core
        opennova_novaworld_jobs
        opennova_novaworld_service
        opennova_runtime
)

# hosting_model (ADR 0053): the hosted servers' rows and renders, the admin
# links' states and the console queue as the web routes read them, the
# per-server stats; the fleet's rows, placement on the machines' slots, its
# settings and the hosting policy, the machines' profile, the capacity it
# sells, its health as findings over its rows, the jobs it enqueues; the
# static addresses' rows and names. The supervisor, the admin links and the
# stats sampler are the fleet program's own (apps/novaworld_fleet).
add_library(opennova_novaworld_hosting_model STATIC
    hosted/model/address_jobs.cpp
    hosted/model/address_names.cpp
    hosted/model/address_repository.cpp
    hosted/model/admin_link_status.cpp
    hosted/model/console_queue.cpp
    hosted/model/fleet_config.cpp
    hosted/model/fleet_health.cpp
    hosted/model/fleet_jobs.cpp
    hosted/model/fleet_repository.cpp
    hosted/model/fleet_roll.cpp
    hosted/model/host_limits.cpp
    hosted/model/hosted_policy.cpp
    hosted/model/hosted_repository.cpp
    hosted/model/hosted_settings.cpp
    hosted/model/limits_hold.cpp
    hosted/model/machine_profile.cpp
    hosted/model/net_cidr.cpp
    hosted/model/placement.cpp
    hosted/model/server_phase.cpp
    hosted/model/sold_capacity.cpp
    hosted/model/stats_jobs.cpp
    hosted/model/stats_repository.cpp
)
target_link_libraries(opennova_novaworld_hosting_model
    PUBLIC
        opennova_novaworld_core
        opennova_novaworld_program
        opennova_novaworld_jobs
        opennova_novaworld_billing
        opennova_novaworld_data_sets
        opennova_novaworld_hosting_protocol
        opennova_novaworld_service
)

# hosting_cloud: the worker's alone. The EC2 machine driver and the local
# stack's docker one (the AWS CLI through apps/common's command runner, the
# Docker Engine API), their settings, the static addresses' EC2 and docker
# drivers with the Route 53 and file zones, and the fleet's and the
# addresses' jobs' handlers over them.
add_library(opennova_novaworld_hosting_cloud STATIC
    hosted/cloud/address_job_handlers.cpp
    hosted/cloud/address_zone.cpp
    hosted/cloud/docker_address_driver.cpp
    hosted/cloud/docker_machine_driver.cpp
    hosted/cloud/driver_config.cpp
    hosted/cloud/ec2_address_driver.cpp
    hosted/cloud/ec2_machine_driver.cpp
    hosted/cloud/fleet_job_handlers.cpp
)
target_link_libraries(opennova_novaworld_hosting_cloud
    PUBLIC
        opennova_novaworld_core
        opennova_novaworld_jobs
        opennova_novaworld_billing
        opennova_novaworld_hosting_protocol
        opennova_novaworld_hosting_model
        opennova_novaworld_service
)
