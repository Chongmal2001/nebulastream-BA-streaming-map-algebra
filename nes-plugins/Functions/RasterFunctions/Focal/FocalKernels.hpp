
#pragma once
#include <cstdint>
#include <FocalDriver.hpp>
#include <FocalSpec.hpp>

namespace Raster
{
enum class FocalStatus : int32_t { Ok = 0, OutputTooSmall = -1, BadGeometry = -2, BadBands = -3 };


/// Focal operation for all bands, band-major output
FocalStatus evaluateTile(const FocalSpec& spec, const FocalContext& ctx, double* out, uint64_t outCells, uint32_t bands = 1) noexcept;
}