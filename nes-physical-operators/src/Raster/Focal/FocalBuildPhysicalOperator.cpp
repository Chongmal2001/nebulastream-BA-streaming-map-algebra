#include <Interface/Record.hpp>
#include <Time/Timestamp.hpp>
#include <ExecutionContext.hpp>
#include <WindowBuildPhysicalOperator.hpp>
#include <function.hpp>
#include <val_ptr.hpp>
#include <Raster/Focal/FocalOperatorHandler.hpp>
#include <Raster/Focal/FocalBuildPhysicalOperator.hpp>

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

    /// Lineage fields if the input carries them, else 0 (unknown)
    nautilus::val<uint32_t> srcId = 0;
    nautilus::val<uint32_t> srcFrameId = 0;
    if (srcIdField && srcFrameIdField)
    {
        srcId = srcIdField->execute(record, ctx.pipelineMemoryProvider.arena).getRawValueAs<nautilus::val<uint32_t>>();
        srcFrameId = srcFrameIdField->execute(record, ctx.pipelineMemoryProvider.arena).getRawValueAs<nautilus::val<uint32_t>>();
    }

    /// Handler updates state and emits; may throw on invalid input
    nautilus::invoke(
        +[](OperatorHandler* handlerPtr,
            OriginId originId,
            int32_t tx,
            int32_t ty,
            uint64_t ts,
            uint32_t srcId,
            uint32_t srcFrameId,
            const int8_t* payloadData,
            uint64_t payloadLen,
            PipelineExecutionContext* pipelineCtx)
        {
            auto* focalHandler = static_cast<FocalOperatorHandler*>(handlerPtr);
            focalHandler->onTileArrival(
                originId, tx, ty, Timestamp(ts), FrameLineage{srcId, srcFrameId}, payloadData, payloadLen, pipelineCtx);
        },
        operatorHandler,
        ctx.originId,
        tileX.getRawValueAs<nautilus::val<int32_t>>(),
        tileY.getRawValueAs<nautilus::val<int32_t>>(),
        timestamp.convertToValue(),
        srcId,
        srcFrameId,
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
    PhysicalFunction payloadField,
    std::optional<PhysicalFunction> srcIdField,
    std::optional<PhysicalFunction> srcFrameIdField)
    : WindowBuildPhysicalOperator(operatorHandlerId, std::move(timeFunction), std::move(sliceStoreRef))
    , tileXField(std::move(tileXField))
    , tileYField(std::move(tileYField))
    , payloadField(std::move(payloadField))
    , srcIdField(std::move(srcIdField))
    , srcFrameIdField(std::move(srcFrameIdField))
{
}

}