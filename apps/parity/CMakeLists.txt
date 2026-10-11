# opennova-parity: the retail-LAN parity verdict (ADR 0050 R4). Decodes each
# topology run's evidence capture into the canonical trace (opennova_wire_trace,
# the builder opennova-wire --parity-events prints), compares every subject
# topology (RO, OR, OO) to the case's RR run per direction and metric, and judges
# each distance against the retail-vs-retail A/A noise floor. Tooling, not a port.
add_library(opennova_parity_core STATIC
    cli.cpp
    metric_math.cpp
    metric_state.cpp     # the decoded C2S 0x0C / S2C 0x0A state metrics
    metric_wire.cpp      # the histogram, initial stream, envelope, handshake, integrity
    run_facts.cpp        # run-summary.json, the session binding, the windows
    verdict.cpp
)

target_include_directories(opennova_parity_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(opennova_parity_core
    PUBLIC
        opennova_wire_trace
)

target_compile_features(opennova_parity_core PUBLIC cxx_std_17)

add_executable(opennova_parity
    main.cpp
)

target_link_libraries(opennova_parity
    PRIVATE
        opennova_parity_core
)

target_compile_features(opennova_parity PRIVATE cxx_std_17)
set_target_properties(opennova_parity PROPERTIES OUTPUT_NAME "opennova-parity")
