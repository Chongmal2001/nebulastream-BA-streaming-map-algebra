# Binary output format: one record per tuple (maplet dump)
add_plugin_as_library(Binary OutputFormatter binary_output_formatter_plugin BinaryOutputFormatter.cpp)
add_plugin_as_library(Binary OutputFormatterValidation binary_output_formatter_validation_plugin BinaryOutputFormatter.cpp)

foreach (library binary_output_formatter_plugin binary_output_formatter_validation_plugin)
    target_include_directories(${library} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
endforeach ()
