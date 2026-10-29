add_library(nes-raster-codec STATIC MapletCodec.cpp)
target_include_directories(nes-raster-codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(nes-raster-codec PUBLIC cxx_std_23)

add_subdirectory(Focal)
add_subdirectory(Bands)
#add_subdirectory(Zonal)

if (NES_ENABLES_TESTS)
    # maplet codec
    add_nes_unit_test(maplet-codec-test tests/MapletCodecTest.cpp)
    target_link_libraries(maplet-codec-test nes-raster-codec)
    target_include_directories(maplet-codec-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    # bands function
    add_nes_unit_test(bands-function-test tests/BandsLogicalFunctionTest.cpp tests/BandsPhysicalFunctionTest.cpp)
    target_link_libraries(bands-function-test
            bands_logical_function_plugin_library bands_physical_function_plugin_library
            nes-logical-operators nes-physical-operators nes-raster-codec)
    target_include_directories(bands-function-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    # focal driver
    add_nes_unit_test(focal-driver-test tests/FocalDriverTest.cpp)
    target_link_libraries(focal-driver-test nes-raster-codec)
    target_include_directories(focal-driver-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Focal)
endif ()