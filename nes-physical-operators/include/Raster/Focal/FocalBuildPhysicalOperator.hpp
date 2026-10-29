#pragma once
#include <memory>
#include <optional>
#include <Watermark/TimeFunction.hpp>
#include <Functions/PhysicalFunction.hpp>
#include <WindowBuildPhysicalOperator.hpp>

namespace NES
{

class FocalBuildPhysicalOperator final : public WindowBuildPhysicalOperator
{
public:
    FocalBuildPhysicalOperator(
        OperatorHandlerId operatorHandlerId,
        std::unique_ptr<TimeFunction> timeFunction,
        std::unique_ptr<SliceStoreRef> sliceStoreRef,
        PhysicalFunction tileXField,
        PhysicalFunction tileYField,
        PhysicalFunction payloadField,
        std::optional<PhysicalFunction> srcIdField = std::nullopt,
        std::optional<PhysicalFunction> srcFrameIdField = std::nullopt);

    void setup(ExecutionContext& executionCtx, CompilationContext& compilationContext) const override;
    void execute(ExecutionContext& ctx, Record& record) const override;

private:
    PhysicalFunction tileXField;
    PhysicalFunction tileYField;
    PhysicalFunction payloadField;
    std::optional<PhysicalFunction> srcIdField;        // lineage (UINT32), absent if the input has no such field
    std::optional<PhysicalFunction> srcFrameIdField;   // lineage (UINT32), absent if the input has no such field
};

}