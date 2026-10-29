#include <LoweringRules/LowerToPhysical/LowerToPhysicalWindowedFocalFrame.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

#include <Raster/Focal/FocalBuildPhysicalOperator.hpp>
#include <Raster/Focal/FocalProbePhysicalOperator.hpp>
#include <Raster/Focal/FocalFrameOperatorHandler.hpp>
#include <LoweringRules/LowerToPhysical/FocalLoweringUtil.hpp>
#include <MapletCodec.hpp>
#include <Functions/ConstantValuePhysicalFunction.hpp>
#include <Functions/FunctionProvider.hpp>
#include <Functions/PhysicalFunction.hpp>
#include <Functions/UnboundFieldAccessLogicalFunction.hpp>
#include <Identifiers/Identifier.hpp>
#include <Identifiers/QualifiedIdentifier.hpp>
#include <Interface/Record.hpp>
#include <LoweringRules/AbstractLoweringRule.hpp>
#include <Operators/LogicalOperator.hpp>
#include <Operators/Windows/WindowedFocalFrameLogicalOperator.hpp>
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
#include <ErrorHandling.hpp>
#include <LoweringRuleRegistry.hpp>
#include <PhysicalOperator.hpp>
#include <QueryExecutionConfiguration.hpp>
#include <WindowBasedOperatorHandler.hpp>

namespace NES
{

/// build -> FocalFrameOperatorHandler -> probe
LoweringRuleResultSubgraph LowerToPhysicalFocalFrame::apply(LogicalOperator logicalOperator)
{
    auto focal = logicalOperator.getAs<WindowedFocalFrameLogicalOperator>();
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

    auto payloadPhys = lowerField(focal->getPayloadField());
    std::optional<PhysicalFunction> srcIdPhys;
    std::optional<PhysicalFunction> srcFrameIdPhys;
    if (focal->hasLineage())
    {
        srcIdPhys = lowerField(WindowedFocalFrameLogicalOperator::SrcIdField);
        srcFrameIdPhys = lowerField(WindowedFocalFrameLogicalOperator::SrcFrameIdField);
    }

    auto sliceAndWindowStore = std::make_unique<DefaultTimeBasedSliceStore>(
        windowSize, windowSlide, conf.sliceCacheConfiguration);

    /// Unused by the focal build, required by WindowBuildPhysicalOperator
    auto sliceStoreRef = sliceAndWindowStore->createSliceStoreRef(
        [](Slice& slice, const WorkerThreadId) -> void* { return &slice; },
        [](WindowBasedOperatorHandler& handler, AbstractBufferProvider&)
        { return handler.getCreateNewSlicesFunction(CreateNewSlicesArguments{}); });

    // Operator handler
    auto handler = std::make_shared<FocalFrameOperatorHandler>(
        *inputOriginIds | std::ranges::to<std::vector>(), outputOriginId, std::move(sliceAndWindowStore),
        windowSize, windowSlide, geometry, focalSpec, params.noData);

    // Build operator. Binary maplets carry their tile position; the position fields of the build are unused constants.
    const FocalBuildPhysicalOperator build{
        handlerId, std::move(timeFunction), std::move(sliceStoreRef),
        PhysicalFunction{ConstantInt32ValueFunction{-1}}, PhysicalFunction{ConstantInt32ValueFunction{-1}}, std::move(payloadPhys),
        std::move(srcIdPhys), std::move(srcFrameIdPhys)};

    // Probe operator, output [ts, srcId, srcFrameId, raster, start, end]
    const auto outputField = [](const Identifier& name) { return QualifiedIdentifier{name}; };
    const FocalProbePhysicalOperator probe{
        handlerId,
        windowMetaData,
        geometry,
        focalSpec,
        Raster::TileSpec{
            Raster::WireFormat::Binary,
            Raster::CellType::Float64,
            static_cast<uint32_t>(geometry.tileW),
            static_cast<uint32_t>(geometry.tileH),
            params.noData,
            1,
            static_cast<uint32_t>(geometry.rasterW),
            static_cast<uint32_t>(geometry.rasterH)},
        params.noData,
        QualifiedIdentifier::parse("tileX"),
        QualifiedIdentifier::parse("tileY"),
        outputField(focal->getPayloadField()),
        focal->hasLineage() ? std::optional{outputField(WindowedFocalFrameLogicalOperator::SrcIdField)} : std::nullopt,
        focal->hasLineage() ? std::optional{outputField(WindowedFocalFrameLogicalOperator::SrcFrameIdField)} : std::nullopt,
        outputField(focal->getTimestampField())};

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
LoweringRuleGeneratedRegistrar::RegisterWindowedFocalFrameLoweringRule(LoweringRuleRegistryArguments argument) /// NOLINT
{
    return std::make_unique<LowerToPhysicalFocalFrame>(argument.conf);
}
}
