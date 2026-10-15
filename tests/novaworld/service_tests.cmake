# The NovaWorld service's tests: the service target (opennova_novaworld_service), the
# server app's sources (apps/novaworld_server) and opennova-serve against the service's
# listeners. tests/CMakeLists.txt includes this file only when the service builds
# (OPENNOVA_NOVAWORLD_SERVICE, off by default; the Linux CI jobs and the server image turn
# it on), so its paths are the including file's: ${CMAKE_CURRENT_SOURCE_DIR} is tests/.

# The database tests run against a real PostgreSQL server: OPENNOVA_TEST_PG names
# it (a libpq URI whose role may create and drop databases), and each test case
# gets a database of its own there (tests/common/test_database.h). ctest hands
# the URI to the test as `--pg <uri>`; with none set the test prints "SKIP: needs
# a PostgreSQL server" and exits 77, which ctest reports Skipped and the core
# suite fails (scripts/ci/test_suites.py). scripts/dev_postgres.sh runs a local
# one and prints the value; CI runs postgres:17 as a job service.
set(OPENNOVA_TEST_PG "" CACHE STRING
    "PostgreSQL server URI the NovaWorld service's database tests create their databases on")
function(opennova_add_pg_test name)
    cmake_parse_arguments(TEST "" "" "COMMAND" ${ARGN})
    if(OPENNOVA_TEST_PG)
        add_test(NAME ${name} COMMAND ${TEST_COMMAND} --pg ${OPENNOVA_TEST_PG})
    else()
        add_test(NAME ${name} COMMAND ${TEST_COMMAND})
    endif()
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77 LABELS pg)
endfunction()

# The real GateListener on port 0 over loopback UDP: the port start() reports is
# the one that answers a probe and the one the reply advertises as POSTIPPORT,
# a second listener on that port fails start(), and each gate tag's reply
# carries its games row's lobby (the repo's migrations and games seed; JO's
# reply byte for byte), an unknown tag none (same listener-source recipe as the
# NW UDP e2e tests below).
add_executable(gate_listener_test
    novaworld/gate_listener_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/catalog_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/gate_listener.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/server_config.cpp)
target_link_libraries(gate_listener_test PRIVATE opennova_novaworld_service opennova_app_common)
target_include_directories(gate_listener_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
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
# connection. Compiles the server's auth.cpp (create_user) directly: it joins
# opennova_novaworld_server_core only under BUILD_NOVAWORLD_HTTP, and this test
# runs on every service build.
add_executable(db_concurrency_test
    novaworld/db_concurrency_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/auth.cpp)
target_link_libraries(db_concurrency_test
    PRIVATE opennova_novaworld_service opennova_bcrypt Threads::Threads)
target_include_directories(db_concurrency_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
target_compile_definitions(db_concurrency_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(db_concurrency COMMAND db_concurrency_test)

# The NovaWorld site's login sessions and roles (web_sessions, players.role):
# the token kept as its SHA-256, the per-account cap, the sliding expiry and
# its race (threads on pooled connections), the refusals, the revocations, the
# prune and the one-shot bootstrap promotion, on a migrated database. Compiles
# the server's web_session.cpp and auth.cpp directly, as db_concurrency_test
# does, so it runs on every service build, not only under BUILD_NOVAWORLD_HTTP.
add_executable(web_session_test
    novaworld/web_session_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/auth.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/web_session.cpp)
target_link_libraries(web_session_test
    PRIVATE opennova_novaworld_service opennova_bcrypt Threads::Threads)
target_include_directories(web_session_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
target_compile_definitions(web_session_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(web_session COMMAND web_session_test)

# The hosted servers' supervisor (apps/novaworld_server/hosted):
# the settings and their renders, the service accounts, the start request's
# checks, the whole lifecycle on a FakeProvider and a hand-stepped clock (the
# stops, the crash restarts and their limit, the timeouts, the health checks,
# the owners' changes, the reconciliation, the admin links following the running
# servers), and LocalProcessProvider against the test binary itself as its
# opennova-serve. Compiles the server's auth.cpp, web_session.cpp and the hosted
# sources directly, as web_session_test does, so it runs on every build, not
# only under BUILD_NOVAWORLD_HTTP.
add_executable(hosted_supervisor_test
    novaworld/hosted_supervisor_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/auth.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/web_session.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/admin_link.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/hosted_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/data_sets.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/hosted_settings.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/local_process_provider.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/owner_file.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/supervisor.cpp)
target_link_libraries(hosted_supervisor_test
    PRIVATE opennova_novaworld_service opennova_bcrypt opennova_app_common Threads::Threads)
if(WIN32)
    target_link_libraries(hosted_supervisor_test PRIVATE advapi32)
endif()
target_include_directories(hosted_supervisor_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
target_compile_definitions(hosted_supervisor_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(hosted_supervisor
    COMMAND hosted_supervisor_test $<TARGET_FILE:hosted_supervisor_test>)
set_tests_properties(hosted_supervisor PROPERTIES TIMEOUT 300)

# The service's admin link (apps/novaworld_server/hosted/admin_link.h) against a real
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
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/admin_link.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/hosted_repository.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/data_sets.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/hosted_settings.cpp)
target_link_libraries(hosted_admin_link_test
    PRIVATE opennova_novaworld_service opennova_app_common Threads::Threads)
target_include_directories(hosted_admin_link_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
target_compile_definitions(hosted_admin_link_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_dependencies(hosted_admin_link_test opennova_serve)
opennova_add_pg_test(hosted_admin_link COMMAND hosted_admin_link_test $<TARGET_FILE:opennova_serve>)
set_tests_properties(hosted_admin_link PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)

# The cloud MachineProvider (apps/novaworld_server/hosted/ec2_provider.h) on a stub
# CommandRunner that checks every AWS CLI argv and answers it: the launch's run-instances
# (the launch template, the subnet, the tags, the client token, the user-data file with the
# bootstrap URL and token filled in, deleted after), describe's state mapping from one
# cached describe-instances (the bundle fetch gating Running, the launch grace, the cache's
# lifetime, a failure reading Pending), terminate's idempotence, list_tagged's scope and its
# throw, and every CLI failure, timeout and malformed reply; the bootstrap tokens
# (hosted/bootstrap.h: issue, bind, one redeem from the bound address, the lapse, the CIDR
# reads) and the bundle's JSON; the machine script's render; and the real
# ProcessCommandRunner (apps/common/command_runner.h) against the test binary itself
# (`--child <mode>`). Crow-free: compiles the hosted sources it needs directly, so it runs on
# every build.
add_executable(hosted_ec2_provider_test
    novaworld/hosted_ec2_provider_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/bootstrap.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/ec2_provider.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/data_sets.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/hosted_settings.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/owner_file.cpp)
target_link_libraries(hosted_ec2_provider_test PRIVATE opennova_app_common Threads::Threads)
if(WIN32)
    target_link_libraries(hosted_ec2_provider_test PRIVATE advapi32)
endif()
target_include_directories(hosted_ec2_provider_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
target_compile_definitions(hosted_ec2_provider_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME hosted_ec2_provider
    COMMAND hosted_ec2_provider_test $<TARGET_FILE:hosted_ec2_provider_test>)
set_tests_properties(hosted_ec2_provider PROPERTIES TIMEOUT 120)

# The console's curated commands (apps/novaworld_server/hosted/console_commands.h, service
# policy): the owners' list and the admins' one wider read, every line composed from checked
# arguments (slots, indexes, mission names, cp1252 chat text) and none off the list, and each
# command's replies, in retail's strings, mapped to the routes' answers. Crow-free: compiles
# the hosted sources it needs directly, so it runs on every build.
add_executable(hosted_console_commands_test
    novaworld/hosted_console_commands_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/console_commands.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/data_sets.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/hosted_settings.cpp)
target_link_libraries(hosted_console_commands_test PRIVATE opennova_net)
target_include_directories(hosted_console_commands_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
add_test(NAME hosted_console_commands COMMAND hosted_console_commands_test)

# The billing seam below the sync worker and the routes (apps/novaworld_server/billing):
# the webhook signature (a vector computed with Python's hmac, several v1 signatures and
# secrets, the 300 s boundary either way, every malformed header) and the event envelope;
# form encoding; CurlStripeApi on a stub CommandRunner (the argv exactly {curl, -q, --config,
# <file>} with no key in it, the config file's lines, its 0600 mode, its deletion after a
# success and after a failure, the status split, curl's failures, the retry rules); the typed
# client's requests, its readers on basil-shaped JSON and its error kinds; the FakeStripeApi
# (novaworld/billing_fake_stripe.h) the later billing tests drive; and the billing settings'
# checks. Crow-free and database-free: compiles the billing sources and hosted/owner_file.cpp
# directly.
add_executable(billing_stripe_test
    novaworld/billing_stripe_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/billing/billing_config.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/billing/stripe_api.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/billing/stripe_client.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/billing/stripe_signature.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/owner_file.cpp)
target_link_libraries(billing_stripe_test PRIVATE opennova_app_common opennova_base Threads::Threads)
target_include_directories(billing_stripe_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
add_test(NAME billing_stripe COMMAND billing_stripe_test)

# The credential routes' brake: token buckets on a hand-stepped clock and the
# trusted-proxy client address. The server's rate_limiter.cpp compiled in
# directly, so it runs on every build.
add_executable(rate_limiter_test
    novaworld/rate_limiter_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/rate_limiter.cpp)
target_link_libraries(rate_limiter_test PRIVATE opennova_io)
target_compile_features(rate_limiter_test PRIVATE cxx_std_17)
target_include_directories(rate_limiter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
add_test(NAME rate_limiter COMMAND rate_limiter_test)

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

# Host registration through the REAL apps/novaworld_server NwUdpListener over a
# loopback UDP socket (not the in-process LobbySession mock): drives ClientSession
# to Verified, sends a ClientHostRequest, asserts the host shows in snapshot_hosted
# (the /api/hosts + GSB source). Compiles the listener sources directly — they live
# in the server exe, not a lib. No DB (snapshot reads in-memory lobby state).
# The engine NWU driver + host role (net/novaworld/nwu_lobby_session.h, nwu_host_role.h) over
# real loopback UDP against the real NwUdpListener and an in-test gate: the probe, the
# handshake, a duplicate gate reply ignored, the registration, a roster change, the 1860-tick
# refresh and the stop.
add_executable(nwu_host_session_e2e_test
    novaworld/nwu_host_session_e2e_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/nw_udp_listener.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/server_config.cpp)
target_link_libraries(nwu_host_session_e2e_test
    PRIVATE opennova_novaworld_service opennova_runtime opennova_app_common)
target_include_directories(nwu_host_session_e2e_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server
    ${CMAKE_SOURCE_DIR}/apps/common)
add_test(NAME nwu_host_session_e2e COMMAND nwu_host_session_e2e_test)
set_tests_properties(nwu_host_session_e2e PROPERTIES RUN_SERIAL TRUE)

add_executable(nw_host_register_e2e_test
    novaworld/nw_host_register_e2e_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/nw_udp_listener.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/server_config.cpp)
target_link_libraries(nw_host_register_e2e_test
    PRIVATE opennova_novaworld_service opennova_runtime opennova_app_common)
target_include_directories(nw_host_register_e2e_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server
    ${CMAKE_SOURCE_DIR}/apps/common)
add_test(NAME nw_host_register_e2e COMMAND nw_host_register_e2e_test)
# This is the only test that drives the complete listener lifecycle through
# real UDP sockets. Its one-second negative/retry receive windows are protocol
# assertions, not load-test budgets, so CPU-saturated parallel ctest runs can
# manufacture packet timeouts that never reproduce in isolation.
set_tests_properties(nw_host_register_e2e PROPERTIES RUN_SERIAL TRUE)

# The lobby's answer to a stock client's 0x44 resend request, through the real
# listener over loopback UDP (same listener-source recipe as above).
add_executable(nw_lobby_resend_e2e_test
    novaworld/nw_lobby_resend_e2e_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/nw_udp_listener.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/server_config.cpp)
target_link_libraries(nw_lobby_resend_e2e_test
    PRIVATE opennova_novaworld_service opennova_runtime opennova_app_common)
target_include_directories(nw_lobby_resend_e2e_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server
    ${CMAKE_SOURCE_DIR}/apps/common)
add_test(NAME nw_lobby_resend_e2e COMMAND nw_lobby_resend_e2e_test)
set_tests_properties(nw_lobby_resend_e2e PROPERTIES RUN_SERIAL TRUE)

# The service's ServerCommand / ServerStopHosting pushes to a listed host, through
# the real listener over loopback UDP (same listener-source recipe as above): the
# RID resolves once hosting, the pushes land as the host session's notices, a
# lost push comes back after the send-interval leg's gap and the client's 0x44,
# and the stop takes the row out of the browser. Its one-second recovery window
# is a protocol assertion, so it runs serially.
add_executable(nw_server_command_push_e2e_test
    novaworld/nw_server_command_push_e2e_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/nw_udp_listener.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/server_config.cpp)
target_link_libraries(nw_server_command_push_e2e_test
    PRIVATE opennova_novaworld_service opennova_runtime opennova_app_common)
target_include_directories(nw_server_command_push_e2e_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server
    ${CMAKE_SOURCE_DIR}/apps/common)
add_test(NAME nw_server_command_push_e2e COMMAND nw_server_command_push_e2e_test)
set_tests_properties(nw_server_command_push_e2e PROPERTIES RUN_SERIAL TRUE)

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

# The service's CD keys below the HTTP layer (apps/novaworld_server/cd_keys.h): the
# serial format, generate / list / assign / revoke, and the DFX2 activations
# (the account's, minting its first key or not, and an admin's manual one), each NWACT
# read back by the client's leg. Compiles the server's cd_keys.cpp directly, as
# hosted_supervisor_test does, so it runs on every build.
add_executable(cd_keys_test
    novaworld/cd_keys_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/cd_keys.cpp)
target_link_libraries(cd_keys_test PRIVATE opennova_novaworld_service)
target_include_directories(cd_keys_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
target_compile_definitions(cd_keys_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(cd_keys COMMAND cd_keys_test)

# The schema backend/migrations builds and the seeds: tables, columns, keys, indexes and
# the rules they hold, the applied list against the directory, the seeds' rows.
add_executable(backend_schema_test novaworld/backend_schema_test.cpp)
target_link_libraries(backend_schema_test PRIVATE opennova_novaworld_service)
target_include_directories(backend_schema_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(backend_schema_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_pg_test(backend_schema COMMAND backend_schema_test)

# The service's HTTP route harness (hosted servers, PR A): the Crow listener from
# opennova_novaworld_server_core on a loopback port over a pooled, migrated database,
# driven with apps/common's http_exchange. Only with the HTTP layer
# (BUILD_NOVAWORLD_HTTP); the service edge is direct for the libpq-containment lint.
if(BUILD_NOVAWORLD_HTTP)
    add_executable(opennova_novaworld_server_http_routes_test novaworld/http_routes_test.cpp)
    target_link_libraries(opennova_novaworld_server_http_routes_test PRIVATE
        opennova_novaworld_server_core opennova_novaworld_service)
    target_include_directories(opennova_novaworld_server_http_routes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(opennova_novaworld_server_http_routes_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(opennova_novaworld_server_http_routes
        COMMAND opennova_novaworld_server_http_routes_test)
    # The concurrent case gives each request up to 30 s; this bounds a hang
    # below ctest's 1500 s default.
    set_tests_properties(opennova_novaworld_server_http_routes PROPERTIES TIMEOUT 120)

    # The listing's owner end to end (host_keys): two in-process services (the
    # HTTP, NW UDP and gate listeners over one pooled database each, one with
    # ONNET_REQUIRE_HOST_KEY) and opennova-serve's Lister with
    # and without an account, plus hand-driven host sessions for the NWU
    # reconnect and a second connection presenting a listed key. Its packet
    # windows are protocol assertions, so it runs serially.
    add_executable(nw_host_ownership_e2e_test novaworld/nw_host_ownership_e2e_test.cpp)
    target_link_libraries(nw_host_ownership_e2e_test PRIVATE
        opennova_novaworld_server_core opennova_serve_core opennova_novaworld_service)
    target_include_directories(nw_host_ownership_e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(nw_host_ownership_e2e_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(nw_host_ownership_e2e COMMAND nw_host_ownership_e2e_test)
    set_tests_properties(nw_host_ownership_e2e PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

    # The owner's pushes end to end: an in-process opennova-serve hosting with an account on an
    # in-process service (the same nw_service_harness.h), administered from the website's routes
    # (POST /api/login, GET /api/me/hosts, POST /api/me/hosts/<rid>/command and /stop): the
    # owner's SetServerName renames the match and saves game.cfg, its TextChatServer runs, another
    # account's push is a 404, and the stop ends the listing. Serial, like the other serve e2e.
    add_executable(nw_owner_admin_e2e_test novaworld/nw_owner_admin_e2e_test.cpp)
    target_link_libraries(nw_owner_admin_e2e_test PRIVATE
        opennova_novaworld_server_core opennova_serve_core opennova_novaworld_service)
    target_include_directories(nw_owner_admin_e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(nw_owner_admin_e2e_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    opennova_add_pg_test(nw_owner_admin_e2e COMMAND nw_owner_admin_e2e_test)
    set_tests_properties(nw_owner_admin_e2e PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

    # A hosted server end to end (apps/novaworld_server/hosted): an in-process service with
    # hosted servers on (the same nw_service_harness.h) and its
    # supervisor running a real opennova-serve child through LocalProcessProvider over a
    # synthetic loose game directory: started from POST /api/me/server/start, listed as its
    # service account and shown as the owner's, renamed by the owner's SetServerName (game.cfg
    # saved), and stopped from POST /api/me/server/stop. ctest passes the opennova-serve binary.
    add_executable(hosted_local_e2e_test novaworld/hosted_local_e2e_test.cpp)
    target_link_libraries(hosted_local_e2e_test PRIVATE
        opennova_novaworld_server_core opennova_novaworld_service)
    target_include_directories(hosted_local_e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(hosted_local_e2e_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_dependencies(hosted_local_e2e_test opennova_serve)
    opennova_add_pg_test(hosted_local_e2e COMMAND hosted_local_e2e_test $<TARGET_FILE:opennova_serve>)
    set_tests_properties(hosted_local_e2e PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)
endif()

# The server binary's signal shutdown: SIGINT and SIGTERM each run main()'s
# orderly stop (exit 0, listeners in order, the database closed) with the HTTP layer
# on or off; with it on, past Crow and a held-open request. A last run checks
# that on every port 0 the gate (and the NW UDP SessionInit) advertise the
# ports the sibling listeners bound. POSIX only (fork, kill); TIMEOUT bounds it
# should the child hang, and the child dies with it.
if(UNIX)
    add_executable(opennova_novaworld_server_signal_shutdown_test
        novaworld/server_signal_shutdown_test.cpp)
    # The service for the test's own database (common/test_database.h).
    target_link_libraries(opennova_novaworld_server_signal_shutdown_test PRIVATE
        opennova_app_common opennova_novaworld_service)
    target_include_directories(opennova_novaworld_server_signal_shutdown_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(opennova_novaworld_server_signal_shutdown_test PRIVATE
        OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        $<$<BOOL:${BUILD_NOVAWORLD_HTTP}>:OPENNOVA_HTTP_ENABLED>)
    add_dependencies(opennova_novaworld_server_signal_shutdown_test opennova_novaworld_server)
    opennova_add_pg_test(opennova_novaworld_server_signal_shutdown
        COMMAND opennova_novaworld_server_signal_shutdown_test $<TARGET_FILE:opennova_novaworld_server>)
    set_tests_properties(opennova_novaworld_server_signal_shutdown PROPERTIES TIMEOUT 120)
endif()

# Unknown-message tracker: record/dedup, sample cap, flush/upsert against the
# migrated unknown_messages table (OPENNOVA_SOURCE_DIR for the SQL on disk), snapshot.
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
add_executable(opennova_serve_novaworld_test
    serve/opennova_serve_novaworld_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/nw_udp_listener.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/server_config.cpp)
target_link_libraries(opennova_serve_novaworld_test PRIVATE opennova_serve_core opennova_novaworld_service)
target_include_directories(opennova_serve_novaworld_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
add_test(NAME opennova_serve_novaworld COMMAND opennova_serve_novaworld_test)
set_tests_properties(opennova_serve_novaworld PROPERTIES RUN_SERIAL TRUE)
# The service's pushes to a listed opennova-serve over its NovaWorld session (the same setup):
# a pushed SetServerName renames the match and saves game.cfg, a pushed Cycle changes the map,
# a pushed ServerStopHosting ends the listing, an unknown RID is refused.
add_executable(opennova_serve_server_command_push_test
    serve/opennova_serve_server_command_push_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/nw_udp_listener.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/server_config.cpp)
target_link_libraries(opennova_serve_server_command_push_test PRIVATE opennova_serve_core opennova_novaworld_service)
target_include_directories(opennova_serve_server_command_push_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
add_test(NAME opennova_serve_server_command_push COMMAND opennova_serve_server_command_push_test)
set_tests_properties(opennova_serve_server_command_push PROPERTIES RUN_SERIAL TRUE)

# opennova-serve's Lister end to end over real loopback UDP against the real NwUdpListener
# and an in-test gate (the host result publishing nothing, the mission start's update, a
# roster edit keeping the other slot, the refresh, the stop).
add_executable(opennova_serve_lister_e2e_test
    serve/opennova_serve_lister_e2e_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/nw_udp_listener.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/server_config.cpp)
target_link_libraries(opennova_serve_lister_e2e_test PRIVATE opennova_serve_core opennova_novaworld_service)
target_include_directories(opennova_serve_lister_e2e_test PRIVATE ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
add_test(NAME opennova_serve_lister_e2e COMMAND opennova_serve_lister_e2e_test)
set_tests_properties(opennova_serve_lister_e2e PROPERTIES RUN_SERIAL TRUE)

# Where a hosted server runs (apps/novaworld_server/hosted/placement.h) and what a slot
# of a machine is (hosted/slots.h): best fit on the fullest shared machine that takes
# servers, the ties, the launches below the fleet's maximums and the waits at them, the
# own-machine add-on's machine, the states that count, the columns' words. Pure
# functions: compiles the source directly, so it runs on every build.
add_executable(hosted_placement_test
    novaworld/hosted_placement_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/hosted/placement.cpp)
target_compile_features(hosted_placement_test PRIVATE cxx_std_17)
target_include_directories(hosted_placement_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
add_test(NAME hosted_placement COMMAND hosted_placement_test)
