#include <RasterFileSource.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <ostream>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <Configurations/Descriptor.hpp>
#include <DataTypes/DataType.hpp>
#include <Identifiers/Identifier.hpp>
#include <Runtime/AbstractBufferProvider.hpp>
#include <Runtime/TupleBuffer.hpp>
#include <Runtime/VariableSizedAccess.hpp>
#include <Sources/Source.hpp>
#include <Sources/SourceDescriptor.hpp>
#include <ErrorHandling.hpp>
#include <FileDataRegistry.hpp>
#include <SourceRegistry.hpp>
#include <SourceValidationRegistry.hpp>
#include <TileStoreReader.hpp>

namespace NES
{
namespace
{
/// Row layout: fields packed in schema order, all NOT NULL
struct RowField
{
    std::string_view name;
    DataType::Type type;
};
constexpr std::array<RowField, 4> RowFields{
    {{"ts", DataType::Type::UINT64},
     {"srcId", DataType::Type::UINT32},
     {"srcFrameId", DataType::Type::UINT32},
     {"raster", DataType::Type::VARSIZED}}};

constexpr uint64_t OffsetTs = 0;
constexpr uint64_t OffsetSrcId = 8;
constexpr uint64_t OffsetSrcFrameId = 12;
constexpr uint64_t OffsetRaster = 16;
constexpr uint64_t RowSize = OffsetRaster + sizeof(VariableSizedAccess);
static_assert(RowSize == 32);

void checkSchema(const Schema<UnqualifiedUnboundField, Ordered>& schema)
{
    const auto expected = std::string{"(ts UINT64 NOT NULL, srcId UINT32 NOT NULL, srcFrameId UINT32 NOT NULL, raster VARSIZED NOT NULL)"};
    if (schema.size() != RowFields.size())
    {
        throw CannotOpenSource("RasterFile: the logical source must be {}, it has {} fields", expected, schema.size());
    }
    for (size_t i = 0; i < RowFields.size(); ++i)
    {
        const auto field = schema[i].value();
        const auto& name = static_cast<const Identifier&>(field.getFullyQualifiedName());
        const auto& type = field.getDataType();
        if (name != Identifier::parse(std::string{RowFields[i].name}) || not type.isType(RowFields[i].type) || type.nullable)
        {
            throw CannotOpenSource("RasterFile: the logical source must be {}, field {} is {}", expected, i, field);
        }
    }
    INVARIANT(schema.getSizeInBytes() == RowSize, "RasterFile row of {} bytes, expected {}", schema.getSizeInBytes(), RowSize);
}
}

RasterFileSource::RasterFileSource(const SourceDescriptor& sourceDescriptor)
    : storePath(sourceDescriptor.getFromConfig(ConfigParametersRasterFile::FILEPATH))
    , srcId(sourceDescriptor.getFromConfig(ConfigParametersRasterFile::SRC_ID))
    , schema(sourceDescriptor.getLogicalSource().getSchema())
{
}

void RasterFileSource::open(std::shared_ptr<AbstractBufferProvider> bufferProvider)
{
    checkSchema(*schema);
    this->bufferProvider = std::move(bufferProvider);
    reader.emplace(storePath);
}

void RasterFileSource::close()
{
    reader.reset();
}

/// One row per maplet until full; each maplet in its own unpooled child buffer
Source::FillTupleBufferResult RasterFileSource::fillTupleBuffer(TupleBuffer& tupleBuffer, const std::stop_token&)
{
    PRECONDITION(reader.has_value() && bufferProvider != nullptr, "RasterFile source used before open()");
    const auto capacity = tupleBuffer.getBufferSize() / RowSize;
    const auto mapletSize = reader->mapletSize();
    auto rows = tupleBuffer.getAvailableMemoryArea<std::byte>();

    uint64_t numberOfTuples = 0;
    while (numberOfTuples < capacity && not reader->atEnd())
    {
        auto child = bufferProvider->getUnpooledBuffer(mapletSize);
        if (not child.has_value())
        {
            throw CannotAllocateBuffer("RasterFile: {} bytes for a maplet", mapletSize);
        }
        const auto info = reader->next(child->getAvailableMemoryArea<int8_t>().first(mapletSize));
        /// var-sized child: used bytes in its tuple count
        child->setNumberOfTuples(mapletSize);
        const auto index = tupleBuffer.storeChildBuffer(child.value());

        auto* row = rows.data() + (numberOfTuples * RowSize);
        const VariableSizedAccess raster{index, VariableSizedAccess::Size{mapletSize}};
        std::memcpy(row + OffsetTs, &info.eventTimeMs, sizeof info.eventTimeMs);
        std::memcpy(row + OffsetSrcId, &srcId, sizeof srcId);
        std::memcpy(row + OffsetSrcFrameId, &info.srcFrameId, sizeof info.srcFrameId);
        std::memcpy(row + OffsetRaster, &raster, sizeof raster);
        ++numberOfTuples;
    }
    emittedMaplets += numberOfTuples;

    if (numberOfTuples == 0)
    {
        return FillTupleBufferResult::eos();
    }
    /// NATIVE source: value is the tuple count
    return FillTupleBufferResult::withBytes(numberOfTuples);
}

DescriptorConfig::Config RasterFileSource::validateAndFormat(std::unordered_map<std::string, std::string> config)
{
    return DescriptorConfig::validateAndFormat<ConfigParametersRasterFile>(std::move(config), NAME);
}

std::ostream& RasterFileSource::toString(std::ostream& str) const
{
    str << std::format("\nRasterFileSource(store: {}, srcId: {}, emitted maplets: {})", storePath, srcId, emittedMaplets.load());
    return str;
}

SourceValidationRegistryReturnType RegisterRasterFileSourceValidation(SourceValidationRegistryArguments sourceConfig)
{
    return RasterFileSource::validateAndFormat(std::move(sourceConfig.config));
}

SourceRegistryReturnType SourceGeneratedRegistrar::RegisterRasterFileSource(SourceRegistryArguments sourceRegistryArguments)
{
    return std::make_unique<RasterFileSource>(sourceRegistryArguments.sourceDescriptor);
}

/// Systests: ATTACH FILE raster/<store> passes the store directory as FILE_PATH
FileDataRegistryReturnType FileDataGeneratedRegistrar::RegisterRasterFileFileData(FileDataRegistryArguments systestAdaptorArguments)
{
    const auto filePath = Identifier::parse("FILE_PATH");
    if (systestAdaptorArguments.physicalSourceConfig.sourceConfig.contains(filePath))
    {
        throw InvalidConfigParameter("The RasterFile test data cannot be attached if file_path is already set.");
    }
    systestAdaptorArguments.physicalSourceConfig.sourceConfig.emplace(filePath, systestAdaptorArguments.testFilePath.string());
    return systestAdaptorArguments.physicalSourceConfig;
}

}
