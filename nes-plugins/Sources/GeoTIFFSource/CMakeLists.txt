

add_plugin_as_library(GeoTIFF Source           nes-sources-registry geotiff_source_plugin_library            GeoTIFFSource.cpp)
add_plugin_as_library(GeoTIFF SourceValidation nes-sources-registry geotiff_source_validation_plugin_library GeoTIFFSource.cpp)

target_include_directories(geotiff_source_plugin_library
        PUBLIC  include
        PRIVATE .
)
target_include_directories(geotiff_source_validation_plugin_library
        PUBLIC  include
        PRIVATE .
)

find_package(GDAL REQUIRED)
target_link_libraries(geotiff_source_plugin_library PRIVATE GDAL::GDAL)
target_link_libraries(geotiff_source_validation_plugin_library PRIVATE GDAL::GDAL)

add_tests_if_enabled(tests)