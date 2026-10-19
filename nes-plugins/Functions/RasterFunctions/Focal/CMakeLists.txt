add_library(nes-raster-focal STATIC
        FocalKernels.cpp
)
target_link_libraries(nes-raster-focal PUBLIC nes-raster-codec)
target_include_directories(nes-raster-focal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(nes-raster-focal PUBLIC cxx_std_23)

if (NES_ENABLES_TESTS)
    add_nes_unit_test(focal-kernels-test ../tests/FocalKernelsTest.cpp)
    target_link_libraries(focal-kernels-test nes-raster-focal)
endif ()