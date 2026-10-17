# opennova-novaworld-web: the NovaWorld service's web program
# (apps/novaworld_common/README.md "Programs"), built with the HTTP layer
# (BUILD_NOVAWORLD_HTTP). Its own code builds as opennova_novaworld_web_core
# (the HTTP listener, its routes, the stores it keeps in Redis), which the
# tests drive in-process; the executable is its main.cpp over it and the
# shared library. The retail pages (templates/, static/) ship in its image.
add_library(opennova_novaworld_web_core STATIC
    http_listener.cpp
    # The admin's user routes (moved out of http_listener.cpp whole).
    admin_user_routes.cpp
    host_routes.cpp
    # The admin's NovaWorld sessions view.
    connection_routes.cpp
    # CD keys (migration 0015): the keys, the DFX2 activations, their routes.
    cd_keys.cpp
    cd_key_routes.cpp
    # Web's shared state in Redis (over the shared library's client and
    # pool): the retail sessions, the retail login pins and the rate
    # limiters' buckets.
    session_store.cpp
    retail_pins.cpp
    rate_limiter.cpp
    web_access.cpp
    # The password reset routes (the links are the shared library's).
    password_reset_routes.cpp
    # The admin's view of the background jobs and its routes.
    jobs/job_admin.cpp
    jobs/job_routes.cpp
    # The hosted servers' routes: the owners' and the admin's, the console,
    # the per-server stats, the fleet.
    hosted/console_commands.cpp
    hosted/console_routes.cpp
    hosted/fleet_routes.cpp
    hosted/hosted_routes.cpp
    hosted/stats_routes.cpp
    # The hosted data sets' and the update releases' admin routes over the object store
    # (ADR 0064), and the jobs they enqueue for the worker.
    data_sets/data_set_routes.cpp
    data_sets/release_routes.cpp
    # Billing's routes and the webhook's signature.
    billing/billing_routes.cpp
    billing/stripe_signature.cpp
    # The admin's infrastructure view: its route, and its AWS reads (signed by
    # curl, the Query APIs' XML read here).
    infra/aws_api.cpp
    infra/aws_xml.cpp
    infra/infra_routes.cpp
    infra/infra_status.cpp
)
target_include_directories(opennova_novaworld_web_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The shared library directly: the hiredis containment
# (scripts/lint/link_graph_check.py) wants every target that reaches
# opennova_redis to link it itself, and the libpq containment the service.
target_link_libraries(opennova_novaworld_web_core
    PUBLIC
        opennova_novaworld_common
        opennova_novaworld_service
)
target_compile_features(opennova_novaworld_web_core PUBLIC cxx_std_17)

add_executable(opennova_novaworld_web main.cpp)
target_link_libraries(opennova_novaworld_web
    PRIVATE
        opennova_novaworld_web_core
        opennova_novaworld_common
        opennova_novaworld_service
)
set_target_properties(opennova_novaworld_web PROPERTIES OUTPUT_NAME "opennova-novaworld-web")
