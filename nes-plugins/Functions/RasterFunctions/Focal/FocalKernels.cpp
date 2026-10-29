#include <FocalKernels.hpp>
#include <MapletCodec.hpp>
#include <Reducers.hpp>

namespace Raster
{
FocalStatus evaluateTile(const FocalSpec& spec, const FocalContext& ctx, double* out, uint64_t outCells, uint32_t bands) noexcept
{
    const auto& g = ctx.geo;
    if (g.tileW <= 0 || g.tileH <= 0 || g.rasterW <= 0 || g.rasterH <= 0)
    {
        return FocalStatus::BadGeometry;
    }
    if (bands == 0 || bands > MAX_BANDS)
    {
        return FocalStatus::BadBands;
    }
    if (outCells < static_cast<uint64_t>(bands) * static_cast<uint64_t>(g.tileW) * static_cast<uint64_t>(g.tileH))
    {
        return FocalStatus::OutputTooSmall;
    }
    if ((spec.kappa == ReductionOp::Mean || spec.kappa == ReductionOp::Sum) && spec.summation != Summation::Exact)
    {
        if (const int32_t reach = denseSquareRadius(spec.n); reach >= 0)
        {
            const bool mean = spec.kappa == ReductionOp::Mean;
            const bool kahan = spec.summation == Summation::Kahan;
            if (mean && kahan) evaluateRunningBands<true, true>(ctx, bands, reach, spec.beta, out);
            else if (mean) evaluateRunningBands<true, false>(ctx, bands, reach, spec.beta, out);
            else if (kahan) evaluateRunningBands<false, true>(ctx, bands, reach, spec.beta, out);
            else evaluateRunningBands<false, false>(ctx, bands, reach, spec.beta, out);
            return FocalStatus::Ok;
        }
    }
    switch (spec.kappa)
    {
        case ReductionOp::Mean: { MeanReducer red; evaluateBands(ctx, bands, spec.n, spec.beta, red, out); break; }
        case ReductionOp::Sum:  { SumReducer  red; evaluateBands(ctx, bands, spec.n, spec.beta, red, out); break; }
        case ReductionOp::Max:  { MaxReducer  red; evaluateBands(ctx, bands, spec.n, spec.beta, red, out); break; }
        case ReductionOp::Min:  { MinReducer  red; evaluateBands(ctx, bands, spec.n, spec.beta, red, out); break; }
    }
    return FocalStatus::Ok;
}
}