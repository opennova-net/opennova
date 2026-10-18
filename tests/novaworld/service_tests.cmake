# The NovaWorld service's tests: the service target (opennova_novaworld_service), its
# programs' sources (apps/novaworld_common, the shared library, and apps/novaworld_<program>,
# each program's own) and opennova-serve against the service's listeners.
# tests/CMakeLists.txt includes this file only when the service builds
# (OPENNOVA_NOVAWORLD_SERVICE, off by default; the Linux CI jobs and the programs' images
# turn it on), so its paths are the including file's: ${CMAKE_CURRENT_SOURCE_DIR} is tests/.

# Every test this file adds carries the `service` ctest label, and every target it adds
# builds under the one aggregate target opennova_service_tests: the images' gate
# (.github/workflows/net-linux.yml) builds and runs the service rows by those two, so a row
# added here is in the gate with no hand list to keep (the end of this file applies them).
get_directory_property(OPENNOVA_SERVICE_TESTS_BEFORE TESTS)
get_directory_property(OPENNOVA_SERVICE_TARGETS_BEFORE BUILDSYSTEM_TARGETS)

# The database tests run against a real PostgreSQL server: OPENNOVA_TEST_PG names
# it (a libpq URI whose role may create and drop databases), and each test case
# gets a database of its own there (tests/common/test_database.h). ctest hands
# the URI to the test as `--pg <uri>`; with none set the test prints "SKIP: needs
# a PostgreSQL server" and exits 77, which ctest reports Skipped and the core
# suite fails (scripts/ci/test_suites.py). scripts/dev_postgres.sh runs a local
# one and prints the value; CI runs postgres:17 as a job service.
set(OPENNOVA_TEST_PG "" CACHE STRING
    "PostgreSQL server URI the NovaWorld service's database tests create their databases on")
# The HTTP layer's tests also need a Redis server (Valkey 8; web's shared
# state, apps/novaworld_common/redis_client.h): OPENNOVA_TEST_REDIS
# names it (redis://host:port), ctest hands it to the test as `--redis <url>`,
# and each test keeps its keys under a prefix of its own there
# (tests/common/test_redis.h). With none set the test prints "SKIP: needs a
# Redis server" and exits 77, which the core suite fails, as for PostgreSQL.
# scripts/dev_redis.sh runs a local one and prints the value; CI runs
# valkey/valkey:8 as a job service. A pg test that needs Redis too names REDIS.
set(OPENNOVA_TEST_REDIS "" CACHE STRING
    "Redis (Valkey) server URL the NovaWorld server's HTTP-layer tests keep their keys on")
function(opennova_add_pg_test name)
    cmake_parse_arguments(TEST "REDIS" "" "COMMAND" ${ARGN})
    set(servers)
    if(OPENNOVA_TEST_PG)
        list(APPEND servers --pg ${OPENNOVA_TEST_PG})
    endif()
    set(labels pg)
    if(TEST_REDIS)
        list(APPEND labels redis)
        if(OPENNOVA_TEST_REDIS)
            list(APPEND servers --redis ${OPENNOVA_TEST_REDIS})
        endif()
    endif()
    add_test(NAME ${name} COMMAND ${TEST_COMMAND} ${servers})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77 LABELS "${labels}")
endfunction()
# A test that needs Redis and no database.
function(opennova_add_redis_test name)
    cmake_parse_arguments(TEST "" "" "COMMAND" ${ARGN})
    if(OPENNOVA_TEST_REDIS)
        add_test(NAME ${name} COMMAND ${TEST_COMMAND} --redis ${OPENNOVA_TEST_REDIS})
    else()
        add_test(NAME ${name} COMMAND ${TEST_COMMAND})
    endif()
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77 LABELS redis)
endfunction()

# The real GateListener on port 0 over loopback UDP: the port start() reports is
# the one that answers a probe and the one the reply advertises as POSTIPPORT,
# a second listener on that port fails start(), and each gate tag's reply
# carries its games row's lobby (the repo's migrations and games seed; JO's
# reply byte for byte), an unknown tag none (same listener-source recipe as the
# NW UDP e2e tests below).
add_executable(gate_listener_test novaworld/gate_listener_test.cpp)
target_link_libraries(gate_listener_test PRIVATE
    opennova_novaworld_gate_core opennova_novaworld_common opennova_novaworld_service)
target_include_directories(gate_listener_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(gate_listener_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(gate_listener COMMAND gate_listener_test)

# Connection registry / manager tests
add_executable(connection_registry_test novaworld/connection_registry_test.cpp)
target_link_libraries(connection_registry_test PRIVATE opennova_novaworld_service)
target_include_directories(connection_registry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME connection_registry COMMAND connection_registry_test)

add_executable(connection_manager_test novaworld/connection_manager_test.cpp)
target_link_libraries(connection_manager_test PRIVATE opennova_novaworld_service)
target_include_directories(connection_manager_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME connection_manager COMMAND connection_manager_test)

# The database wrapper (net/novaworld/db/database.h) and its migration runner:
# binds and types, errors and SQLSTATEs, savepoints, read snapshots, the pool's
# bound, the writer lock and its timeout, a reconnect, the migrations' records
# and checksum refusal and the database contract.
add_executable(db_test novaworld/db_test.cpp)
target_link_libraries(db_test PRIVATE opennova_novaworld_service Threads::Threads)
target_include_directories(db_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(db_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(db COMMAND db_test)

# One connection per thread under load (issue #981): registrations race host
# writes and roster readers on one database, each thread on its own pooled
# connection. Compiles the shared library's accounts/auth.cpp (create_user) directly: it joins
# opennova_novaworld_common only under BUILD_NOVAWORLD_HTTP, and this test runs on
# every service build.
add_executable(db_concurrency_test
    novaworld/db_concurrency_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/accounts/auth.cpp)
target_link_libraries(db_concurrency_test
    PRIVATE opennova_novaworld_service opennova_bcrypt Threads::Threads)
target_include_directories(db_concurrency_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
target_compile_definitions(db_concurrency_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(db_concurrency COMMAND db_concurrency_test)

# The NovaWorld site's login sessions and roles (web_sessions, players.role):
# the token kept as its SHA-256, the per-account cap, the sliding expiry and
# its race (threads on pooled connections), the refusals, the revocations, the
# prune and the one-shot bootstrap promotion, on a migrated database. Compiles
# the server's accounts/web_session.cpp and accounts/auth.cpp directly, as db_concurrency_test
# does, so it runs on every service build, not only under BUILD_NOVAWORLD_HTTP.
add_executable(web_session_test
    novaworld/web_session_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/accounts/auth.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/accounts/web_session.cpp)
target_link_libraries(web_session_test
    PRIVATE opennova_novaworld_service opennova_bcrypt Threads::Threads)
target_include_directories(web_session_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
target_compile_definitions(web_session_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(web_session COMMAND web_session_test)

# The service's background jobs (apps/novaworld_common/jobs): the queue in the database,
# the worker pool, the periodic jobs' scheduler. Compiled into the tests that run jobs.
set(OPENNOVA_JOBS_SOURCES
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/jobs/job_queue.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/jobs/scheduler.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/jobs/worker_pool.cpp)

# The job queue on a migrated database (apps/novaworld_common/jobs/job_queue.h,
# worker_pool.h, scheduler.h): enqueue in a transaction (committed or rolled back with
# it), the dedupe and schedule keys, claims by several concurrent workers (each job run
# once, FOR UPDATE SKIP LOCKED), the per-kind caps held across workers, a lease that ran
# out queued again (or failed past its attempts), a claim that lapsed changing nothing,
# the retries' backoff and jitter, the secret dropped as a job ends, the periodic buckets,
# the prune, and the worker pool's threads and keeper. Compiles the job sources directly,
# so it runs on every service build.
add_executable(jobs_test
    novaworld/jobs_test.cpp
    ${OPENNOVA_JOBS_SOURCES}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/jobs/maintenance_jobs.cpp)
target_link_libraries(jobs_test PRIVATE opennova_novaworld_service Threads::Threads)
target_include_directories(jobs_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
target_compile_definitions(jobs_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(jobs COMMAND jobs_test)
set_tests_properties(jobs PROPERTIES TIMEOUT 300)

# The hosted servers' supervisor (apps/novaworld_fleet/hosted/supervisor.h over the
# shared library's apps/novaworld_common/hosted): the settings and
# their renders, the host limits and the offered data sets, the trees pinned at a start
# and carried in the sync, the service accounts, the start request's checks (the
# fleet's room among them), and each server's lifecycle on the fleet over a FakeDriver
# (novaworld/hosted_fake_driver.h) whose machines' agents the test plays through the
# supervisor's sync, on a hand-stepped clock: placement onto one shared machine for two
# servers, a launch and the wait for it, the own machine, the stops, the crash restarts
# and their limit, the timeouts, the health checks, the owners' changes and limits, a lost
# machine placing its servers again with no restart counted, the reconciliation, the
# admin links following the running servers, and a static address's hold, lapse and
# brake. Compiles the server's accounts/auth.cpp,
# accounts/web_session.cpp and the hosted sources directly (the shared library's and the fleet
# program's), as web_session_test does, so it runs on every build, not only under
# BUILD_NOVAWORLD_HTTP.
set(OPENNOVA_HOSTED_SUPERVISOR_SOURCES
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/accounts/auth.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/accounts/web_session.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/billing_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/entitlement.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/address_jobs.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/address_names.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/address_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/admin_link_status.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/agent_protocol.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/data_sets/data_set_files.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/data_sets/data_set_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/fleet_jobs.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/fleet_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/fleet_roll.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/host_limits.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/hosted_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/hosted_settings.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/limits_hold.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/owner_file.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/placement.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/stats_jobs.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/stats_repository.cpp
    # stats.prune rolls up and prunes the programs' own samples too.
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/program/program_usage.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/proc_stats.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/ecs_metadata.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_fleet/hosted/admin_link.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_fleet/hosted/stats_sampler.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_fleet/hosted/supervisor.cpp
    ${OPENNOVA_JOBS_SOURCES})
add_executable(hosted_supervisor_test
    novaworld/hosted_supervisor_test.cpp
    ${OPENNOVA_HOSTED_SUPERVISOR_SOURCES})
target_link_libraries(hosted_supervisor_test
    PRIVATE opennova_novaworld_service opennova_bcrypt opennova_app_common Threads::Threads)
if(WIN32)
    target_link_libraries(hosted_supervisor_test PRIVATE advapi32)
endif()
target_include_directories(hosted_supervisor_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common
    ${CMAKE_SOURCE_DIR}/apps/novaworld_fleet)
target_compile_definitions(hosted_supervisor_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(hosted_supervisor COMMAND hosted_supervisor_test)
set_tests_properties(hosted_supervisor PROPERTIES TIMEOUT 300)

# The narrow locks that replaced the writer lock (net/novaworld/db/database.h): for each
# invariant a check and its write hold, two writers on two connections, the first's
# transaction held open until the second waits on its lock (pg_locks): two starts against
# one account's limit and the cap, a session and a hosting key minted around a password
# reset either way, the status blob folded into a row the lobby is writing, crossing
# usernames and a shared PCID, two first admins, the CD keys' assignment, revocation,
# auto-mint and activation; and the edge's writes beside an open web transaction, never
# waiting. The hosted sources (as hosted_supervisor_test) and cd_keys.cpp compile in.
add_executable(db_locks_test
    novaworld/db_locks_test.cpp
    ${OPENNOVA_HOSTED_SUPERVISOR_SOURCES}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_web/cd_keys.cpp)
target_link_libraries(db_locks_test
    PRIVATE opennova_novaworld_service opennova_bcrypt opennova_app_common Threads::Threads)
if(WIN32)
    target_link_libraries(db_locks_test PRIVATE advapi32)
endif()
target_include_directories(db_locks_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common
    ${CMAKE_SOURCE_DIR}/apps/novaworld_fleet
    ${CMAKE_SOURCE_DIR}/apps/novaworld_web)
target_compile_definitions(db_locks_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(db_locks COMMAND db_locks_test)

# The fleet on the job queue (apps/novaworld_common/hosted/fleet_jobs.h): the supervisor's
# pass never calls the machine driver. Over a FakeDriver whose every call takes a while
# and a worker pool of real threads: twenty servers started at once launch in about one
# call's time (the launches in parallel, every pass short), a worker gone mid-launch has
# its job run again with the same machine id (the same client token) and one machine
# results, and a driver that stalls trips no watchdog. Same sources as
# hosted_supervisor_test, the health policy besides.
add_executable(hosted_fleet_jobs_test
    novaworld/hosted_fleet_jobs_test.cpp
    ${OPENNOVA_HOSTED_SUPERVISOR_SOURCES}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/health.cpp)
target_link_libraries(hosted_fleet_jobs_test
    PRIVATE opennova_novaworld_service opennova_bcrypt opennova_app_common Threads::Threads)
if(WIN32)
    target_link_libraries(hosted_fleet_jobs_test PRIVATE advapi32)
endif()
target_include_directories(hosted_fleet_jobs_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common
    ${CMAKE_SOURCE_DIR}/apps/novaworld_fleet)
target_compile_definitions(hosted_fleet_jobs_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(hosted_fleet_jobs COMMAND hosted_fleet_jobs_test)
set_tests_properties(hosted_fleet_jobs PROPERTIES TIMEOUT 300)

# The fleet program's election (apps/novaworld_fleet/election.h): two fleet processes
# on one database, each over a supervisor of its own and the FakeDriver, the fleet's jobs
# on worker threads and a hand-stepped clock: one leads and alone runs passes, its lock
# connection killed hands over within the standby's lock timeout (the generation moved
# on, the old leader fenced), and the new leader resumes a machine's boot timer and a
# kind's launch backoff from their rows; a leader that stops hands over at once; a pass an
# election overtook launches nothing; a console command its leader claimed and never
# answered is answered link down by the next leader's drain. Same sources as
# hosted_supervisor_test, the election and the console queue and its drain besides.
add_executable(fleet_election_test
    novaworld/fleet_election_test.cpp
    ${OPENNOVA_HOSTED_SUPERVISOR_SOURCES}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_fleet/election.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/console_queue.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_fleet/hosted/console_drain.cpp)
target_link_libraries(fleet_election_test
    PRIVATE opennova_novaworld_service opennova_bcrypt opennova_app_common Threads::Threads)
if(WIN32)
    target_link_libraries(fleet_election_test PRIVATE advapi32)
endif()
target_include_directories(fleet_election_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common
    ${CMAKE_SOURCE_DIR}/apps/novaworld_fleet)
target_compile_definitions(fleet_election_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(fleet_election COMMAND fleet_election_test)
set_tests_properties(fleet_election PROPERTIES TIMEOUT 300)

# The fleet's side of the supervisor (apps/novaworld_fleet/hosted/supervisor.h) over the
# same FakeDriver: the desired state each machine's sync gets (the files until the agent
# reports a container running or exited, the stopping ones, a container rebuilt after a
# service restart), and the machines' lifecycle (a boot that never reports, lost then gone
# or terminated, the mass-lost brake, an agent reporting again, a shared machine idle past
# its time, an own machine ending with its server, a drain moving servers one at a time,
# a roll and its plan, an admin's end, the reconciliation of the driver's machines with the
# rows). Same sources as hosted_supervisor_test.
add_executable(hosted_fleet_test
    novaworld/hosted_fleet_test.cpp
    ${OPENNOVA_HOSTED_SUPERVISOR_SOURCES})
target_link_libraries(hosted_fleet_test
    PRIVATE opennova_novaworld_service opennova_bcrypt opennova_app_common Threads::Threads)
if(WIN32)
    target_link_libraries(hosted_fleet_test PRIVATE advapi32)
endif()
target_include_directories(hosted_fleet_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common
    ${CMAKE_SOURCE_DIR}/apps/novaworld_fleet)
target_compile_definitions(hosted_fleet_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(hosted_fleet COMMAND hosted_fleet_test)
set_tests_properties(hosted_fleet PROPERTIES TIMEOUT 300)

# What each program's tasks use (apps/novaworld_common/program/program_usage.h; migration
# 0018_program_usage.sql) over fixture trees standing in for a container's kernel files and
# a migrated database: the cgroup's CPU limit (v2, v1, none), the sampler (no CPU for its
# first minute, then the percent of the task's limit; the memory's limit from the cgroup,
# the task or the machine; the network's bytes; null where nothing reads), the task found
# off ECS, a minute written once, the hours rolled up from the minutes (a counter that
# started over), the prunes at 48 hours and 400 days.
add_executable(program_usage_test novaworld/program_usage_test.cpp)
target_link_libraries(program_usage_test PRIVATE
    opennova_novaworld_common opennova_novaworld_service)
target_include_directories(program_usage_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(program_usage_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(program_usage COMMAND program_usage_test)

# Where the logs are and a program's copy of its own (apps/novaworld_common/core/program_log.h):
# ONNET_LOG_STORE's forms and refusals, and the tee in the test's own process (stdout's and
# stderr's lines after their times in programs/<program>/<task>.log, an unended line at the
# stop, the descriptors back after it, a directory it cannot make refused). Database-free.
add_executable(program_log_test novaworld/program_log_test.cpp)
target_link_libraries(program_log_test PRIVATE
    opennova_novaworld_common opennova_novaworld_service Threads::Threads)
target_include_directories(program_log_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME program_log COMMAND program_log_test)

# The hosted servers' stats (apps/novaworld_fleet/hosted/stats_sampler.h,
# stats_repository.h; migration 0004_server_stats.sql) on a migrated database and a
# hand-stepped wall clock: one sample a minute of each live server (its listing's players
# and mission, its link's scene, its agent's CPU, memory and egress: the counter's
# differences, a restart of it counted from 0), the unique players counted and never
# stored, the missions, the hourly rollups, the machines' monthly egress, the prune, and the
# routes' reads over each range (the summary, the series' buckets, one server, an account's
# servers, the fleet). Same sources as hosted_supervisor_test.
add_executable(hosted_stats_test
    novaworld/hosted_stats_test.cpp
    ${OPENNOVA_HOSTED_SUPERVISOR_SOURCES})
target_link_libraries(hosted_stats_test
    PRIVATE opennova_novaworld_service opennova_bcrypt opennova_app_common Threads::Threads)
if(WIN32)
    target_link_libraries(hosted_stats_test PRIVATE advapi32)
endif()
target_include_directories(hosted_stats_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common
    ${CMAKE_SOURCE_DIR}/apps/novaworld_fleet)
target_compile_definitions(hosted_stats_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(hosted_stats COMMAND hosted_stats_test)
set_tests_properties(hosted_stats PROPERTIES TIMEOUT 300)

# The service's admin link (apps/novaworld_fleet/hosted/admin_link.h) against a real
# opennova-serve child on the LAN network type with its remote-admin port open, over a
# synthetic loose game directory and the hosted renders' admin.cfg: the login, PLAYER LIST,
# CHAT SEND read back by CHAT GET, the per-verb reply framing (two packets, one, the quiet
# gap), the status poll, the reconnect after the child is killed and started again on the
# same port, QUIT at the end, and the admin password in no log or console line. Compiles the
# hosted sources it needs directly, as hosted_supervisor_test does, so it runs on every
# build; ctest passes the opennova-serve binary. Serial: the child holds one TCP port across
# its restart.
add_executable(hosted_admin_link_test
    novaworld/hosted_admin_link_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/billing_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/entitlement.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/admin_link_status.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/agent_protocol.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/fleet_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/hosted_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/data_sets/data_set_files.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/data_sets/data_set_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/host_limits.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/hosted_settings.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/placement.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_fleet/hosted/admin_link.cpp)
target_link_libraries(hosted_admin_link_test
    PRIVATE opennova_novaworld_service opennova_app_common Threads::Threads)
target_include_directories(hosted_admin_link_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common
    ${CMAKE_SOURCE_DIR}/apps/novaworld_fleet)
target_compile_definitions(hosted_admin_link_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_dependencies(hosted_admin_link_test opennova_serve)
opennova_add_pg_test(hosted_admin_link COMMAND hosted_admin_link_test $<TARGET_FILE:opennova_serve>)
set_tests_properties(hosted_admin_link PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)

# The real ProcessCommandRunner (apps/common/command_runner.h) against the test binary
# itself (`--child <mode>`): both streams captured apart, the exit code, the timeout, a
# program that does not start, nothing left in the scratch directory, and the AWS CLI's
# environment (the AWS and system variables, never the service's own). Database-free.
add_executable(command_runner_test
    novaworld/command_runner_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/owner_file.cpp)
target_link_libraries(command_runner_test PRIVATE opennova_app_common Threads::Threads)
if(WIN32)
    target_link_libraries(command_runner_test PRIVATE advapi32)
endif()
target_include_directories(command_runner_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
add_test(NAME command_runner COMMAND command_runner_test $<TARGET_FILE:command_runner_test>)
set_tests_properties(command_runner PROPERTIES TIMEOUT 120)

# The fleet's IPv4 networks (apps/novaworld_common/hosted/net_cidr.h): the dotted quads and
# CIDRs read strictly, the peers admitted (IPv4-mapped IPv6 included), the private ranges
# the agent routes' networks must lie in, and the refusal log's throttle. Pure functions.
add_executable(hosted_net_cidr_test
    novaworld/hosted_net_cidr_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/health.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/net_cidr.cpp)
target_compile_features(hosted_net_cidr_test PRIVATE cxx_std_17)
target_include_directories(hosted_net_cidr_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
add_test(NAME hosted_net_cidr COMMAND hosted_net_cidr_test)

# The console's curated commands (apps/novaworld_web/hosted/console_commands.h, service
# policy): the owners' list and the admins' one wider read, every line composed from checked
# arguments (slots, indexes, mission names, cp1252 chat text) and none off the list, and each
# command's replies, in retail's strings, mapped to the routes' answers. Crow-free: compiles
# the hosted sources it needs directly, so it runs on every build.
add_executable(hosted_console_commands_test
    novaworld/hosted_console_commands_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_web/hosted/console_commands.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/host_limits.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/hosted_settings.cpp)
target_link_libraries(hosted_console_commands_test PRIVATE opennova_net)
target_include_directories(hosted_console_commands_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common
    ${CMAKE_SOURCE_DIR}/apps/novaworld_web)
add_test(NAME hosted_console_commands COMMAND hosted_console_commands_test)

# The billing seam below the sync worker and the routes (apps/novaworld_common/billing):
# the webhook signature (a vector computed with Python's hmac, several v1 signatures and
# secrets, the 300 s boundary either way, every malformed header) and the event envelope;
# form encoding; CurlStripeApi on a stub CommandRunner (the argv exactly {curl, -q, --config,
# <file>} with no key in it, the config file's lines, its 0600 mode, its deletion after a
# success and after a failure, the status split, curl's failures, the retry rules); the typed
# client's requests, its readers on basil-shaped JSON and its error kinds; the FakeStripeApi
# (novaworld/billing_fake_stripe.h) the later billing tests drive; and the billing settings'
# checks. Crow-free and database-free: compiles the billing sources and core/owner_file.cpp
# directly.
add_executable(billing_stripe_test
    novaworld/billing_stripe_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/billing_config.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/stripe_api.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/stripe_client.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_web/billing/stripe_signature.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/owner_file.cpp)
target_link_libraries(billing_stripe_test PRIVATE opennova_app_common opennova_base Threads::Threads)
target_include_directories(billing_stripe_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common
    ${CMAKE_SOURCE_DIR}/apps/novaworld_web)
add_test(NAME billing_stripe COMMAND billing_stripe_test)

# The hosting entitlement (apps/novaworld_common/billing/entitlement.h) and the billing
# tables under it (billing_repository.h): which subscription statuses grant, the period's
# 48 h net, comp grants against tiers, the own-machine add-on counting only beside a tier,
# past_due keeping servers but starting none, an inactive or service account getting
# nothing; and the tables' rows (the instance id minted once, the customers, the
# subscriptions' status_since, the events' dedupe, pending order, cutoff and prune) on a
# migrated database. Crow-free: compiles the billing sources directly.
add_executable(billing_entitlement_test
    novaworld/billing_entitlement_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/billing_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/entitlement.cpp)
target_link_libraries(billing_entitlement_test PRIVATE opennova_novaworld_service)
target_include_directories(billing_entitlement_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
target_compile_definitions(billing_entitlement_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(billing_entitlement COMMAND billing_entitlement_test)

# The sold capacity (apps/novaworld_common/hosted/sold_capacity.h) and the checkout
# reservations it counts (billing/billing_repository.h): what an entitlement holds, what the
# fleet sells, what one more subscription adds and whether it fits; every account's
# holdings with its live reservations on a migrated database; and two checkouts for the
# last place judged under the hosting-start lock, the second seeing the first's
# reservation. Crow-free: compiles the sources directly.
add_executable(hosted_sold_capacity_test
    novaworld/hosted_sold_capacity_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/billing_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/entitlement.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/placement.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/sold_capacity.cpp)
target_link_libraries(hosted_sold_capacity_test PRIVATE opennova_novaworld_service Threads::Threads)
target_include_directories(hosted_sold_capacity_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
target_compile_definitions(hosted_sold_capacity_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(hosted_sold_capacity COMMAND hosted_sold_capacity_test)

# The billing sync (apps/novaworld_common/billing/billing_sync.h, billing_jobs.h) against
# the in-memory Stripe (novaworld/billing_fake_stripe.h) on a migrated database, its jobs'
# work on a stepped clock (and the jobs themselves, their state in the database): a webhook event's customer synced into the entitlement, a redelivery recorded
# once, another instance's customer ignored and listed as an orphan, the tables wiped and
# rebuilt by a reconcile, the backoff of a customer Stripe could not be asked about, a
# deleted customer, a row Stripe no longer knows, duplicates, and the price check.
# Crow-free: compiles the billing sources directly.
add_executable(billing_sync_test
    novaworld/billing_sync_test.cpp
    ${OPENNOVA_JOBS_SOURCES}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/billing_config.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/billing_jobs.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/billing_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/billing_sync.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/entitlement.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/stripe_api.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/stripe_client.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/owner_file.cpp)
target_link_libraries(billing_sync_test PRIVATE opennova_novaworld_service opennova_app_common Threads::Threads)
if(WIN32)
    target_link_libraries(billing_sync_test PRIVATE advapi32)
endif()
target_include_directories(billing_sync_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
target_compile_definitions(billing_sync_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(billing_sync COMMAND billing_sync_test)

# Phase D — LobbySession dispatch tests
add_executable(lobby_session_test novaworld/lobby_session_test.cpp)
target_link_libraries(lobby_session_test PRIVATE opennova_novaworld_service opennova_net)
target_include_directories(lobby_session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME lobby_session COMMAND lobby_session_test)

# ADR 0010 Phase 1 — ClientSession handshake (hk echo + 0x83 verify) driven
# in-process against the real server-side parsers/builders + LobbySession.
add_executable(client_session_loopback_test novaworld/client_session_loopback_test.cpp)
target_link_libraries(client_session_loopback_test PRIVATE opennova_novaworld_service opennova_net)
target_include_directories(client_session_loopback_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME client_session_loopback COMMAND client_session_loopback_test)

# Host registration through the REAL NwUdpListener (apps/novaworld_lobby's core) over a
# loopback UDP socket (not the in-process LobbySession mock): drives ClientSession
# to Verified, sends a ClientHostRequest, asserts the host shows in snapshot_hosted
# (the /api/hosts + GSB source). No DB (snapshot reads in-memory lobby state).
# The engine NWU driver + host role (net/novaworld/nwu_lobby_session.h, nwu_host_role.h) over
# real loopback UDP against the real NwUdpListener and an in-test gate: the probe, the
# handshake, a duplicate gate reply ignored, the registration, a roster change, the 1860-tick
# refresh and the stop.
add_executable(nwu_host_session_e2e_test novaworld/nwu_host_session_e2e_test.cpp)
target_link_libraries(nwu_host_session_e2e_test PRIVATE
    opennova_novaworld_lobby_core opennova_novaworld_common opennova_novaworld_service)
target_include_directories(nwu_host_session_e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nwu_host_session_e2e COMMAND nwu_host_session_e2e_test)
set_tests_properties(nwu_host_session_e2e PROPERTIES RUN_SERIAL TRUE)

add_executable(nw_host_register_e2e_test novaworld/nw_host_register_e2e_test.cpp)
target_link_libraries(nw_host_register_e2e_test PRIVATE
    opennova_novaworld_lobby_core opennova_novaworld_common opennova_novaworld_service)
target_include_directories(nw_host_register_e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_host_register_e2e COMMAND nw_host_register_e2e_test)
# This is the only test that drives the complete listener lifecycle through
# real UDP sockets. Its one-second negative/retry receive windows are protocol
# assertions, not load-test budgets, so CPU-saturated parallel ctest runs can
# manufacture packet timeouts that never reproduce in isolation.
set_tests_properties(nw_host_register_e2e PROPERTIES RUN_SERIAL TRUE)

# The lobby's answer to a stock client's 0x44 resend request, through the real
# listener over loopback UDP (same listener-source recipe as above).
add_executable(nw_lobby_resend_e2e_test novaworld/nw_lobby_resend_e2e_test.cpp)
target_link_libraries(nw_lobby_resend_e2e_test PRIVATE
    opennova_novaworld_lobby_core opennova_novaworld_common opennova_novaworld_service)
target_include_directories(nw_lobby_resend_e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_lobby_resend_e2e COMMAND nw_lobby_resend_e2e_test)
set_tests_properties(nw_lobby_resend_e2e PROPERTIES RUN_SERIAL TRUE)

# The service's ServerCommand / ServerStopHosting pushes to a listed host, through
# the real listener over loopback UDP (same listener-source recipe as above) and a
# migrated database: each push a command the test inserts on its own connection
# and the listener's receive thread takes (lobby_repository.h). The session's
# lobby_sessions row from its Hello to its goodbye, the RID resolving once
# hosting, the pushes landing as the host session's notices, an expired command
# dropped unsent, a lost push coming back after the send-interval leg's gap and
# the client's 0x44, and the stop taking the row out of the browser. Its
# one-second recovery window is a protocol assertion, so it runs serially.
add_executable(nw_server_command_push_e2e_test novaworld/nw_server_command_push_e2e_test.cpp)
target_link_libraries(nw_server_command_push_e2e_test PRIVATE
    opennova_novaworld_lobby_core opennova_novaworld_common opennova_novaworld_service)
target_include_directories(nw_server_command_push_e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nw_server_command_push_e2e_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(nw_server_command_push_e2e COMMAND nw_server_command_push_e2e_test)
set_tests_properties(nw_server_command_push_e2e PROPERTIES RUN_SERIAL TRUE)

# Several lobby instances on one database sharing one lobby address (nw_udp_listener.h,
# gate_listener.h, lobby_repository.h): real NW UDP listeners, each an instance of its own at the
# one endpoint, and a real gate, over loopback UDP and a migrated, seeded database: the gate hands
# every client that address, a host on each gets a RID of its own and a push queued for it alone,
# forget_owner reaches both, one drained leaves the gate handing out the address while both
# listings stay and both drained leave it silent, a lobby's start reaps a stale instance with its
# rows and leaves the others', a clean stop deletes its instance, and a host whose lobby was
# replaced behind the address (its receive window run out by its clock) reconnects there and is
# listed on the new instance. Serial: its packet windows are protocol assertions. POSIX (it binds
# 127.0.0.x).
if(UNIX)
    add_executable(lobby_instances_e2e_test novaworld/lobby_instances_e2e_test.cpp)
    target_link_libraries(lobby_instances_e2e_test PRIVATE
        opennova_novaworld_gate_core opennova_novaworld_lobby_core opennova_novaworld_common
        opennova_novaworld_service)
    target_include_directories(lobby_instances_e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(lobby_instances_e2e_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(lobby_instances_e2e COMMAND lobby_instances_e2e_test)
    set_tests_properties(lobby_instances_e2e PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
endif()

# Host-row staleness backstop sweep (prune_stale_hosts).
add_executable(host_prune_test novaworld/host_prune_test.cpp)
target_link_libraries(host_prune_test PRIVATE opennova_novaworld_service)
target_include_directories(host_prune_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(host_prune_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(host_prune COMMAND host_prune_test)

# End-to-end host register -> read-back (the path /api/lobbies + /jop_2.gsb use).
add_executable(host_register_list_test novaworld/host_register_list_test.cpp)
target_link_libraries(host_register_list_test PRIVATE opennova_novaworld_service opennova_net)
target_include_directories(host_register_list_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(host_register_list_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(host_register_list COMMAND host_register_list_test)

# The listing's owner below the HTTP layer (host_keys): the hosting-key store's hash, cap,
# claims and sweep, and the lobby session linking a listing to the account its HostKey was
# minted for, through updates and a parked re-host, plus ONNET_REQUIRE_HOST_KEY's refusal. A
# migrated database per case; runs on every service build.
add_executable(host_key_test novaworld/host_key_test.cpp)
target_link_libraries(host_key_test PRIVATE opennova_novaworld_service opennova_net)
target_include_directories(host_key_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(host_key_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(host_key COMMAND host_key_test)

# The lobby's tables below the listener (net/novaworld/lobby_repository.h, migrations 0006, 0009
# and 0012): its instances (two at one endpoint) and the RID sequence, its session rows, the pushes, ends and forget_owner
# the routes and the supervisor queue for it (answered from the session rows as the lobby would,
# bearing the session's lobby_id, a forget_owner one per instance), each instance's take in
# insert order with the expiry, a session's delete taking its commands, and clear_unheld. A
# migrated database per case; runs on every service build.
add_executable(lobby_repository_test novaworld/lobby_repository_test.cpp)
target_link_libraries(lobby_repository_test PRIVATE opennova_novaworld_service)
target_include_directories(lobby_repository_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lobby_repository_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(lobby_repository COMMAND lobby_repository_test)

# The service's CD keys below the HTTP layer (apps/novaworld_web/cd_keys.h): the
# serial format, generate / list / assign / revoke, and the DFX2 activations
# (the account's, minting its first key or not, and an admin's manual one), each NWACT
# read back by the client's leg. Compiles the server's cd_keys.cpp directly, as
# hosted_supervisor_test does, so it runs on every build.
add_executable(cd_keys_test
    novaworld/cd_keys_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_web/cd_keys.cpp)
target_link_libraries(cd_keys_test PRIVATE opennova_novaworld_service)
target_include_directories(cd_keys_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common
    ${CMAKE_SOURCE_DIR}/apps/novaworld_web)
target_compile_definitions(cd_keys_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(cd_keys COMMAND cd_keys_test)

# The schema backend/migrations builds and the seeds: tables, columns, keys, indexes and
# the rules they hold, the applied list against the directory, the seeds' rows.
add_executable(backend_schema_test novaworld/backend_schema_test.cpp)
target_link_libraries(backend_schema_test PRIVATE opennova_novaworld_service)
target_include_directories(backend_schema_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(backend_schema_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(backend_schema COMMAND backend_schema_test)

# The service's HTTP route harness (hosted servers, PR A): web's Crow listener
# (opennova_novaworld_web_core) on a loopback port over a pooled, migrated database,
# driven with apps/common's http_exchange. Only with the HTTP layer
# (BUILD_NOVAWORLD_HTTP); the service link is direct for the libpq-containment lint.
if(BUILD_NOVAWORLD_HTTP)
    add_executable(opennova_novaworld_web_http_routes_test novaworld/http_routes_test.cpp)
    target_link_libraries(opennova_novaworld_web_http_routes_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_lobby_core opennova_novaworld_fleet_core
        opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(opennova_novaworld_web_http_routes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(opennova_novaworld_web_http_routes_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(opennova_novaworld_web_http_routes REDIS
        COMMAND opennova_novaworld_web_http_routes_test)

    # The static addresses (apps/novaworld_common/hosted/address_*.h): the name rules, the
    # file and Route 53 zones, the EC2 and docker address drivers' calls on stub runners;
    # then on a migrated database the names, the comp addresses, the entitlement's
    # addresses, a start naming one, and the address jobs run by hand over a recording
    # driver (allocate, associate after the previous holder's end, release after the
    # machine's end, the record's sync and TTL, the grant read again) and fleet.sweep's
    # orphans.
    add_executable(hosted_address_test novaworld/hosted_address_test.cpp)
    target_link_libraries(hosted_address_test PRIVATE
        opennova_novaworld_fleet_core opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(hosted_address_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(hosted_address_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(hosted_address COMMAND hosted_address_test)
    # The concurrent case gives each request up to 30 s; this bounds a hang
    # below ctest's 1500 s default.
    set_tests_properties(opennova_novaworld_web_http_routes PROPERTIES TIMEOUT 120)

    # The admin's background jobs routes (apps/novaworld_web/jobs/job_routes.h over
    # job_admin.h) on the in-process Crow listener over a migrated database and a Redis
    # namespace: every route refused without an admin, the overview's counts, latency,
    # minutes, running leases, workers and schedules, the list's filters and pages, a
    # job's detail, the retry, cancel, run-now and prune actions as compare-and-sets,
    # and never a job's secret in a reply.
    add_executable(job_routes_test novaworld/job_routes_test.cpp)
    target_link_libraries(job_routes_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(job_routes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(job_routes_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(job_routes REDIS COMMAND job_routes_test)
    set_tests_properties(job_routes PROPERTIES TIMEOUT 120)

    # The admin's infrastructure view's AWS reads (apps/novaworld_web/infra/), no
    # database: the Query APIs' XML reader, the curl-signed calls and their config
    # files on a stub CommandRunner (infra_aws_stub.h), the ECS agent's
    # credentials, AWS's errors in words, every section of a deployment mid-deploy
    # parsed, a failing section standing alone, the local stack calling nothing,
    # and the cache window with one read shared by callers at once.
    add_executable(infra_status_test novaworld/infra_status_test.cpp)
    target_link_libraries(infra_status_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(infra_status_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME infra_status COMMAND infra_status_test)

    # Its route (infra/infra_routes.h) on the in-process Crow listener over a
    # migrated database and a Redis namespace: refused without an admin; the local
    # stack's view (the cloud unavailable, the lobbies, the controller and the
    # machines from rows); a deployment's from the stub AWS, read once per cache
    # window; and no credential in any reply.
    add_executable(infra_routes_test novaworld/infra_routes_test.cpp)
    target_link_libraries(infra_routes_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(infra_routes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(infra_routes_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(infra_routes REDIS COMMAND infra_routes_test)
    set_tests_properties(infra_routes PROPERTIES TIMEOUT 120)

    # The logs the admin's pages read (apps/novaworld_web/infra/log_store.h), no database:
    # CloudWatch Logs behind a scripted CommandRunner (GetLogEvents' bodies and tokens for a
    # known stream, FilterLogEvents' windows walking back for a program's every task and a
    # filter, a millisecond kept whole at a page's cut, a dense window narrowed, the calls
    # bounded, the cache, AWS's words, no credential on argv), a directory's files paged
    # backwards and merged by time, the programs' stamps.
    add_executable(log_store_test novaworld/log_store_test.cpp)
    target_link_libraries(log_store_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(log_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME log_store COMMAND log_store_test)

    # Its route (infra/log_routes.h) on the in-process Crow listener over a migrated database
    # and a Redis namespace: refusals, a server's starts, a machine's hostd and a program's
    # tasks from a directory store, the stream a start's row names read from CloudWatch (a
    # stub), AWS's refusal a 502, no store a 503.
    add_executable(log_routes_test novaworld/log_routes_test.cpp)
    target_link_libraries(log_routes_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(log_routes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(log_routes_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(log_routes REDIS COMMAND log_routes_test)
    set_tests_properties(log_routes PROPERTIES TIMEOUT 120)

    # The AWS resources' metrics (apps/novaworld_web/infra/metrics_store.h), no database:
    # GetMetricData's form for the database, the cache's node and the balancer's groups over
    # 48 h and 400 days, the points in order, a NextToken, AWS's refusal, the 60 s cache, the
    # local stack calling nothing.
    add_executable(metrics_store_test novaworld/metrics_store_test.cpp)
    target_link_libraries(metrics_store_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(metrics_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME metrics_store COMMAND metrics_store_test)

    # The usage routes (infra/usage_routes.h) on the in-process Crow listener over a migrated
    # database and a Redis namespace: a server's, a machine's and a program's minutes and
    # hours (the counters' rates, none across a gap or a start-over; the tasks added up), an
    # AWS resource's metrics (none locally; a stub's), the programs from the samples and from
    # ECS (a stub).
    add_executable(usage_routes_test novaworld/usage_routes_test.cpp)
    target_link_libraries(usage_routes_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(usage_routes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(usage_routes_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(usage_routes REDIS COMMAND usage_routes_test)
    set_tests_properties(usage_routes PROPERTIES TIMEOUT 120)

    # The fleet's health on the admin's pages (apps/novaworld_web/hosted/fleet_routes.h,
    # infra/attention_routes.h) on in-process Crow listeners over a migrated database and a
    # Redis namespace, the release pinning its images: the machine JSON's use (the
    # network's rates between its two newest minutes), images, outdated and static address
    # in the fleet view, one machine's page (its moves, its trees with their manifests'
    # totals), a server's ticks in the admin's view alone; the attention list's findings
    # over the rows in their order with their links, no alarm read locally, a deployment's
    # firing alarms through CloudWatch behind a stub (DescribeAlarms' form, the 20 s
    # cache, AWS's refusal a warn finding, no credential).
    add_executable(fleet_health_routes_test novaworld/fleet_health_routes_test.cpp)
    target_link_libraries(fleet_health_routes_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(fleet_health_routes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(fleet_health_routes_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(fleet_health_routes REDIS COMMAND fleet_health_routes_test)
    set_tests_properties(fleet_health_routes PROPERTIES TIMEOUT 120)

    # The hosted data sets' admin routes (apps/novaworld_web/data_sets/, ADR 0064) on the
    # in-process Crow listener over a migrated database, a Redis namespace and a local
    # object store in a temp directory: every route refused without the admin, the list's
    # shapes over the seeded rows, a base and an expansion created (every shape and database
    # refusal), patched and deleted; a tree upload begun with the kind's files (a missing
    # and an unexpected name refused), each part PUT through the route (the ETag back; one
    # over the cap, out of range or of a name the kind lacks refused), each file completed
    # (a wrong ETag refused), the tree finalized with its tree.finalize job, ready and
    # current (its delete refused), an upload abandoned, a superseded tree pruned with a
    # tree.prune job, a pinned one refused.
    add_executable(data_set_routes_test novaworld/data_set_routes_test.cpp)
    target_link_libraries(data_set_routes_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(data_set_routes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(data_set_routes_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(data_set_routes REDIS COMMAND data_set_routes_test)

    # The update listener (apps/novaworld_web/updater/update_listener.h) over a migrated database
    # and a local object store: a packed update built from the synthetic trees
    # (opennova-update-tool's builder) as two data sets' published release, fetched and
    # unpacked into the listener's cache, the stock client's sequence on a loopback port (the
    # gateway's login and SERVER.LST, the update server's login, CRC and files), a login no
    # release serves refused, the release retired and the cache swept after a refresh, a
    # second release served.
    add_executable(update_listener_test novaworld/update_listener_test.cpp)
    target_link_libraries(update_listener_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_common opennova_novaworld_service
        opennova_update_tool_core)
    target_include_directories(update_listener_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(update_listener_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(update_listener COMMAND update_listener_test)
    set_tests_properties(update_listener PROPERTIES TIMEOUT 120)
    set_tests_properties(data_set_routes PROPERTIES TIMEOUT 120)

    # The outbound mail (apps/novaworld_common/accounts/mailer.h) and the address rule
    # (accounts/auth.h normalize_email): the shapes taken and refused, SesMailer's settings,
    # argv and SendEmail request on a stub CommandRunner (the request file
    # owner-only while the CLI runs and gone after, the CLI's failures), the
    # outbox's order, bound and stop, and the templates. Database-free.
    add_executable(mailer_test novaworld/mailer_test.cpp)
    target_link_libraries(mailer_test PRIVATE opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(mailer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME mailer COMMAND mailer_test)

    # The credential routes' brake: token buckets in Redis on a hand-stepped
    # clock (the burst, the refill, the Retry-After, the take/refund pair, a
    # bucket's key expiring when it would be full again, two replicas drawing
    # on one bucket exactly) and the trusted-proxy client address.
    add_executable(rate_limiter_test novaworld/rate_limiter_test.cpp)
    target_link_libraries(rate_limiter_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_common opennova_novaworld_service
        Threads::Threads)
    target_include_directories(rate_limiter_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    opennova_add_redis_test(rate_limiter COMMAND rate_limiter_test)

    # The web role's other shared state in Redis, as two replicas see it: the
    # client (its URL, errors, the reconnect after the server drops it, the
    # commands it sends again and the takes it never does), the retail login,
    # join and host sessions (written by one replica, read by the other, gone
    # after their TTL, a host key taken once however many replicas race for it)
    # and the retail login pins (each read moving a pin's expiry out, a
    # revocation on one replica holding on the other).
    add_executable(redis_state_test novaworld/redis_state_test.cpp)
    target_link_libraries(redis_state_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_common opennova_novaworld_service
        Threads::Threads)
    target_include_directories(redis_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    opennova_add_redis_test(redis_state COMMAND redis_state_test)

    # The listing's owner end to end (host_keys): two in-process services (the
    # HTTP, NW UDP and gate listeners over one pooled database each, one with
    # ONNET_REQUIRE_HOST_KEY) and opennova-serve's Lister with
    # and without an account, plus hand-driven host sessions for the NWU
    # reconnect and a second connection presenting a listed key. Its packet
    # windows are protocol assertions, so it runs serially.
    add_executable(nw_host_ownership_e2e_test novaworld/nw_host_ownership_e2e_test.cpp)
    target_link_libraries(nw_host_ownership_e2e_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_lobby_core opennova_novaworld_gate_core
        opennova_serve_core opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(nw_host_ownership_e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(nw_host_ownership_e2e_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(nw_host_ownership_e2e REDIS COMMAND nw_host_ownership_e2e_test)
    set_tests_properties(nw_host_ownership_e2e PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

    # The owner's pushes end to end: an in-process opennova-serve hosting with an account on an
    # in-process service (the same nw_service_harness.h), administered from the website's routes
    # (POST /api/login, GET /api/me/hosts, POST /api/me/hosts/<rid>/command and /stop): the
    # owner's SetServerName renames the match and saves game.cfg, its TextChatServer runs, another
    # account's push is a 404, and the stop ends the listing. Serial, like the other serve e2e.
    add_executable(nw_owner_admin_e2e_test novaworld/nw_owner_admin_e2e_test.cpp)
    target_link_libraries(nw_owner_admin_e2e_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_lobby_core opennova_novaworld_gate_core
        opennova_serve_core opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(nw_owner_admin_e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(nw_owner_admin_e2e_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(nw_owner_admin_e2e REDIS COMMAND nw_owner_admin_e2e_test)
    set_tests_properties(nw_owner_admin_e2e PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

    # Two hosted servers end to end on one machine's slots (apps/novaworld_common/hosted): an
    # in-process service with hosted servers on (the same nw_service_harness.h), its
    # supervisor over machines that are the real agent, opennova-hostd with its process
    # runtime (as the local stack's machine containers run it), against the internal
    # listener on loopback, running real opennova-serve children over synthetic loose game
    # directories: started from POST /api/me/servers, both placed on one shared machine
    # (slots 0 and 1, each listed at its slot's game port), listed as their service accounts
    # and shown as the owners', renamed by the owner's SetServerName (game.cfg saved in the
    # slot), and stopped from POST /api/me/servers/<id>/stop. ctest passes the opennova-serve
    # and opennova-hostd binaries.
    add_executable(hosted_local_e2e_test novaworld/hosted_local_e2e_test.cpp)
    target_link_libraries(hosted_local_e2e_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_lobby_core opennova_novaworld_gate_core
        opennova_novaworld_fleet_core opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(hosted_local_e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(hosted_local_e2e_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_dependencies(hosted_local_e2e_test opennova_serve opennova_hostd)
    opennova_add_pg_test(hosted_local_e2e REDIS COMMAND hosted_local_e2e_test $<TARGET_FILE:opennova_serve>
        $<TARGET_FILE:opennova_hostd>)
    set_tests_properties(hosted_local_e2e PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)

    # The machine agents' routes (apps/novaworld_fleet/hosted/agent_route.h) on the internal
    # listener (hosted/internal_listener.h) on a loopback port over a migrated database, driven
    # with apps/common's http_exchange against a fake FleetDesiredState: the bootstrap's one
    # redeem and its refusals, the sync's report, answer and refusals, another protocol
    # draining the machine, the one bare 404 for everything else, the networks admitted; then
    # the real agent end to end against the routes: opennova-hostd with its process runtime
    # (as the local stack's machine containers run it), this test binary as its
    # opennova-serve (`--resource-dir ...`). Linux: the agent is.
    add_executable(hosted_agent_route_test novaworld/hosted_agent_route_test.cpp)
    target_link_libraries(hosted_agent_route_test PRIVATE
        opennova_novaworld_fleet_core opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(hosted_agent_route_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(hosted_agent_route_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_dependencies(hosted_agent_route_test opennova_hostd)
    opennova_add_pg_test(hosted_agent_route
        COMMAND hosted_agent_route_test $<TARGET_FILE:hosted_agent_route_test>
            $<TARGET_FILE:opennova_hostd>)
    set_tests_properties(hosted_agent_route PROPERTIES TIMEOUT 180)

    # The support tiers end to end against Stripe's TEST mode: an in-process service (the
    # same nw_service_harness.h) with billing over the real curl transport; a Checkout
    # session made through POST /api/billing/checkout and read back from Stripe, a second
    # one expiring the first, the subscription Checkout would make, Stripe's webhook signed
    # as Stripe signs it, and the entitlement GET /api/me/billing shows. Needs
    # ONNET_STRIPE_TEST_SECRET_KEY (a test-mode key; locally from the vault through
    # deploy/env/billing.e2e.env.tpl): without it, a skip the core suite accepts. A pg test
    # besides; labelled `stripe`.
    add_executable(billing_stripe_e2e_test novaworld/billing_stripe_e2e_test.cpp)
    target_link_libraries(billing_stripe_e2e_test PRIVATE
        opennova_novaworld_web_core opennova_novaworld_lobby_core opennova_novaworld_gate_core
        opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(billing_stripe_e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(billing_stripe_e2e_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(billing_stripe_e2e REDIS COMMAND billing_stripe_e2e_test)
    set_property(TEST billing_stripe_e2e APPEND PROPERTY LABELS stripe)
    set_tests_properties(billing_stripe_e2e PROPERTIES TIMEOUT 300)
endif()

# The six programs' binaries run whole, as child processes
# (novaworld/program_process.h), on a database and a Redis namespace of the test's
# own; ctest passes each binary (gate, lobby, web, fleet, worker, data, in that
# order). POSIX only (fork, kill); each TIMEOUT bounds a run should a child hang, and
# the children die with it. With the HTTP layer, which web, the fleet, the worker and
# the data program are built with.
#
#   programs         each program boots alone with its own parts: its boot line, the
#                    ports it binds (its health listener's GET /livez among them),
#                    no other program's part; the gate's reply and the lobby's
#                    SessionInit advertise the configured endpoints, never a bound
#                    port; a gate or a lobby missing its endpoints, or web its own,
#                    does not boot; two lobbies at one lobby address, each its own
#                    instance, a gate handing out that address, the first one's stop
#                    at once while the second serves on.
#   signal_shutdown  SIGINT and SIGTERM each run a program's orderly stop (exit 0, its
#                    listeners in order, the database closed): a lobby's, and a web
#                    process's past Crow and a held-open request; the second signal;
#                    and web's boot stopped by a Redis it cannot reach.
#   nw_programs_e2e  the six end to end on one database and one Redis: a
#                    retail-protocol host lists through the gate and the lobby and a
#                    client sits in its lobby, an admin's push through web reaches
#                    the host; then web, the worker, the data program and the
#                    gate stopped and the fleet killed, all five started again, the
#                    listing and the lobby sessions survive and a push still lands;
#                    then the fleet's database frozen (a proxy that stops
#                    forwarding): its watchdog exits it alone (70), the others serve
#                    on; then the lobby restarted, which alone drops its own
#                    sessions and listings. Serial: its packet windows and its
#                    stall's 30 s.
if(UNIX AND BUILD_NOVAWORLD_HTTP)
    set(OPENNOVA_NOVAWORLD_PROGRAMS
        opennova_novaworld_gate opennova_novaworld_lobby opennova_novaworld_web
        opennova_novaworld_fleet opennova_novaworld_worker opennova_novaworld_data)
    set(OPENNOVA_NOVAWORLD_PROGRAM_FILES)
    foreach(program ${OPENNOVA_NOVAWORLD_PROGRAMS})
        list(APPEND OPENNOVA_NOVAWORLD_PROGRAM_FILES $<TARGET_FILE:${program}>)
    endforeach()
    foreach(program_test programs signal_shutdown)
        set(test_target opennova_novaworld_${program_test}_test)
        add_executable(${test_target} novaworld/${program_test}_test.cpp)
        # The service for the test's own database (common/test_database.h), the
        # shared library for the Redis test helper's header (common/test_redis.h:
        # web's ONNET_REDIS_URL).
        target_link_libraries(${test_target} PRIVATE
            opennova_novaworld_common opennova_novaworld_service)
        target_include_directories(${test_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(${test_target} PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        add_dependencies(${test_target} ${OPENNOVA_NOVAWORLD_PROGRAMS})
        opennova_add_pg_test(opennova_novaworld_${program_test} REDIS
            COMMAND ${test_target} ${OPENNOVA_NOVAWORLD_PROGRAM_FILES})
        set_tests_properties(opennova_novaworld_${program_test} PROPERTIES TIMEOUT 180)
    endforeach()

    # The local stack's smoke test (novaworld/stack_smoke.cpp): no ctest, it runs against a
    # running stack (scripts/dev_stack.sh smoke, from the `smoke` image of
    # apps/novaworld_common/Dockerfile), and writes the synthetic data set the stack's
    # machines serve without licensed data. Built here so every service build compiles it.
    add_executable(opennova_novaworld_stack_smoke novaworld/stack_smoke.cpp)
    target_link_libraries(opennova_novaworld_stack_smoke PRIVATE
        opennova_app_common opennova_net opennova_formats Threads::Threads)
    target_include_directories(opennova_novaworld_stack_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
    set_target_properties(opennova_novaworld_stack_smoke PROPERTIES
        OUTPUT_NAME "opennova-novaworld-stack-smoke")

    add_executable(nw_programs_e2e_test novaworld/nw_programs_e2e_test.cpp)
    target_link_libraries(nw_programs_e2e_test PRIVATE
        opennova_novaworld_common opennova_novaworld_service)
    target_include_directories(nw_programs_e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(nw_programs_e2e_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_dependencies(nw_programs_e2e_test ${OPENNOVA_NOVAWORLD_PROGRAMS})
    opennova_add_pg_test(nw_programs_e2e REDIS COMMAND nw_programs_e2e_test
        ${OPENNOVA_NOVAWORLD_PROGRAM_FILES})
    set_tests_properties(nw_programs_e2e PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)
endif()

# GET /livez on ONNET_HEALTH_PORT (apps/novaworld_common/core/health_listener.h): the
# replies to each request line (the path, the method, HEAD's empty body, a request
# that does not read) from a healthy watchdog and a stalled one on a hand-stepped
# clock, then the listener itself over loopback TCP on a port the OS picks: 200 "ok",
# then 503 "stalled" once a watched loop stood still, a slow probe bounded, the port
# let go at stop(). Database-free.
add_executable(health_listener_test novaworld/health_listener_test.cpp)
target_link_libraries(health_listener_test PRIVATE
    opennova_novaworld_common opennova_novaworld_service Threads::Threads)
target_include_directories(health_listener_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME health_listener COMMAND health_listener_test)

# Where a hosted server's start stands in its owner's words, and why its last run ended
# (apps/novaworld_common/hosted/server_phase.h), over hand-made rows: each state's phase, a
# provisioning server's wait by its last run's end or the fleet, a booting one's by its
# machine and its container's report (the data's bytes so far of their total), the last
# run's end for an ended server alone. Database-free.
add_executable(hosted_server_phase_test novaworld/hosted_server_phase_test.cpp)
target_link_libraries(hosted_server_phase_test PRIVATE
    opennova_novaworld_common opennova_novaworld_service)
target_include_directories(hosted_server_phase_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME hosted_server_phase COMMAND hosted_server_phase_test)

# The fleet's health (apps/novaworld_common/hosted/fleet_health.h) over hand-made rows:
# each rule just under and just over its bound (stalled starts, stuck and flapping
# machines, the brake's held machines, a protocol mismatch, outdated agents and images,
# machine pressure, late ticks, held launches, no lobby, failed servers and addresses,
# orphans, oversold), the level, the admin page each links and its rule's name, stale rows
# a warn "unknown", a machine's outdated by each image pair, the order and the heartbeat's
# gauges. Database-free.
add_executable(hosted_fleet_health_test novaworld/hosted_fleet_health_test.cpp)
target_link_libraries(hosted_fleet_health_test PRIVATE
    opennova_novaworld_common opennova_novaworld_service)
target_include_directories(hosted_fleet_health_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME hosted_fleet_health COMMAND hosted_fleet_health_test)

# What an ECS task finds from its own metadata (apps/novaworld_common/core/ecs_metadata.h):
# the fleet's bind (the task's private IPv4) against a fake metadata endpoint (an
# HTTP server in the test on loopback), the environment's bind standing where it is
# set or there is no metadata; and every refusal: an endpoint that does not answer or
# answers an error, no JSON or no address. Database-free.
add_executable(ecs_metadata_test novaworld/ecs_metadata_test.cpp)
target_link_libraries(ecs_metadata_test PRIVATE
    opennova_novaworld_common opennova_novaworld_service Threads::Threads)
target_include_directories(ecs_metadata_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ecs_metadata COMMAND ecs_metadata_test)

# Unknown-message tracker: record/dedup, sample cap, flush/upsert against the
# migrated unknown_messages table (OPENNOVA_SOURCE_DIR for the SQL on disk), snapshot,
# the bounded signatures and keys, and the age prune.
add_executable(unknown_tracker_test novaworld/unknown_tracker_test.cpp)
target_link_libraries(unknown_tracker_test PRIVATE opennova_novaworld_service)
target_include_directories(unknown_tracker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(unknown_tracker_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(unknown_tracker COMMAND unknown_tracker_test)

# opennova-serve listed on NovaWorld (ADR 0051 d6) against the real NwUdpListener and an in-test
# gate over loopback: it hosts on the game socket before the boot, lists the Serve Only columns
# with no slot 0, the advertised endpoint is the game socket a joiner plays on, a NovaWorld
# rename / message change is saved to game.cfg and survives the map change, and the stop
# deregisters.
add_executable(opennova_serve_novaworld_test serve/opennova_serve_novaworld_test.cpp)
target_link_libraries(opennova_serve_novaworld_test PRIVATE
    opennova_serve_core opennova_novaworld_lobby_core opennova_novaworld_common
    opennova_novaworld_service)
target_include_directories(opennova_serve_novaworld_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME opennova_serve_novaworld COMMAND opennova_serve_novaworld_test)
set_tests_properties(opennova_serve_novaworld PROPERTIES RUN_SERIAL TRUE)
# The service's pushes to a listed opennova-serve over its NovaWorld session (the same setup,
# over a migrated database: each push a command for the listener, lobby_repository.h): a pushed
# SetServerName renames the match and saves game.cfg, a pushed Cycle changes the map, a pushed
# ServerStopHosting ends the listing, an unknown RID is refused.
add_executable(opennova_serve_server_command_push_test serve/opennova_serve_server_command_push_test.cpp)
target_link_libraries(opennova_serve_server_command_push_test PRIVATE
    opennova_serve_core opennova_novaworld_lobby_core opennova_novaworld_common
    opennova_novaworld_service)
target_include_directories(opennova_serve_server_command_push_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(opennova_serve_server_command_push_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(opennova_serve_server_command_push COMMAND opennova_serve_server_command_push_test)
set_tests_properties(opennova_serve_server_command_push PROPERTIES RUN_SERIAL TRUE)

# opennova-serve's Lister end to end over real loopback UDP against the real NwUdpListener
# and an in-test gate (the host result publishing nothing, the mission start's update, a
# roster edit keeping the other slot, the refresh, the stop).
add_executable(opennova_serve_lister_e2e_test serve/opennova_serve_lister_e2e_test.cpp)
target_link_libraries(opennova_serve_lister_e2e_test PRIVATE
    opennova_serve_core opennova_novaworld_lobby_core opennova_novaworld_common
    opennova_novaworld_service)
add_test(NAME opennova_serve_lister_e2e COMMAND opennova_serve_lister_e2e_test)
set_tests_properties(opennova_serve_lister_e2e PROPERTIES RUN_SERIAL TRUE)

# Where a hosted server runs (apps/novaworld_common/hosted/placement.h) and what a slot
# of a machine is (hosted/slots.h): best fit on the fullest shared machine that takes
# servers, the ties, the launches below the fleet's maximums and the waits at them, the
# own-machine add-on's machine, the states that count, the columns' words. Pure
# functions: compiles the source directly, so it runs on every build.
add_executable(hosted_placement_test
    novaworld/hosted_placement_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/placement.cpp)
target_compile_features(hosted_placement_test PRIVATE cxx_std_17)
target_include_directories(hosted_placement_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
add_test(NAME hosted_placement COMMAND hosted_placement_test)

# The machine agent's protocol (apps/novaworld_common/hosted/agent_protocol.h), the one
# the service and opennova-hostd both build: every message's round trip through its
# encoder and strict decoder, the wire's member names, the protocol check, every field's
# shape and range, the routes' paths. Pure functions: compiles the source directly, so it
# runs on every build.
add_executable(hosted_agent_protocol_test
    novaworld/hosted_agent_protocol_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/agent_protocol.cpp)
target_link_libraries(hosted_agent_protocol_test PRIVATE opennova_io)
target_compile_features(hosted_agent_protocol_test PRIVATE cxx_std_17)
target_include_directories(hosted_agent_protocol_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
add_test(NAME hosted_agent_protocol COMMAND hosted_agent_protocol_test)

# The fleet in the database (apps/novaworld_common/hosted/fleet_repository.h, migration
# 0003_fleet.sql): a machine's row and its one-time token (its SHA-256 alone), every lifecycle
# move a compare-and-set, the bootstrap's redeem, the agent's secret, a sync's report and its
# slots' rows, the silent machines, and the slots the live servers hold (assigned once, the
# generation moving on, let go, read back for placement). Compiles the hosted sources it needs
# directly, so it runs on every service build.
# The hosted data sets' rows (data_sets/data_set_repository.h, migration 0011, ADR 0064): the
# seeded data sets, trees, the servers' pins, and the migration's mapping of the old columns.
add_executable(data_set_repository_test
    novaworld/data_set_repository_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/data_sets/data_set_files.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/data_sets/data_set_repository.cpp)
target_link_libraries(data_set_repository_test PRIVATE opennova_novaworld_service Threads::Threads)
target_include_directories(data_set_repository_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
target_compile_definitions(data_set_repository_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(data_set_repository COMMAND data_set_repository_test)
set_tests_properties(data_set_repository PROPERTIES TIMEOUT 300)

# The data sets' jobs (data_sets/data_set_jobs.h, ADR 0064) on a migrated database and a local
# object store in a scratch directory, each run as the workers run it: a packed synthetic
# tree finalized (its manifest, missions, version; the refusals; an expansion's pair) and
# the prune. Compiles the jobs, the data set sources and the local store directly; links
# the runtime for the mission catalog.
add_executable(data_set_jobs_test
    novaworld/data_set_jobs_test.cpp
    ${OPENNOVA_JOBS_SOURCES}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/data_sets/data_set_files.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/data_sets/data_set_jobs.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/data_sets/data_set_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/object_store.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/local_object_store.cpp)
target_link_libraries(data_set_jobs_test
    PRIVATE opennova_novaworld_service opennova_runtime opennova_update_tool_core Threads::Threads)
target_include_directories(data_set_jobs_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
target_compile_definitions(data_set_jobs_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(data_set_jobs COMMAND data_set_jobs_test)
set_tests_properties(data_set_jobs PROPERTIES TIMEOUT 300)

add_executable(hosted_fleet_repository_test
    novaworld/hosted_fleet_repository_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/billing_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/billing/entitlement.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/agent_protocol.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/data_sets/data_set_files.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/data_sets/data_set_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/fleet_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/host_limits.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/hosted_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/hosted_settings.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/placement.cpp)
target_link_libraries(hosted_fleet_repository_test
    PRIVATE opennova_novaworld_service opennova_app_common Threads::Threads)
target_include_directories(hosted_fleet_repository_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
target_compile_definitions(hosted_fleet_repository_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(hosted_fleet_repository COMMAND hosted_fleet_repository_test)

# The fleet's machine drivers and settings: the EC2 driver (apps/novaworld_common/hosted/
# ec2_machine_driver.h) on a stub CommandRunner that checks every AWS CLI argv and answers it
# (each kind's launch template, the tags, the client token, the user-data file with the
# machine's values filled in and deleted after, the subnet of two a launch takes and a zone
# without capacity handing it to the other, describe's states from one cached listing, a
# failure reading Unknown and its back-off, terminate's idempotence, list_tagged's scope and its
# throw, the slots' ports) and its config checks; the shipped
# machine script (hosted/machine/machine_user_data.sh) and its render; the settings
# (hosted/fleet_config.h); and the docker driver, the local stack's
# (hosted/docker_machine_driver.h), against an engine in memory. Crow-free and database-free:
# compiles the hosted sources it needs directly.
add_executable(hosted_ec2_machine_driver_test
    novaworld/hosted_ec2_machine_driver_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/health.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/agent_protocol.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/docker_machine_driver.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/ec2_machine_driver.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/fleet_config.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/host_limits.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/hosted_settings.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/net_cidr.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/owner_file.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/hosted/placement.cpp)
target_link_libraries(hosted_ec2_machine_driver_test PRIVATE opennova_app_common Threads::Threads)
if(WIN32)
    target_link_libraries(hosted_ec2_machine_driver_test PRIVATE advapi32)
endif()
target_include_directories(hosted_ec2_machine_driver_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
target_compile_definitions(hosted_ec2_machine_driver_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME hosted_ec2_machine_driver COMMAND hosted_ec2_machine_driver_test)
set_tests_properties(hosted_ec2_machine_driver PROPERTIES TIMEOUT 120)

# The object store seam (apps/novaworld_common/core/object_store.h, ADR 0064): the URL forms, the
# local store over a directory (its multipart uploads part by part), and the S3 store's argv
# through a recording runner (a part's bytes handed to `upload-part` in a scratch file).
# Crow-free and database-free.
add_executable(object_store_test
    novaworld/object_store_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/object_store.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/s3_object_store.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/local_object_store.cpp)
target_link_libraries(object_store_test PRIVATE opennova_app_common Threads::Threads)
if(WIN32)
    target_link_libraries(object_store_test PRIVATE advapi32)
endif()
target_include_directories(object_store_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
add_test(NAME object_store COMMAND object_store_test)

# What a Linux machine or container uses (apps/novaworld_common/core/proc_stats.h: hostd's machine
# stats, each program's own samples), read from fixture trees: /proc/stat's busy and total and
# a percent between two readings, /proc/meminfo, /proc/net/dev less loopback, a cgroup v2 and a
# v1 tree (CPU time, memory less the inactive file cache, the limit or none), statvfs.
# Crow-free and database-free.
add_executable(proc_stats_test
    novaworld/proc_stats_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common/core/proc_stats.cpp)
target_link_libraries(proc_stats_test PRIVATE opennova_base)
target_include_directories(proc_stats_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_common)
add_test(NAME proc_stats COMMAND proc_stats_test)

# The `service` label and the aggregate target over everything this file added (the head of
# this file says why).
get_directory_property(OPENNOVA_SERVICE_TESTS TESTS)
list(REMOVE_ITEM OPENNOVA_SERVICE_TESTS ${OPENNOVA_SERVICE_TESTS_BEFORE})
set_property(TEST ${OPENNOVA_SERVICE_TESTS} APPEND PROPERTY LABELS service)
get_directory_property(OPENNOVA_SERVICE_TARGETS BUILDSYSTEM_TARGETS)
list(REMOVE_ITEM OPENNOVA_SERVICE_TARGETS ${OPENNOVA_SERVICE_TARGETS_BEFORE})
add_custom_target(opennova_service_tests)
add_dependencies(opennova_service_tests ${OPENNOVA_SERVICE_TARGETS})
