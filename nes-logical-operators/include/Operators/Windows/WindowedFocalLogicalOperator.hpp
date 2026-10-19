#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <DataTypes/DataType.hpp>
#include <DataTypes/UnboundField.hpp>
#include <Identifiers/Identifier.hpp>
#include <Identifiers/Identifiers.hpp>
#include <Operators/LogicalOperator.hpp>
#include <Operators/LogicalOperatorFwd.hpp>
#include <Operators/OriginIdAssigner.hpp>
#include <Operators/Reorderer.hpp>
#include <Operators/Reprojecter.hpp>
#include <Schema/Field.hpp>
#include <Serialization/ReflectedOperator.hpp>
#include <Traits/Trait.hpp>
#include <Traits/TraitSet.hpp>
#include <Util/DynamicBase.hpp>
#include <Util/PlanRenderer.hpp>
#include <Util/Reflection.hpp>
#include <WindowTypes/Measures/TimeCharacteristic.hpp>
#include <WindowTypes/Types/TimeBasedWindowType.hpp>

namespace NES
{


enum class FocalReductionOp : uint8_t { Mean, Max, Min, Sum };
enum class FocalBoundaryRule : uint8_t { NoData, Clamp, Reflect };
enum class FocalNeighbourhoodShape: uint8_t { Square, Disc };

struct FocalParameters
{
    int64_t rasterWidth  = 0;
    int64_t rasterHeight = 0;
    int32_t tileWidth    = 0;
    int32_t tileHeight   = 0;
    int32_t radius       = 0;
    FocalNeighbourhoodShape neighbourhood = FocalNeighbourhoodShape::Square;
    FocalReductionOp  reduction = FocalReductionOp::Mean;
    FocalBoundaryRule boundary  = FocalBoundaryRule::NoData;
    double  noData = 0.0;

    friend bool operator==(const FocalParameters& lhs, const FocalParameters& rhs);
};

class WindowedFocalLogicalOperator final : public OriginIdAssigner, public Reprojecter, public Reorderer, public ManagedByOperator
{
public:
    WindowedFocalLogicalOperator(
        WeakLogicalOperator self,
        Identifier tileXField,
        Identifier tileYField,
        Identifier payloadField,
        FocalParameters params,
        Windowing::TimeBasedWindowType windowType,
        Windowing::TimeCharacteristic timeCharacteristic);

    WindowedFocalLogicalOperator(
        WeakLogicalOperator self,
        LogicalOperator child,
        Identifier tileXField,
        Identifier tileYField,
        Identifier payloadField,
        FocalParameters params,
        Windowing::TimeBasedWindowType windowType,
        Windowing::TimeCharacteristic timeCharacteristic);

    static TypedLogicalOperator<WindowedFocalLogicalOperator> create(
        Identifier tileXField,
        Identifier tileYField,
        Identifier payloadField,
        FocalParameters params,
        Windowing::TimeBasedWindowType windowType,
        Windowing::TimeCharacteristic timeCharacteristic);

    static TypedLogicalOperator<WindowedFocalLogicalOperator> create(
        LogicalOperator child,
        Identifier tileXField,
        Identifier tileYField,
        Identifier payloadField,
        FocalParameters params,
        Windowing::TimeBasedWindowType windowType,
        Windowing::TimeCharacteristic timeCharacteristic);

    /// Focal-specific accessors (read by the lowering rule)
    [[nodiscard]] const FocalParameters& getParameters() const;
    [[nodiscard]] const Identifier& getTileXField() const;
    [[nodiscard]] const Identifier& getTileYField() const;
    [[nodiscard]] const Identifier& getPayloadField() const;
    [[nodiscard]] Windowing::TimeBasedWindowType getWindowType() const;
    [[nodiscard]] const UnqualifiedUnboundField& getWindowStartField() const;
    [[nodiscard]] const UnqualifiedUnboundField& getWindowEndField() const;
    [[nodiscard]] std::variant<Windowing::UnboundTimeCharacteristic, Windowing::BoundTimeCharacteristic> getCharacteristic() const;

    [[nodiscard]] bool operator==(const WindowedFocalLogicalOperator& rhs) const;

    /// LogicalOperator concept surface (mirrors WindowedAggregationLogicalOperator)
    [[nodiscard]] WindowedFocalLogicalOperator withTraitSet(TraitSet traitSet) const;
    [[nodiscard]] TraitSet getTraitSet() const;

    [[nodiscard]] WindowedFocalLogicalOperator withChildrenUnsafe(std::vector<LogicalOperator> children) const;
    [[nodiscard]] WindowedFocalLogicalOperator withChildren(std::vector<LogicalOperator> children) const;
    [[nodiscard]] std::vector<LogicalOperator> getChildren() const;
    [[nodiscard]] LogicalOperator getChild() const;

    [[nodiscard]] Schema<Field, Unordered> getOutputSchema() const;

    [[nodiscard]] std::string explain(ExplainVerbosity verbosity, OperatorId) const;
    [[nodiscard]] std::string_view getName() const noexcept;

    [[nodiscard]] WindowedFocalLogicalOperator withInferredSchema() const;

    [[nodiscard]] std::unordered_map<Field, std::unordered_set<Field>> getAccessedFieldsForOutput() const override;
    [[nodiscard]] Schema<Field, Ordered> getOrderedOutputSchema(ChildOutputOrderProvider orderProvider) const override;
    [[nodiscard]] const DynamicBase* getDynamicBase() const;

private:
    static constexpr std::string_view NAME = "WindowedFocal";

    std::optional<LogicalOperator> child;
    Windowing::TimeBasedWindowType windowType;

    Identifier tileXField;
    Identifier tileYField;
    Identifier payloadField;
    FocalParameters params;

    void inferLocalSchema();
    std::optional<Schema<UnqualifiedUnboundField, Unordered>> outputSchema;   // set during inference
    Windowing::TimeCharacteristic timestampField;

    std::array<UnqualifiedUnboundField, 2> startEndFields = std::array{
        UnqualifiedUnboundField{Identifier::parse("start"), DataType::Type::UINT64},
        UnqualifiedUnboundField{Identifier::parse("end"), DataType::Type::UINT64}};

    TraitSet traitSet;

    friend struct std::hash<WindowedFocalLogicalOperator>;
    friend Reflector<TypedLogicalOperator<WindowedFocalLogicalOperator>>;
};

template <>
struct Reflector<TypedLogicalOperator<WindowedFocalLogicalOperator>>
{
    Reflected operator()(const TypedLogicalOperator<WindowedFocalLogicalOperator>& op) const;
};

template <>
struct Unreflector<TypedLogicalOperator<WindowedFocalLogicalOperator>>
{
    using ContextType = std::shared_ptr<ReflectedPlan>;
    ContextType plan;
    explicit Unreflector(ContextType plan);
    TypedLogicalOperator<WindowedFocalLogicalOperator> operator()(const Reflected& reflected, const ReflectionContext& context) const;
};

static_assert(LogicalOperatorConcept<WindowedFocalLogicalOperator>);

}

template <>
struct std::hash<NES::WindowedFocalLogicalOperator>
{
    std::size_t operator()(const NES::WindowedFocalLogicalOperator& op) const noexcept;
};

namespace NES::detail
{
struct ReflectedWindowedFocalLogicalOperator
{
    OperatorId operatorId{OperatorId::INVALID};
    Windowing::TimeBasedWindowType windowType;
    Identifier tileXField;
    Identifier tileYField;
    Identifier payloadField;
    FocalParameters params;
    Windowing::TimeCharacteristic timestampField;
};
}