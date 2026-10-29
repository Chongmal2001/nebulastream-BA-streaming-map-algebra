#include <Operators/Windows/WindowedFocalFrameLogicalOperator.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <Functions/FieldAccessLogicalFunction.hpp>
#include <Functions/UnboundFieldAccessLogicalFunction.hpp>
#include <Schema/Binder.hpp>
#include <Schema/Field.hpp>
#include <Serialization/ReflectedOperator.hpp>
#include <Serialization/LogicalFunctionReflection.hpp>
#include <Util/Hash.hpp>
#include <Util/Reflection.hpp>
#include <fmt/format.h>
#include <folly/hash/Hash.h>
#include <ErrorHandling.hpp>

namespace NES
{
namespace
{
/// NaN NoData equals NaN NoData (NaN is the NoData value of the tile stores)
bool sameNoData(double lhs, double rhs)
{
    return lhs == rhs || (std::isnan(lhs) && std::isnan(rhs));
}
}

WindowedFocalFrameLogicalOperator::WindowedFocalFrameLogicalOperator(
    WeakLogicalOperator self,
    Identifier payloadField,
    FocalParameters params,
    Windowing::TimeBasedWindowType windowType,
    Windowing::TimeCharacteristic timeCharacteristic)
    : ManagedByOperator(std::move(self))
    , windowType(std::move(windowType))
    , payloadField(std::move(payloadField))
    , params(std::move(params))
    , timestampField(std::move(timeCharacteristic))
{
}

WindowedFocalFrameLogicalOperator::WindowedFocalFrameLogicalOperator(
    WeakLogicalOperator self,
    LogicalOperator child,
    Identifier payloadField,
    FocalParameters params,
    Windowing::TimeBasedWindowType windowType,
    Windowing::TimeCharacteristic timeCharacteristic)
    : ManagedByOperator(std::move(self))
    , child(std::move(child))
    , windowType(std::move(windowType))
    , payloadField(std::move(payloadField))
    , params(std::move(params))
    , timestampField(std::move(timeCharacteristic))
{
    inferLocalSchema();
}

TypedLogicalOperator<WindowedFocalFrameLogicalOperator> WindowedFocalFrameLogicalOperator::create(
    Identifier payloadField, FocalParameters params, Windowing::TimeBasedWindowType windowType, Windowing::TimeCharacteristic timeCharacteristic)
{
    return TypedLogicalOperator<WindowedFocalFrameLogicalOperator>{
        std::move(payloadField), std::move(params), std::move(windowType), std::move(timeCharacteristic)};
}

TypedLogicalOperator<WindowedFocalFrameLogicalOperator> WindowedFocalFrameLogicalOperator::create(
    LogicalOperator child,
    Identifier payloadField,
    FocalParameters params,
    Windowing::TimeBasedWindowType windowType,
    Windowing::TimeCharacteristic timeCharacteristic)
{
    return TypedLogicalOperator<WindowedFocalFrameLogicalOperator>{
        std::move(child), std::move(payloadField), std::move(params), std::move(windowType), std::move(timeCharacteristic)};
}

bool operator==(const FocalParameters& lhs, const FocalParameters& rhs)
{
    return lhs.rasterWidth == rhs.rasterWidth && lhs.rasterHeight == rhs.rasterHeight && lhs.tileWidth == rhs.tileWidth
        && lhs.tileHeight == rhs.tileHeight && lhs.radius == rhs.radius && lhs.neighbourhood == rhs.neighbourhood
        && lhs.reduction == rhs.reduction && lhs.boundary == rhs.boundary && sameNoData(lhs.noData, rhs.noData);
}

std::string_view WindowedFocalFrameLogicalOperator::getName() const noexcept { return NAME; }

std::string WindowedFocalFrameLogicalOperator::explain(ExplainVerbosity verbosity, OperatorId id) const
{
    if (verbosity == ExplainVerbosity::Debug)
    {
        return fmt::format(
            "WINDOW FOCAL FRAME(opId: {}, tiles: {}x{}, raster: {}x{}, radius: {}, lineage: {}, window type: {})",
            id, params.tileWidth, params.tileHeight, params.rasterWidth, params.rasterHeight, params.radius, lineage, windowType);
    }
    return fmt::format("WINDOW FOCAL FRAME(radius: {})", params.radius);
}

bool WindowedFocalFrameLogicalOperator::operator==(const WindowedFocalFrameLogicalOperator& rhs) const
{
    return payloadField == rhs.payloadField && params == rhs.params && windowType == rhs.windowType && outputSchema == rhs.outputSchema && traitSet == rhs.traitSet
        && timestampField == rhs.timestampField && startEndFields == rhs.startEndFields;
}

WindowedFocalFrameLogicalOperator WindowedFocalFrameLogicalOperator::withInferredSchema() const
{
    PRECONDITION(child.has_value(), "Child not set when calling schema inference");
    auto copy = *this;
    copy.child = copy.child->withInferredSchema();
    copy.inferLocalSchema();
    return copy;
}

const detail::DynamicBase* WindowedFocalFrameLogicalOperator::getDynamicBase() const
{
    return static_cast<const Reprojecter*>(this);
}

TraitSet WindowedFocalFrameLogicalOperator::getTraitSet() const { return traitSet; }

WindowedFocalFrameLogicalOperator WindowedFocalFrameLogicalOperator::withTraitSet(TraitSet traitSet) const
{
    auto copy = *this;
    copy.traitSet = std::move(traitSet);
    return copy;
}

WindowedFocalFrameLogicalOperator WindowedFocalFrameLogicalOperator::withChildrenUnsafe(std::vector<LogicalOperator> children) const
{
    PRECONDITION(children.size() == 1, "Can only set exactly one child for WindowedFocalFrame, got {}", children.size());
    auto copy = *this;
    copy.child = std::move(children.at(0));
    return copy;
}

WindowedFocalFrameLogicalOperator WindowedFocalFrameLogicalOperator::withChildren(std::vector<LogicalOperator> children) const
{
    PRECONDITION(children.size() == 1, "Can only set exactly one child for WindowedFocalFrame, got {}", children.size());
    auto copy = *this;
    copy.child = std::move(children.at(0));
    copy.inferLocalSchema();
    return copy;
}

std::vector<LogicalOperator> WindowedFocalFrameLogicalOperator::getChildren() const
{
    if (child.has_value()) { return {*child}; }
    return {};
}

LogicalOperator WindowedFocalFrameLogicalOperator::getChild() const
{
    PRECONDITION(child.has_value(), "Child not set when trying to retrieve child");
    return child.value();
}

Schema<Field, Unordered> WindowedFocalFrameLogicalOperator::getOutputSchema() const
{
    INVARIANT(outputSchema.has_value(), "Retrieving output schema before calling schema inference");
    return NES::bindToOperator(self.lock(), outputSchema.value());
}

/// [ts, srcId, srcFrameId, raster, start, end]
Schema<Field, Ordered> WindowedFocalFrameLogicalOperator::getOrderedOutputSchema(ChildOutputOrderProvider) const
{
    std::vector<Identifier> frameFields{getTimestampField()};
    if (lineage)
    {
        frameFields.push_back(SrcIdField);
        frameFields.push_back(SrcFrameIdField);
    }
    frameFields.push_back(payloadField);
    auto boundFrameFields = frameFields
        | std::views::transform([this](const auto& id) { return NES::bindToOperator(self.lock(), id); })
        | std::ranges::to<Schema<Field, Ordered>>();
    auto boundStartEnd = startEndFields | RangeBinder{self.lock()} | std::ranges::to<Schema<Field, Ordered>>();
    return std::array{std::move(boundFrameFields), std::move(boundStartEnd)} | std::views::join
        | std::ranges::to<Schema<Field, Ordered>>();
}

/// Output raster from the input raster; ts and lineage are trivial forwards, start / end new
std::unordered_map<Field, std::unordered_set<Field>> WindowedFocalFrameLogicalOperator::getAccessedFieldsForOutput() const
{
    PRECONDITION(child.has_value(), "Child not set when retrieving accessed fields");
    const auto outputRaster = getOutputSchema().getFieldByName(payloadField);
    const auto inputRaster = child->getOutputSchema().getFieldByName(payloadField);
    INVARIANT(outputRaster.has_value() && inputRaster.has_value(), "Raster field {} missing after schema inference", payloadField);
    std::unordered_map<Field, std::unordered_set<Field>> accessedFields;
    accessedFields.try_emplace(outputRaster.value(), std::unordered_set<Field>{inputRaster.value()});
    return accessedFields;
}

void WindowedFocalFrameLogicalOperator::inferLocalSchema()
{
    PRECONDITION(child.has_value(), "Child not set when calling schema inference");
    const Schema<Field, Unordered>& inputSchema = child->getOutputSchema();

    /// Raster field, one maplet per tuple; bound against the input schema
    const auto payloadType = UnboundFieldAccessLogicalFunction{payloadField}
        .withInferredDataType(inputSchema).getAs<FieldAccessLogicalFunction>()->getDataType();
    if (not payloadType.isType(DataType::Type::VARSIZED))
    {
        throw CannotInferSchema("FOCAL_* expects a VARSIZED raster field, but {} is not", payloadField);
    }

    /// Event time in ms: the handler writes it back into the output
    const auto boundTime = Windowing::TimeCharacteristicWrapper{std::move(timestampField)}.withInferredSchema(inputSchema);
    timestampField = boundTime;
    const auto* eventTime = std::get_if<Windowing::BoundEventTimeCharacteristic>(&boundTime);
    if (eventTime == nullptr)
    {
        throw CannotInferSchema("FOCAL_* needs event time: all tiles of a frame carry the frame's timestamp");
    }
    if (eventTime->unit.getMillisecondsConversionMultiplier() != 1)
    {
        throw CannotInferSchema("FOCAL_* needs the event time field in milliseconds, got unit {}", eventTime->unit);
    }
    const auto tsField = eventTime->field->getField();
    timestampFieldName = tsField.getLastName();

    /// Lineage fields: found by name, both or neither, UINT32
    const auto srcId = inputSchema.getFieldByName(SrcIdField);
    const auto srcFrameId = inputSchema.getFieldByName(SrcFrameIdField);
    if (srcId.has_value() != srcFrameId.has_value())
    {
        throw CannotInferSchema("FOCAL_* input has only one of the lineage fields {} and {}", SrcIdField, SrcFrameIdField);
    }
    lineage = srcId.has_value();
    if (lineage
        && (not srcId->getDataType().isType(DataType::Type::UINT32) || not srcFrameId->getDataType().isType(DataType::Type::UINT32)))
    {
        throw CannotInferSchema("FOCAL_* lineage fields {} and {} must be UINT32", SrcIdField, SrcFrameIdField);
    }

    /// Output schema: [ts, srcId, srcFrameId, raster, start, end]
    std::vector<UnqualifiedUnboundField> outputFields{};
    outputFields.emplace_back(tsField.getLastName(), tsField.getDataType());
    if (lineage)
    {
        outputFields.emplace_back(SrcIdField, srcId->getDataType());
        outputFields.emplace_back(SrcFrameIdField, srcFrameId->getDataType());
    }
    outputFields.emplace_back(payloadField, payloadType);
    outputFields.push_back(startEndFields[0]);
    outputFields.push_back(startEndFields[1]);

    const auto outputSchemaOrCollisions = Schema<UnqualifiedUnboundField, Unordered>::tryCreateCollisionFree(outputFields);
    if (not outputSchemaOrCollisions.has_value())
    {
        throw CannotInferSchema(
            "Found collisions in focal output schema: "
            + Schema<UnqualifiedUnboundField, Unordered>::createCollisionString(outputSchemaOrCollisions.error()));
    }
    outputSchema = outputSchemaOrCollisions.value();
}


//  focal-specific accessors
const FocalParameters& WindowedFocalFrameLogicalOperator::getParameters() const { return params; }
const Identifier& WindowedFocalFrameLogicalOperator::getPayloadField() const { return payloadField; }
Windowing::TimeBasedWindowType WindowedFocalFrameLogicalOperator::getWindowType() const { return windowType; }
const UnqualifiedUnboundField& WindowedFocalFrameLogicalOperator::getWindowStartField() const { return startEndFields[0]; }
const UnqualifiedUnboundField& WindowedFocalFrameLogicalOperator::getWindowEndField() const { return startEndFields[1]; }

bool WindowedFocalFrameLogicalOperator::hasLineage() const
{
    INVARIANT(outputSchema.has_value(), "Lineage is known only after schema inference");
    return lineage;
}

const Identifier& WindowedFocalFrameLogicalOperator::getTimestampField() const
{
    INVARIANT(timestampFieldName.has_value(), "Timestamp field is known only after schema inference");
    return timestampFieldName.value();
}

std::variant<Windowing::UnboundTimeCharacteristic, Windowing::BoundTimeCharacteristic>
WindowedFocalFrameLogicalOperator::getCharacteristic() const
{
    return timestampField;
}

Reflected Reflector<TypedLogicalOperator<WindowedFocalFrameLogicalOperator>>::operator()(
    const TypedLogicalOperator<WindowedFocalFrameLogicalOperator>& op) const
{
    auto params = op->getParameters();
    const auto noDataBits = std::bit_cast<int64_t>(params.noData);
    params.noData = 0.0; // carried in noDataBits (JSON has no NaN)
    return reflect(detail::ReflectedWindowedFocalFrameLogicalOperator{
        .operatorId     = op.getId(),
        .windowType     = op->getWindowType(),
        .payloadField   = op->getPayloadField(),
        .params         = params,
        .noDataBits     = noDataBits,
        .timestampField = op->getCharacteristic()});
}

Unreflector<TypedLogicalOperator<WindowedFocalFrameLogicalOperator>>::Unreflector(ContextType plan) : plan(std::move(plan))
{
}

TypedLogicalOperator<WindowedFocalFrameLogicalOperator> Unreflector<TypedLogicalOperator<WindowedFocalFrameLogicalOperator>>::operator()(
    const Reflected& reflected, const ReflectionContext& context) const
{
    auto [id, windowType, payloadField, params, noDataBits, timestampField]
        = context.unreflect<detail::ReflectedWindowedFocalFrameLogicalOperator>(reflected);
    params.noData = std::bit_cast<double>(noDataBits);
    auto children = plan->getChildrenFor(id, context);
    if (children.size() != 1)
    {
        throw CannotDeserialize("WindowedFocalFrameLogicalOperator must have exactly one child, but got {}", children.size());
    }
    return WindowedFocalFrameLogicalOperator::create(children.at(0), payloadField, params, windowType, timestampField);
}

}

std::size_t std::hash<NES::WindowedFocalFrameLogicalOperator>::operator()(
    const NES::WindowedFocalFrameLogicalOperator& op) const noexcept
{
    return folly::hash::hash_combine_generic(
        NES::Hash{},
        op.windowType,
        op.timestampField,
        op.payloadField,
        op.params.rasterWidth,
        op.params.rasterHeight,
        op.params.tileWidth,
        op.params.tileHeight,
        op.params.radius,
        static_cast<uint8_t>(op.params.neighbourhood),
        static_cast<uint8_t>(op.params.reduction),
        static_cast<uint8_t>(op.params.boundary),
        std::isnan(op.params.noData) ? std::numeric_limits<double>::quiet_NaN() : op.params.noData); // consistent with sameNoData
}
