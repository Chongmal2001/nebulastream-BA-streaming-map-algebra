#include <BinaryOutputFormatter.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Configurations/Descriptor.hpp>
#include <DataTypes/DataType.hpp>
#include <DataTypes/VarVal.hpp>
#include <DataTypes/VariableSizedData.hpp>
#include <Interface/Record.hpp>
#include <Interface/RecordBuffer.hpp>
#include <OutputFormatters/OutputFormatter.hpp>
#include <Runtime/AbstractBufferProvider.hpp>
#include <Runtime/TupleBuffer.hpp>
#include <Runtime/VariableSizedAccess.hpp>
#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>
#include <ErrorHandling.hpp>
#include <OutputFormatterRegistry.hpp>
#include <OutputFormatterValidationRegistry.hpp>
#include <function.hpp>
#include <static.hpp>
#include <val_arith.hpp>
#include <val_concepts.hpp>
#include <val_ptr.hpp>

namespace NES
{
namespace
{
static_assert(std::endian::native == std::endian::little, "BinaryOutputFormatter writes host byte order and promises little-endian");

/// writeValueToBuffer with explicit length (binary-safe), same placement; returns bytes written to the main buffer
uint64_t writeBytes(
    const int8_t* data, uint64_t size, uint64_t remainingSpace, TupleBuffer* tupleBuffer, AbstractBufferProvider* bufferProvider, int8_t* address)
{
    uint64_t remainingBytes = size;
    uint32_t numOfChildBuffers = tupleBuffer->getNumberOfChildBuffers();
    uint64_t writtenToMainMemory = 0;
    if (numOfChildBuffers == 0)
    {
        const uint64_t fitsInMainBuffer = std::min(remainingBytes, remainingSpace);
        std::memcpy(address, data, fitsInMainBuffer);
        writtenToMainMemory = fitsInMainBuffer;
        remainingBytes -= fitsInMainBuffer;
        if (remainingBytes > 0)
        {
            auto child = bufferProvider->getBufferBlocking();
            (void)tupleBuffer->storeChildBuffer(child);
            ++numOfChildBuffers;
        }
    }
    while (remainingBytes > 0)
    {
        auto lastChild = tupleBuffer->loadChildBuffer(VariableSizedAccess::Index{numOfChildBuffers - 1});
        const uint64_t used = lastChild.getNumberOfTuples();
        const uint64_t writable = std::min(remainingBytes, lastChild.getBufferSize() - used);
        std::memcpy(lastChild.getAvailableMemoryArea<int8_t>().data() + used, data + (size - remainingBytes), writable);
        lastChild.setNumberOfTuples(used + writable);
        remainingBytes -= writable;
        if (remainingBytes > 0)
        {
            auto child = bufferProvider->getBufferBlocking();
            (void)tupleBuffer->storeChildBuffer(child);
            ++numOfChildBuffers;
        }
    }
    return writtenToMainMemory;
}

/// One fixed-width value as its little-endian bytes
template <typename T>
uint64_t writeFixed(T value, uint64_t remainingSpace, TupleBuffer* tupleBuffer, AbstractBufferProvider* bufferProvider, int8_t* address)
{
    std::array<int8_t, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(T));
    return writeBytes(bytes.data(), bytes.size(), remainingSpace, tupleBuffer, bufferProvider, address);
}

/// A VARSIZED value as u32 length + bytes
uint64_t writeVarSized(
    const int8_t* content, uint64_t size, uint64_t remainingSpace, TupleBuffer* tupleBuffer, AbstractBufferProvider* bufferProvider, int8_t* address)
{
    if (size > std::numeric_limits<uint32_t>::max())
    {
        throw FormattingError("Binary output: a VARSIZED value of {} bytes exceeds the u32 length prefix", size);
    }
    const auto length = static_cast<uint32_t>(size);
    const auto lengthWritten = writeFixed<uint32_t>(length, remainingSpace, tupleBuffer, bufferProvider, address);
    return lengthWritten
        + writeBytes(content, size, remainingSpace - lengthWritten, tupleBuffer, bufferProvider, address + lengthWritten);
}

template <typename T>
nautilus::val<uint64_t> invokeWriteFixed(
    const VarVal& value,
    const nautilus::val<int8_t*>& address,
    const nautilus::val<uint64_t>& remainingSize,
    const RecordBuffer& recordBuffer,
    const nautilus::val<AbstractBufferProvider*>& bufferProvider)
{
    return nautilus::invoke(
        writeFixed<T>, value.getRawValueAs<nautilus::val<T>>(), remainingSize, recordBuffer.getReference(), bufferProvider, address);
}
}

BinaryOutputFormatter::BinaryOutputFormatter(const std::vector<Record::RecordFieldIdentifier>& fieldNames) : OutputFormatter(fieldNames)
{
}

nautilus::val<uint64_t> BinaryOutputFormatter::writeFormattedValue(
    const VarVal& value,
    const DataType& fieldType,
    const nautilus::static_val<uint64_t>&,
    const nautilus::val<int8_t*>& fieldPointer,
    const nautilus::val<uint64_t>& remainingSize,
    const RecordBuffer& recordBuffer,
    const nautilus::val<AbstractBufferProvider*>& bufferProvider) const
{
    if (fieldType.nullable)
    {
        throw UnknownDataType("Binary output format does not support nullable fields (sink schema field {} is nullable)", fieldType);
    }
    switch (fieldType.type)
    {
        case DataType::Type::VARSIZED: {
            const auto varSized = value.getRawValueAs<VariableSizedData>();
            return nautilus::invoke(
                writeVarSized,
                varSized.getContent(),
                varSized.getSize(),
                remainingSize,
                recordBuffer.getReference(),
                bufferProvider,
                fieldPointer);
        }
        case DataType::Type::INT8:
            return invokeWriteFixed<int8_t>(value, fieldPointer, remainingSize, recordBuffer, bufferProvider);
        case DataType::Type::INT16:
            return invokeWriteFixed<int16_t>(value, fieldPointer, remainingSize, recordBuffer, bufferProvider);
        case DataType::Type::INT32:
            return invokeWriteFixed<int32_t>(value, fieldPointer, remainingSize, recordBuffer, bufferProvider);
        case DataType::Type::INT64:
            return invokeWriteFixed<int64_t>(value, fieldPointer, remainingSize, recordBuffer, bufferProvider);
        case DataType::Type::UINT8:
            return invokeWriteFixed<uint8_t>(value, fieldPointer, remainingSize, recordBuffer, bufferProvider);
        case DataType::Type::UINT16:
            return invokeWriteFixed<uint16_t>(value, fieldPointer, remainingSize, recordBuffer, bufferProvider);
        case DataType::Type::UINT32:
            return invokeWriteFixed<uint32_t>(value, fieldPointer, remainingSize, recordBuffer, bufferProvider);
        case DataType::Type::UINT64:
            return invokeWriteFixed<uint64_t>(value, fieldPointer, remainingSize, recordBuffer, bufferProvider);
        case DataType::Type::FLOAT32:
            return invokeWriteFixed<float>(value, fieldPointer, remainingSize, recordBuffer, bufferProvider);
        case DataType::Type::FLOAT64:
            return invokeWriteFixed<double>(value, fieldPointer, remainingSize, recordBuffer, bufferProvider);
        case DataType::Type::BOOLEAN:
            return invokeWriteFixed<bool>(value, fieldPointer, remainingSize, recordBuffer, bufferProvider);
        case DataType::Type::CHAR:
            return invokeWriteFixed<char>(value, fieldPointer, remainingSize, recordBuffer, bufferProvider);
        case DataType::Type::UNDEFINED:
            break;
    }
    throw UnknownDataType("Binary output format does not support type {}", magic_enum::enum_name(fieldType.type));
}

DescriptorConfig::Config BinaryOutputFormatter::validateAndFormat(std::unordered_map<std::string, std::string> config)
{
    return DescriptorConfig::validateAndFormat<OutputFormatterConfig::ConfigParametersBinary>(std::move(config), "Binary");
}

std::ostream& operator<<(std::ostream& out, const BinaryOutputFormatter&)
{
    return out << fmt::format("BinaryOutputFormatter()");
}

OutputFormatterValidationRegistryReturnType
OutputFormatterValidationGeneratedRegistrar::RegisterBinaryOutputFormatterValidation(OutputFormatterValidationRegistryArguments args)
{
    return BinaryOutputFormatter::validateAndFormat(std::move(args.config));
}

OutputFormatterRegistryReturnType OutputFormatterGeneratedRegistrar::RegisterBinaryOutputFormatter(OutputFormatterRegistryArguments args)
{
    return std::make_unique<BinaryOutputFormatter>(std::move(args.fieldNames));
}

}
