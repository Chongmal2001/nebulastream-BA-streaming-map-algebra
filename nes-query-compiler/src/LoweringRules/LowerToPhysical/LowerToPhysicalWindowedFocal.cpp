#include <LoweringRules/LowerToPhysical/LowerToPhysicalWindowedFocal.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <Focal/FocalBuildPhysicalOperator.hpp>
#include <Focal/FocalProbePhysicalOperator.hpp>
#include <Focal/FocalOperatorHandler.hpp>
#include <FocalDriver.hpp>
#include <FocalKernels.hpp>
#include <FocalKernels.hpp>
#include <DataTypes/DataTypeProvider.hpp>
#include <Functions/FieldAccessPhysicalFunction.hpp>
#include <Functions/FunctionProvider.hpp>
#include <Functions/PhysicalFunction.hpp>
#include <Identifiers/Identifiers.hpp>
#include <Interface/BufferRef/LowerSchemaProvider.hpp>
#include <Interface/Hash/MurMur3HashFunction.hpp>
#include <Interface/PagedVector/PagedVectorRef.hpp>
#include <Interface/Record.hpp>
#include <LoweringRules/AbstractLoweringRule.hpp>
#include <Operators/LogicalOperator.hpp>
#include <Operators/Windows/WindowedFocalLogicalOperator.hpp>
#include <Runtime/AbstractBufferProvider.hpp>
#include <Runtime/Execution/OperatorHandler.hpp>
#include <SliceStore/DefaultTimeBasedSliceStore.hpp>
#include <SliceStore/Slice.hpp>
#include <Traits/FieldMappingTrait.hpp>
#include <Traits/MemoryLayoutTypeTrait.hpp>
#include <Traits/OutputOriginIdsTrait.hpp>
#include <Traits/TraitSet.hpp>
#include <Util/SchemaFactory.hpp>
#include <Watermark/TimeFunction.hpp>
#include <WindowTypes/Measures/TimeCharacteristic.hpp>
#include <WindowTypes/Types/TimeBasedWindowType.hpp>
#include <magic_enum/magic_enum.hpp>

#include <DataTypes/UnboundField.hpp>
#include <Functions/FieldAccessLogicalFunction.hpp>
#include <Functions/LogicalFunction.hpp>
#include <Identifiers/Identifier.hpp>
#include <Identifiers/QualifiedIdentifier.hpp>
#include <ErrorHandling.hpp>
#include <LoweringRuleRegistry.hpp>
#include <PhysicalOperator.hpp>
#include <QueryExecutionConfiguration.hpp>
#include <WindowBasedOperatorHandler.hpp>

namespace NES
{
/// Converter helpers
namespace
{
Raster::RasterGeometry toRasterGeometry(const FocalParameters& params)
{
    return Raster::RasterGeometry{
        params.rasterWidth,
        params.rasterHeight,
        params.tileWidth,
        params.tileHeight,
    };
}

Raster::ReductionOp toReductionOp(const FocalReductionOp op)
{
    switch (op)
    {
        case FocalReductionOp::Sum:  return Raster::ReductionOp::Sum;
        case FocalReductionOp::Mean: return Raster::ReductionOp::Mean;
        case FocalReductionOp::Max:  return Raster::ReductionOp::Max;
        case FocalReductionOp::Min:  return Raster::ReductionOp::Min;
            throw NotImplemented("FOCAL reduction '{}' is not yet implemented in the raster plugin",
                                 magic_enum::enum_name(op));
    }
    std::unreachable();
}

Raster::BoundaryRule toBoundaryRule(const FocalBoundaryRule rule)
{
    switch (rule)
    {
        case FocalBoundaryRule::NoData:  return Raster::BoundaryRule::NoData;
        case FocalBoundaryRule::Clamp:   return Raster::BoundaryRule::Clamp;
        case FocalBoundaryRule::Reflect: return Raster::BoundaryRule::Reflect;
    }
    std::unreachable();
}

Raster::Neighbourhood toNeighbourhood(const FocalNeighbourhoodShape shape, const int32_t radius)
{
    switch (shape)
    {
        case FocalNeighbourhoodShape::Square: return Raster::makeDenseSquare(radius);
        case FocalNeighbourhoodShape::Disc:   return Raster::makeDisc(radius);
    }
    std::unreachable();
}

Raster::FocalSpec toFocalSpec(const FocalParameters& params)
{
    return Raster::FocalSpec{
        toNeighbourhood(params.neighbourhood, params.radius),
        toReductionOp(params.reduction),
        toBoundaryRule(params.boundary)};
}
Raster::TileSpec toTileSpec(const Raster::RasterGeometry& geom, double noData)
{
    return Raster::TileSpec{
        Raster::WireFormat::Text,
        Raster::CellType::Float64,
        static_cast<uint32_t>(geom.tileW),
        static_cast<uint32_t>(geom.tileH),
        noData};
}

template <typename Policy>
std::pair<std::shared_ptr<WindowBasedOperatorHandler>, std::unique_ptr<SliceStoreRef>>
makeFocalHandlerAndRef(
    std::unique_ptr<DefaultTimeBasedSliceStore> store,
    std::vector<OriginId> inputOrigins,
    OriginId outputOriginId,
    uint64_t windowSize,
    uint64_t windowSlide,
    Raster::RasterGeometry geometry,
    const Raster::FocalSpec& focalSpec,
    double noData)
{
    auto sliceStoreRef = store->createSliceStoreRef(
        [](Slice& slice, const WorkerThreadId) -> void*
        {
            /// Focal build calls onTileArrival directly; this per-thread pointer is unused by execute().
            return &dynamic_cast<FocalSlice<Policy>&>(slice);   // TODO: nullptr if base never dereferences it
        },

        [](WindowBasedOperatorHandler& handler, AbstractBufferProvider&)
        {
            /// getCreateNewSlicesFunction is virtual on the base, no downcast needed
            return handler.getCreateNewSlicesFunction(CreateNewSlicesArguments{});
        });

    auto handler = std::make_shared<FocalOperatorHandlerImpl<Policy>>(
        std::move(inputOrigins), outputOriginId, std::move(store),
        windowSize, windowSlide, std::move(geometry), focalSpec, noData);

    return {std::move(handler), std::move(sliceStoreRef)};
}
}


LoweringRuleResultSubgraph LowerToPhysicalFocal::apply(LogicalOperator logicalOperator)
{
    auto focal = logicalOperator.getAs<WindowedFocalLogicalOperator>();
    const auto traitSet = logicalOperator.getTraitSet();
    const auto childTraitSet = focal->getChild().getTraitSet();

    auto outputOriginIds = traitSet.get<OutputOriginIdsTrait>();
    auto inputOriginIds = childTraitSet.get<OutputOriginIdsTrait>();
    PRECONDITION(std::ranges::size(*outputOriginIds) == 1, "Expected one output origin id");
    auto outputOriginId = (*outputOriginIds)[0];

    const auto memoryLayoutType = traitSet.get<MemoryLayoutTypeTrait>()->memoryLayout;

    PRECONDITION(
        std::holds_alternative<Windowing::BoundTimeCharacteristic>(focal->getCharacteristic()),
        "Expected time characteristic to be bound");

    auto handlerId = getNextOperatorHandlerId();
    auto timeFunction = TimeFunction::create(std::get<Windowing::BoundTimeCharacteristic>(focal->getCharacteristic()));
    auto windowType = focal->getWindowType();

    const auto physicalInputSchema  = createPhysicalOutputSchema(childTraitSet);
    const auto physicalOutputSchema = createPhysicalOutputSchema(traitSet);
    const auto windowMetaData = WindowMetaData{focal->getWindowStartField(), focal->getWindowEndField()};

    const auto params = focal->getParameters();
    auto geometry = toRasterGeometry(params);
    auto focalSpec = toFocalSpec(params);
    const auto windowSize = windowType.getSize().getTime();
    const auto windowSlide = windowType.getSlide().getTime();

    // Field accessors for the build
    const auto& fieldMapping = *childTraitSet.get<FieldMappingTrait>();
    const auto childSchema = focal->getChild().getOutputSchema();

    auto lowerField = [&](const Identifier& name) -> PhysicalFunction
    {
        /// Identifier -> unbound access -> bound (type resolved from child schema) -> physical.
        auto bound = UnboundFieldAccessLogicalFunction{name}.withInferredDataType(childSchema);
        return QueryCompilation::FunctionProvider::lowerFunction(bound, fieldMapping);
    };

    auto tileXPhys   = lowerField(focal->getTileXField());
    auto tileYPhys   = lowerField(focal->getTileYField());
    auto payloadPhys = lowerField(focal->getPayloadField());

    auto sliceAndWindowStore = std::make_unique<DefaultTimeBasedSliceStore>(
        windowSize, windowSlide, conf.sliceCacheConfiguration);

    auto sliceStoreRef = sliceAndWindowStore->createSliceStoreRef(
        [](Slice& slice, const WorkerThreadId) -> void*
        { return &dynamic_cast<FocalSlice<MultiThreadedPolicy>&>(slice); },
        [](WindowBasedOperatorHandler& handler, AbstractBufferProvider&)
        { return handler.getCreateNewSlicesFunction(CreateNewSlicesArguments{}); });

    // Operator handler
    auto handler = std::make_shared<FocalOperatorHandlerImpl<MultiThreadedPolicy>>(
        *inputOriginIds | std::ranges::to<std::vector>(), outputOriginId, std::move(sliceAndWindowStore),
        windowSize, windowSlide, geometry, focalSpec, params.noData);

    // Build operator
    const FocalBuildPhysicalOperator build{
        handlerId, std::move(timeFunction), std::move(sliceStoreRef),
        std::move(tileXPhys), std::move(tileYPhys), std::move(payloadPhys)};

    // Probe operator
    const FocalProbePhysicalOperator probe{
        handlerId,
        windowMetaData,
        geometry,
        focalSpec,
        toTileSpec(geometry, params.noData),
        params.noData,
        QualifiedIdentifier::parse("tileX"),
        QualifiedIdentifier::parse("tileY"),
        QualifiedIdentifier::parse(focal->getPayloadField().getOriginalString())
    };

    auto buildWrapper = std::make_shared<PhysicalOperatorWrapper>(
    build, physicalInputSchema, physicalOutputSchema,
    memoryLayoutType, memoryLayoutType, handlerId, handler,
    PhysicalOperatorWrapper::PipelineLocation::EMIT);

    auto probeWrapper = std::make_shared<PhysicalOperatorWrapper>(
        probe, physicalInputSchema, physicalOutputSchema,
        memoryLayoutType, memoryLayoutType, handlerId, handler,
        PhysicalOperatorWrapper::PipelineLocation::SCAN,
        std::vector{buildWrapper});

    std::vector leaves(logicalOperator.getChildren().size(), buildWrapper);
    return {.root = probeWrapper, .leaves = {leaves}};
}

std::unique_ptr<AbstractLoweringRule>
LoweringRuleGeneratedRegistrar::RegisterWindowedFocalLoweringRule(LoweringRuleRegistryArguments argument) /// NOLINT
{
    return std::make_unique<LowerToPhysicalFocal>(argument.conf);
}
}
