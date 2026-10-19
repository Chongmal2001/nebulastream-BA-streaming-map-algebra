#pragma once
#include <memory>
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
        PhysicalFunction payloadField);

    void setup(ExecutionContext& executionCtx, CompilationContext& compilationContext) const override;
    void execute(ExecutionContext& ctx, Record& record) const override;

private:
    PhysicalFunction tileXField;
    PhysicalFunction tileYField;
    PhysicalFunction payloadField;
};

}