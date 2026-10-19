// FocalKernels.cpp
#include <FocalKernels.hpp>
#include <Reducers.hpp>

namespace Raster
{
FocalStatus evaluateTile(const FocalSpec& spec, const FocalContext& ctx, double* out, uint64_t outCells) noexcept
{
    const auto& g = ctx.geo;
    if (g.tileW <= 0 || g.tileH <= 0 || g.rasterW <= 0 || g.rasterH <= 0)
    {
        return FocalStatus::BadGeometry;
    }
    if (outCells < static_cast<uint64_t>(g.tileW) * g.tileH)
    {
        return FocalStatus::OutputTooSmall;
    }
    switch (spec.kappa)
    {
        case ReductionOp::Mean: { MeanReducer red; evaluateInto(ctx, spec.n, spec.beta, red, out); break; }
        case ReductionOp::Sum:  { SumReducer  red; evaluateInto(ctx, spec.n, spec.beta, red, out); break; }
        case ReductionOp::Max:  { MaxReducer  red; evaluateInto(ctx, spec.n, spec.beta, red, out); break; }
        case ReductionOp::Min:  { MinReducer  red; evaluateInto(ctx, spec.n, spec.beta, red, out); break; }
    }
    return FocalStatus::Ok;
}
}  // namespace Raster