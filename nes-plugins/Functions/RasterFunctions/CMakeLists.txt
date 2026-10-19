add_library(nes-raster-codec STATIC MapletCodec.cpp)
target_include_directories(nes-raster-codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(nes-raster-codec PUBLIC cxx_std_23)

add_subdirectory(Focal)
#add_subdirectory(Bands)
#add_subdirectory(Zonal)

if (NES_ENABLES_TESTS)
    add_nes_unit_test(maplet-codec-test tests/MapletCodecTest.cpp)
    target_link_libraries(maplet-codec-test nes-raster-codec)
endif ()