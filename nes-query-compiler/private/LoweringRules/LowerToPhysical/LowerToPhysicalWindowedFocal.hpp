#pragma once

#include <utility>
#include <LoweringRules/AbstractLoweringRule.hpp>
#include <Operators/LogicalOperator.hpp>
#include <QueryExecutionConfiguration.hpp>

namespace NES
{

struct LowerToPhysicalFocal : AbstractLoweringRule
{
    explicit LowerToPhysicalFocal(QueryExecutionConfiguration conf) : conf(std::move(conf)) { }

    LoweringRuleResultSubgraph apply(LogicalOperator logicalOperator) override;

private:
    QueryExecutionConfiguration conf;
};

}