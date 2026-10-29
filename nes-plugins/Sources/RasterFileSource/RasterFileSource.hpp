#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>

#include <Configurations/Descriptor.hpp>
#include <DataTypes/Schema.hpp>
#include <DataTypes/UnboundField.hpp>
#include <Runtime/AbstractBufferProvider.hpp>
#include <Runtime/TupleBuffer.hpp>
#include <Sources/Source.hpp>
#include <Sources/SourceDescriptor.hpp>
#include <TileStoreReader.hpp>

namespace NES
{

/// Reads a tile store: one NATIVE row (ts, srcId, srcFrameId, raster) per maplet
class RasterFileSource final : public Source
{
public:
    static constexpr std::string_view NAME = "RasterFile";

    explicit RasterFileSource(const SourceDescriptor& sourceDescriptor);
    ~RasterFileSource() override = default;

    RasterFileSource(const RasterFileSource&) = delete;
    RasterFileSource& operator=(const RasterFileSource&) = delete;
    RasterFileSource(RasterFileSource&&) = delete;
    RasterFileSource& operator=(RasterFileSource&&) = delete;

    FillTupleBufferResult fillTupleBuffer(TupleBuffer& tupleBuffer, const std::stop_token& stopToken) override;

    /// Checks the logical schema and reads the first frame header
    void open(std::shared_ptr<AbstractBufferProvider> bufferProvider) override;
    /// Closes the open frame file
    void close() override;

    /// validates and formats a string to string configuration
    static DescriptorConfig::Config validateAndFormat(std::unordered_map<std::string, std::string> config);

    [[nodiscard]] std::ostream& toString(std::ostream& str) const override;

private:
    std::string storePath;
    uint32_t srcId;
    std::shared_ptr<const Schema<UnqualifiedUnboundField, Ordered>> schema;

    std::shared_ptr<AbstractBufferProvider> bufferProvider;
    std::optional<TileStoreReader> reader;
    std::atomic<uint64_t> emittedMaplets = 0;
};

struct ConfigParametersRasterFile
{
    /// Store directory (frames/*.mpl); same key as the File source for ATTACH FILE
    static inline const DescriptorConfig::ConfigParameter<std::string> FILEPATH{
        "FILE_PATH",
        std::nullopt,
        [](const std::unordered_map<std::string, std::string>& config) { return DescriptorConfig::tryGet(FILEPATH, config); }};

    /// srcId of every row (store identity)
    static inline const DescriptorConfig::ConfigParameter<uint32_t> SRC_ID{
        "SRC_ID",
        1,
        [](const std::unordered_map<std::string, std::string>& config) { return DescriptorConfig::tryGet(SRC_ID, config); }};

    static inline std::unordered_map<std::string, DescriptorConfig::ConfigParameterContainer> parameterMap
        = DescriptorConfig::createConfigParameterContainerMap(SourceDescriptor::parameterMap, FILEPATH, SRC_ID);
};

}
