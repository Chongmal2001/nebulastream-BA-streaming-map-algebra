#include <Interface/Record.hpp>
#include <Time/Timestamp.hpp>
#include <ExecutionContext.hpp>
#include <WindowBuildPhysicalOperator.hpp>
#include <function.hpp>
#include <val_ptr.hpp>
#include <Focal/FocalOperatorHandler.hpp>
#include <Focal/FocalBuildPhysicalOperator.hpp>

namespace NES
{
void FocalBuildPhysicalOperator::setup(ExecutionContext& executionCtx, CompilationContext& compilationContext) const
{
    WindowBuildPhysicalOperator::setup(executionCtx, compilationContext);
}
void FocalBuildPhysicalOperator::execute(ExecutionContext& ctx, Record& record) const
{
    /// Handler from local state
    auto* const localState = dynamic_cast<WindowOperatorBuildLocalState*>(ctx.getLocalState(id));
    auto operatorHandler = localState->getOperatorHandler();

    /// Event time via inherited time function
    const auto timestamp = timeFunction->getTs(ctx, record);

    /// Extract the three focal fields in traced code
    const auto tileX = tileXField.execute(record, ctx.pipelineMemoryProvider.arena);
    const auto tileY = tileYField.execute(record, ctx.pipelineMemoryProvider.arena);

    const auto payloadVal = payloadField.execute(record, ctx.pipelineMemoryProvider.arena);
    const auto payloadVarSized = payloadVal.getRawValueAs<VariableSizedData>();
    const auto payloadData = payloadVarSized.getContent();
    const auto payloadLen = payloadVarSized.getSize();

    /// population of FocalWindowState outsourced to operator handler
    nautilus::invoke(
        +[](OperatorHandler* handlerPtr,
            int32_t tx,
            int32_t ty,
            uint64_t ts,
            const int8_t* payloadData,
            uint64_t payloadLen,
            PipelineExecutionContext* pipelineCtx) noexcept
        {
            auto* focalHandler = static_cast<FocalOperatorHandler*>(handlerPtr);
            focalHandler->onTileArrival(tx, ty, Timestamp(ts), payloadData, payloadLen, pipelineCtx);
        },
        operatorHandler,
        tileX.getRawValueAs<nautilus::val<int32_t>>(),
        tileY.getRawValueAs<nautilus::val<int32_t>>(),
        timestamp.convertToValue(),
        payloadData,
        payloadLen,
        ctx.pipelineContext);
}

FocalBuildPhysicalOperator::FocalBuildPhysicalOperator(
    const OperatorHandlerId operatorHandlerId,
    std::unique_ptr<TimeFunction> timeFunction,
    std::unique_ptr<SliceStoreRef> sliceStoreRef,
    PhysicalFunction tileXField,
    PhysicalFunction tileYField,
    PhysicalFunction payloadField)
    : WindowBuildPhysicalOperator(operatorHandlerId, std::move(timeFunction), std::move(sliceStoreRef))
    , tileXField(std::move(tileXField))
    , tileYField(std::move(tileYField))
    , payloadField(std::move(payloadField))
{
}

}