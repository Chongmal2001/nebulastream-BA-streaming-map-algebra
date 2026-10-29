#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <Configurations/Descriptor.hpp>
#include <DataTypes/DataType.hpp>
#include <DataTypes/VarVal.hpp>
#include <Interface/Record.hpp>
#include <Interface/RecordBuffer.hpp>
#include <OutputFormatters/OutputFormatter.hpp>
#include <Runtime/AbstractBufferProvider.hpp>
#include <Util/Logger/Formatter.hpp>
#include <static.hpp>
#include <val_arith.hpp>
#include <val_concepts.hpp>
#include <val_ptr.hpp>

namespace NES
{

/// One binary record per tuple: fields in schema order, VARSIZED as u32 length + bytes, no nullable fields
class BinaryOutputFormatter : public OutputFormatter
{
public:
    explicit BinaryOutputFormatter(const std::vector<Record::RecordFieldIdentifier>& fieldNames);

    [[nodiscard]] nautilus::val<uint64_t> writeFormattedValue(
        const VarVal& value,
        const DataType& fieldType,
        const nautilus::static_val<uint64_t>& fieldIndex,
        const nautilus::val<int8_t*>& fieldPointer,
        const nautilus::val<uint64_t>& remainingSize,
        const RecordBuffer& recordBuffer,
        const nautilus::val<AbstractBufferProvider*>& bufferProvider) const override;

    static DescriptorConfig::Config validateAndFormat(std::unordered_map<std::string, std::string> config);

    std::ostream& toString(std::ostream& os) const override { return os << *this; }
    friend std::ostream& operator<<(std::ostream& out, const BinaryOutputFormatter& format);
};

}

namespace NES::OutputFormatterConfig
{
struct ConfigParametersBinary
{
    static inline std::unordered_map<std::string, DescriptorConfig::ConfigParameterContainer> parameterMap
        = DescriptorConfig::createConfigParameterContainerMap();
};
}
