#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include <DataTypes/DataType.hpp>
#include <DataTypes/DataTypeProvider.hpp>
#include <DataTypes/Schema.hpp>
#include <DataTypes/UnboundField.hpp>
#include <Functions/UnboundFieldAccessLogicalFunction.hpp>
#include <Identifiers/Identifier.hpp>
#include <Operators/LogicalOperator.hpp>
#include <Operators/Sources/SourceDescriptorLogicalOperator.hpp>
#include <Operators/Windows/WindowedFocalFrameLogicalOperator.hpp>
#include <Sources/LogicalSource.hpp>
#include <Sources/SourceCatalog.hpp>
#include <Sources/SourceDescriptor.hpp>
#include <WindowTypes/Measures/TimeCharacteristic.hpp>
#include <WindowTypes/Measures/TimeMeasure.hpp>
#include <WindowTypes/Types/TimeBasedWindowType.hpp>
#include <WindowTypes/Types/TumblingWindow.hpp>
#include <DistributedQuery.hpp>

namespace NES
{
namespace
{

/// Schema inference: output fields and order, lineage by name, input checks
class WindowedFocalFrameLogicalOperatorTest : public ::testing::Test
{
public:
    using Fields = std::vector<std::pair<std::string, DataType::Type>>;

    static TypedLogicalOperator<SourceDescriptorLogicalOperator> makeSource(SourceCatalog& catalog, const Fields& fields)
    {
        std::vector<UnqualifiedUnboundField> unbound;
        for (const auto& [name, type] : fields)
        {
            unbound.emplace_back(Identifier::parse(name), DataTypeProvider::provideDataType(type));
        }
        const auto logical
            = catalog.addLogicalSource(Identifier::parse("raster_src"), unbound | std::ranges::to<Schema<UnqualifiedUnboundField, Ordered>>())
                  .value();
        const std::unordered_map<Identifier, std::string> sourceConfig{{Identifier::parse("file_path"), "/dev/null"}};
        const std::unordered_map<Identifier, std::string> parserConfig{{Identifier::parse("type"), "CSV"}};
        const auto descriptor
            = catalog.addPhysicalSource(logical, Identifier::parse("file"), Host{"localhost"}, sourceConfig, parserConfig).value();
        return SourceDescriptorLogicalOperator::create(descriptor);
    }

    static Windowing::TimeCharacteristic eventTime(std::string_view field)
    {
        return Windowing::UnboundTimeCharacteristic{Windowing::TimeCharacteristicWrapper::createEventTime(
            UnboundFieldAccessLogicalFunction{Identifier::parse(std::string{field})}, Windowing::TimeUnit::Milliseconds())};
    }

    /// Inference runs in the constructor; the catalog must outlive the operator
    static TypedLogicalOperator<WindowedFocalFrameLogicalOperator>
    makeFocal(SourceCatalog& catalog, const Fields& fields, Windowing::TimeCharacteristic time = eventTime("ts"))
    {
        const FocalParameters params{.rasterWidth = 4, .rasterHeight = 4, .tileWidth = 2, .tileHeight = 2, .radius = 1};
        return WindowedFocalFrameLogicalOperator::create(
            makeSource(catalog, fields),
            Identifier::parse("raster"),
            params,
            Windowing::TimeBasedWindowType{Windowing::TumblingWindow{Windowing::TimeMeasure(100)}},
            std::move(time));
    }

    static std::vector<std::string> orderedNames(const TypedLogicalOperator<WindowedFocalFrameLogicalOperator>& op)
    {
        std::vector<std::string> names;
        for (const auto& field : op->getOrderedOutputSchema({}))
        {
            names.emplace_back(field.getLastName().getOriginalString());
        }
        return names;
    }

    static const Fields& rasterSource()
    {
        static const Fields fields{
            {"ts", DataType::Type::UINT64},
            {"srcId", DataType::Type::UINT32},
            {"srcFrameId", DataType::Type::UINT32},
            {"raster", DataType::Type::VARSIZED}};
        return fields;
    }
};

TEST_F(WindowedFocalFrameLogicalOperatorTest, LineageAndTimestampPassThrough)
{
    SourceCatalog catalog;
    const auto focal = makeFocal(catalog, rasterSource());
    EXPECT_TRUE(focal->hasLineage());
    EXPECT_EQ(focal->getTimestampField(), Identifier::parse("ts"));
    EXPECT_EQ(orderedNames(focal), (std::vector<std::string>{"ts", "srcId", "srcFrameId", "raster", "start", "end"}));

    const auto output = focal->getOutputSchema();
    EXPECT_TRUE(output.getFieldByName(Identifier::parse("srcId"))->getDataType().isType(DataType::Type::UINT32));
    EXPECT_TRUE(output.getFieldByName(Identifier::parse("raster"))->getDataType().isType(DataType::Type::VARSIZED));
}

TEST_F(WindowedFocalFrameLogicalOperatorTest, WithoutLineage)
{
    SourceCatalog catalog;
    const auto focal = makeFocal(catalog, {{"ts", DataType::Type::UINT64}, {"raster", DataType::Type::VARSIZED}});
    EXPECT_FALSE(focal->hasLineage());
    EXPECT_EQ(orderedNames(focal), (std::vector<std::string>{"ts", "raster", "start", "end"}));
}

/// Fields the operator does not use (e.g. the legacy tile position) are not part of its output
TEST_F(WindowedFocalFrameLogicalOperatorTest, OtherInputFieldsDropped)
{
    SourceCatalog catalog;
    const auto focal = makeFocal(
        catalog, {{"ts", DataType::Type::UINT64}, {"tileX", DataType::Type::INT32}, {"raster", DataType::Type::VARSIZED}});
    EXPECT_EQ(orderedNames(focal), (std::vector<std::string>{"ts", "raster", "start", "end"}));
}

/// The output raster is computed from the input raster; nothing else is reported
TEST_F(WindowedFocalFrameLogicalOperatorTest, AccessedFieldsForOutput)
{
    SourceCatalog catalog;
    const auto focal = makeFocal(catalog, rasterSource());
    const auto accessed = focal->getAccessedFieldsForOutput();
    ASSERT_EQ(accessed.size(), 1U);
    const auto& [output, inputs] = *accessed.begin();
    EXPECT_EQ(output.getLastName(), Identifier::parse("raster"));
    ASSERT_EQ(inputs.size(), 1U);
    EXPECT_EQ(inputs.begin()->getLastName(), Identifier::parse("raster"));
}

TEST_F(WindowedFocalFrameLogicalOperatorTest, OnlyOneLineageFieldThrows)
{
    SourceCatalog catalog;
    EXPECT_ANY_THROW(static_cast<void>(makeFocal(
        catalog, {{"ts", DataType::Type::UINT64}, {"srcId", DataType::Type::UINT32}, {"raster", DataType::Type::VARSIZED}})));
}

TEST_F(WindowedFocalFrameLogicalOperatorTest, LineageFieldOfWrongTypeThrows)
{
    SourceCatalog catalog;
    EXPECT_ANY_THROW(static_cast<void>(makeFocal(
        catalog,
        {{"ts", DataType::Type::UINT64},
         {"srcId", DataType::Type::UINT64},
         {"srcFrameId", DataType::Type::UINT32},
         {"raster", DataType::Type::VARSIZED}})));
}

TEST_F(WindowedFocalFrameLogicalOperatorTest, NonVarsizedRasterThrows)
{
    SourceCatalog catalog;
    EXPECT_ANY_THROW(static_cast<void>(makeFocal(catalog, {{"ts", DataType::Type::UINT64}, {"raster", DataType::Type::UINT64}})));
}

TEST_F(WindowedFocalFrameLogicalOperatorTest, IngestionTimeThrows)
{
    SourceCatalog catalog;
    EXPECT_ANY_THROW(static_cast<void>(makeFocal(
        catalog,
        {{"ts", DataType::Type::UINT64}, {"raster", DataType::Type::VARSIZED}},
        Windowing::UnboundTimeCharacteristic{Windowing::IngestionTimeCharacteristic{}})));
}

TEST_F(WindowedFocalFrameLogicalOperatorTest, EventTimeNotInMillisecondsThrows)
{
    SourceCatalog catalog;
    EXPECT_ANY_THROW(static_cast<void>(makeFocal(
        catalog,
        {{"ts", DataType::Type::UINT64}, {"raster", DataType::Type::VARSIZED}},
        Windowing::UnboundTimeCharacteristic{Windowing::TimeCharacteristicWrapper::createEventTime(
            UnboundFieldAccessLogicalFunction{Identifier::parse("ts")}, Windowing::TimeUnit::Seconds())})));
}

}
}
