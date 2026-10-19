#include <Operators/Windows/WindowedFocalLogicalOperator.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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


WindowedFocalLogicalOperator::WindowedFocalLogicalOperator(
    WeakLogicalOperator self,
    Identifier tileXField, Identifier tileYField, Identifier payloadField,
    FocalParameters params,
    Windowing::TimeBasedWindowType windowType,
    Windowing::TimeCharacteristic timeCharacteristic)
    : ManagedByOperator(std::move(self))
    , windowType(std::move(windowType))
    , tileXField(std::move(tileXField))
    , tileYField(std::move(tileYField))
    , payloadField(std::move(payloadField))
    , params(std::move(params))
    , timestampField(std::move(timeCharacteristic))
{
}

WindowedFocalLogicalOperator::WindowedFocalLogicalOperator(
    WeakLogicalOperator self,
    LogicalOperator child,
    Identifier tileXField, Identifier tileYField, Identifier payloadField,
    FocalParameters params,
    Windowing::TimeBasedWindowType windowType,
    Windowing::TimeCharacteristic timeCharacteristic)
    : ManagedByOperator(std::move(self))
    , child(std::move(child))
    , windowType(std::move(windowType))
    , tileXField(std::move(tileXField))
    , tileYField(std::move(tileYField))
    , payloadField(std::move(payloadField))
    , params(std::move(params))
    , timestampField(std::move(timeCharacteristic))
{
    inferLocalSchema();
}

TypedLogicalOperator<WindowedFocalLogicalOperator> WindowedFocalLogicalOperator::create(
    Identifier tileXField, Identifier tileYField, Identifier payloadField,
    FocalParameters params, Windowing::TimeBasedWindowType windowType, Windowing::TimeCharacteristic timeCharacteristic)
{
    return TypedLogicalOperator<WindowedFocalLogicalOperator>{
        std::move(tileXField), std::move(tileYField), std::move(payloadField),
        std::move(params), std::move(windowType), std::move(timeCharacteristic)};
}

TypedLogicalOperator<WindowedFocalLogicalOperator> WindowedFocalLogicalOperator::create(
    LogicalOperator child,
    Identifier tileXField, Identifier tileYField, Identifier payloadField,
    FocalParameters params, Windowing::TimeBasedWindowType windowType, Windowing::TimeCharacteristic timeCharacteristic)
{
    return TypedLogicalOperator<WindowedFocalLogicalOperator>{
        std::move(child), std::move(tileXField), std::move(tileYField), std::move(payloadField),
        std::move(params), std::move(windowType), std::move(timeCharacteristic)};
}

bool operator==(const FocalParameters& lhs, const FocalParameters& rhs)
{
    return lhs.rasterWidth == rhs.rasterWidth && lhs.rasterHeight == rhs.rasterHeight
        && lhs.tileWidth == rhs.tileWidth && lhs.tileHeight == rhs.tileHeight
        && lhs.radius == rhs.radius && lhs.reduction == rhs.reduction
        && lhs.boundary == rhs.boundary && lhs.noData == rhs.noData;
}

std::string_view WindowedFocalLogicalOperator::getName() const noexcept { return NAME; }

std::string WindowedFocalLogicalOperator::explain(ExplainVerbosity verbosity, OperatorId id) const
{
    if (verbosity == ExplainVerbosity::Debug)
    {
        return fmt::format(
            "WINDOW FOCAL(opId: {}, tiles: {}x{}, raster: {}x{}, radius: {}, window type: {})",
            id, params.tileWidth, params.tileHeight, params.rasterWidth, params.rasterHeight, params.radius, windowType);
    }
    return fmt::format("WINDOW FOCAL(radius: {})", params.radius);
}

bool WindowedFocalLogicalOperator::operator==(const WindowedFocalLogicalOperator& rhs) const
{
    return tileXField == rhs.tileXField && tileYField == rhs.tileYField && payloadField == rhs.payloadField
        && params == rhs.params && windowType == rhs.windowType && outputSchema == rhs.outputSchema
        && traitSet == rhs.traitSet && timestampField == rhs.timestampField && startEndFields == rhs.startEndFields;
}

WindowedFocalLogicalOperator WindowedFocalLogicalOperator::withInferredSchema() const
{
    PRECONDITION(child.has_value(), "Child not set when calling schema inference");
    auto copy = *this;
    copy.child = copy.child->withInferredSchema();
    copy.inferLocalSchema();
    return copy;
}

const detail::DynamicBase* WindowedFocalLogicalOperator::getDynamicBase() const
{
    return static_cast<const Reprojecter*>(this);
}

TraitSet WindowedFocalLogicalOperator::getTraitSet() const { return traitSet; }

WindowedFocalLogicalOperator WindowedFocalLogicalOperator::withTraitSet(TraitSet traitSet) const
{
    auto copy = *this;
    copy.traitSet = std::move(traitSet);
    return copy;
}

WindowedFocalLogicalOperator WindowedFocalLogicalOperator::withChildrenUnsafe(std::vector<LogicalOperator> children) const
{
    PRECONDITION(children.size() == 1, "Can only set exactly one child for WindowedFocal, got {}", children.size());
    auto copy = *this;
    copy.child = std::move(children.at(0));
    return copy;
}

WindowedFocalLogicalOperator WindowedFocalLogicalOperator::withChildren(std::vector<LogicalOperator> children) const
{
    PRECONDITION(children.size() == 1, "Can only set exactly one child for WindowedFocal, got {}", children.size());
    auto copy = *this;
    copy.child = std::move(children.at(0));
    copy.inferLocalSchema();
    return copy;
}

std::vector<LogicalOperator> WindowedFocalLogicalOperator::getChildren() const
{
    if (child.has_value()) { return {*child}; }
    return {};
}

LogicalOperator WindowedFocalLogicalOperator::getChild() const
{
    PRECONDITION(child.has_value(), "Child not set when trying to retrieve child");
    return child.value();
}

Schema<Field, Unordered> WindowedFocalLogicalOperator::getOutputSchema() const
{
    INVARIANT(outputSchema.has_value(), "Retrieving output schema before calling schema inference");
    return NES::bindToOperator(self.lock(), outputSchema.value());
}

Schema<Field, Ordered> WindowedFocalLogicalOperator::getOrderedOutputSchema(ChildOutputOrderProvider) const
{
    auto tileFields = std::array{tileXField, tileYField, payloadField}
        | std::views::transform([this](const auto& id) { return NES::bindToOperator(self.lock(), id); })
        | std::ranges::to<Schema<Field, Ordered>>();
    auto boundStartEnd = startEndFields | RangeBinder{self.lock()} | std::ranges::to<Schema<Field, Ordered>>();
    return std::array{std::move(tileFields), std::move(boundStartEnd)} | std::views::join
        | std::ranges::to<Schema<Field, Ordered>>();
}

std::unordered_map<Field, std::unordered_set<Field>> WindowedFocalLogicalOperator::getAccessedFieldsForOutput() const
{
    /*
    PRECONDITION(child.has_value(), "Child not set when retrieving accessed fields");
    const auto boundOutputSchema = getOutputSchema();
    const auto boundInputSchema = child->getOutputSchema();

    std::unordered_set<Field> accessedInput;
    for (const auto& id : {tileXField, tileYField, payloadField})
    {
        if (const auto f = boundInputSchema.getFieldByName(id); f.has_value())
        {
            accessedInput.insert(f.value());
        }
    }

    std::unordered_map<Field, std::unordered_set<Field>> result;
    for (const auto& outField : boundOutputSchema)   // Schema is iterable; adjust if your API differs
    {
        result[outField] = accessedInput;
    }
    return result;*/
    return {};
}

void WindowedFocalLogicalOperator::inferLocalSchema()
{
    PRECONDITION(child.has_value(), "Child not set when calling schema inference");
    const Schema<Field, Unordered>& inputSchema = child->getOutputSchema();

    /// Validate the three required input fields exist and capture their types.
    /// withInferredDataType binds the field against the input schema and throws if it's absent
    const auto tileXType = UnboundFieldAccessLogicalFunction{tileXField}
        .withInferredDataType(inputSchema).getAs<FieldAccessLogicalFunction>()->getDataType();
    const auto tileYType = UnboundFieldAccessLogicalFunction{tileYField}
        .withInferredDataType(inputSchema).getAs<FieldAccessLogicalFunction>()->getDataType();
    const auto payloadType = UnboundFieldAccessLogicalFunction{payloadField}
        .withInferredDataType(inputSchema).getAs<FieldAccessLogicalFunction>()->getDataType();

    timestampField = Windowing::TimeCharacteristicWrapper{std::move(timestampField)}.withInferredSchema(inputSchema);

    /// Output schema: [tileX, tileY, payload, start, end].
    std::vector<UnqualifiedUnboundField> outputFields{};
    outputFields.emplace_back(tileXField, tileXType);
    outputFields.emplace_back(tileYField, tileYType);
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
const FocalParameters& WindowedFocalLogicalOperator::getParameters() const { return params; }
const Identifier& WindowedFocalLogicalOperator::getTileXField() const { return tileXField; }
const Identifier& WindowedFocalLogicalOperator::getTileYField() const { return tileYField; }
const Identifier& WindowedFocalLogicalOperator::getPayloadField() const { return payloadField; }
Windowing::TimeBasedWindowType WindowedFocalLogicalOperator::getWindowType() const { return windowType; }
const UnqualifiedUnboundField& WindowedFocalLogicalOperator::getWindowStartField() const { return startEndFields[0]; }
const UnqualifiedUnboundField& WindowedFocalLogicalOperator::getWindowEndField() const { return startEndFields[1]; }

std::variant<Windowing::UnboundTimeCharacteristic, Windowing::BoundTimeCharacteristic>
WindowedFocalLogicalOperator::getCharacteristic() const
{
    return timestampField;
}

Reflected Reflector<TypedLogicalOperator<WindowedFocalLogicalOperator>>::operator()(
    const TypedLogicalOperator<WindowedFocalLogicalOperator>& op) const
{
    return reflect(detail::ReflectedWindowedFocalLogicalOperator{
        .operatorId     = op.getId(),
        .windowType     = op->getWindowType(),
        .tileXField     = op->getTileXField(),
        .tileYField     = op->getTileYField(),
        .payloadField   = op->getPayloadField(),
        .params         = op->getParameters(),
        .timestampField = op->getCharacteristic()});
}

Unreflector<TypedLogicalOperator<WindowedFocalLogicalOperator>>::Unreflector(ContextType plan) : plan(std::move(plan))
{
}

TypedLogicalOperator<WindowedFocalLogicalOperator> Unreflector<TypedLogicalOperator<WindowedFocalLogicalOperator>>::operator()(
    const Reflected& reflected, const ReflectionContext& context) const
{
    auto [id, windowType, tileXField, tileYField, payloadField, params, timestampField]
        = context.unreflect<detail::ReflectedWindowedFocalLogicalOperator>(reflected);
    auto children = plan->getChildrenFor(id, context);
    if (children.size() != 1)
    {
        throw CannotDeserialize("WindowedFocalLogicalOperator must have exactly one child, but got {}", children.size());
    }
    return WindowedFocalLogicalOperator::create(
        children.at(0), tileXField, tileYField, payloadField, params, windowType, timestampField);
}

}

std::size_t std::hash<NES::WindowedFocalLogicalOperator>::operator()(
    const NES::WindowedFocalLogicalOperator& op) const noexcept
{
    return folly::hash::hash_combine_generic(
        NES::Hash{},
        op.windowType,
        op.timestampField,
        op.tileXField,
        op.tileYField,
        op.payloadField,
        op.params.rasterWidth,
        op.params.rasterHeight,
        op.params.tileWidth,
        op.params.tileHeight,
        op.params.radius,
        static_cast<uint8_t>(op.params.reduction),
        static_cast<uint8_t>(op.params.boundary),
        op.params.noData);
}