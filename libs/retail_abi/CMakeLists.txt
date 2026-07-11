add_library(opennova_retail_abi INTERFACE)
add_library(OpenNova::retail_abi ALIAS opennova_retail_abi)

target_compile_features(opennova_retail_abi INTERFACE cxx_std_17)

target_include_directories(
    opennova_retail_abi
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
