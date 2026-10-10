#include <ultraviolent/arch/mips/processor_model.hpp>

#include <ultraviolent/arch/mips/decoded.hpp>

#include <algorithm>
#include <span>

namespace ultraviolent::mips {

namespace {

// The MIPS IV encodings, less the operations of levels the model does not implement.
std::array<DecodedOpcode, decode_table_size> operations_for(const ProcessorModel& model) {
    std::array<DecodedOpcode, decode_table_size> table = detail::mips_iv_operations;
    const auto remove = [&](std::span<const DecodedOpcode> operations) {
        for (DecodedOpcode& op : table) {
            if (std::ranges::find(operations, op) != operations.end()) {
                op = DecodedOpcode::reserved;
            }
        }
    };
    if (!model.supports(Feature::mips3)) {
        remove(mips3_operations);
    }
    if (!model.supports(Feature::mips4)) {
        remove(mips4_operations);
    }
    return table;
}

} // namespace

ProcessorModel r10000(const CpuConfig& identity) {
    ProcessorModel model;
    model.name_ = "R10000";
    model.identity_ = identity;
    model.features_ = 1u << static_cast<unsigned>(Feature::mips3) |
                      1u << static_cast<unsigned>(Feature::mips4);
    model.count_shift_ = 1; // Count every other cycle (UM 14.8).
    model.operations_ = operations_for(model);
    return model;
}

} // namespace ultraviolent::mips
