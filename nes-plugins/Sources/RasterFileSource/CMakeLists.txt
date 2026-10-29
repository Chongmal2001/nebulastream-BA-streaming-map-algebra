# Frame and maplet reading without NES runtime types (unit-testable)
add_library(nes-raster-file-store STATIC TileStoreReader.cpp)
target_include_directories(nes-raster-file-store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nes-raster-file-store PUBLIC nes-raster-codec PRIVATE nes-common)

add_plugin_as_library(RasterFile Source rasterfile_source_plugin_library RasterFileSource.cpp)
add_plugin_as_library(RasterFile SourceValidation rasterfile_source_validation_plugin_library RasterFileSource.cpp)
add_plugin_as_library(RasterFile FileData rasterfile_file_data_plugin_library RasterFileSource.cpp)

foreach (library rasterfile_source_plugin_library rasterfile_source_validation_plugin_library rasterfile_file_data_plugin_library)
    target_include_directories(${library} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${library} PRIVATE nes-raster-file-store)
endforeach ()

if (NES_ENABLES_TESTS)
    add_nes_unit_test(tile-store-reader-test tests/TileStoreReaderTest.cpp)
    target_link_libraries(tile-store-reader-test nes-raster-file-store nes-raster-codec)
endif ()
