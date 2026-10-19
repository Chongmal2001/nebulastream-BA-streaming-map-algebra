
#pragma once
#include <cstdint>
#include <FocalDriver.hpp>
#include <FocalSpec.hpp>

namespace Raster
{
enum class FocalStatus : int32_t { Ok = 0, OutputTooSmall = -1, BadGeometry = -2 };

/// Computes the output tile as specified by spec into "out"
FocalStatus evaluateTile(const FocalSpec& spec, const FocalContext& ctx, double* out, uint64_t outCells) noexcept;
}