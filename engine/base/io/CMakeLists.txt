# Header-only shared infrastructure (byte/bit IO, fixed-point, strutil).
# No compile-features here on purpose: consumers keep their own C++ standard
# (io/le.h, io/fixed.h, io/byte_*.h, io/bit_stream.h are C++11-clean;
# io/strutil.h needs C++17 and is only included by C++17 libs).
add_library(opennova_io INTERFACE)

# engine/ is the one public include root (ADR 0040): <base/<lib>/file.h>.
target_include_directories(opennova_io
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
