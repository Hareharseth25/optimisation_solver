#include "milp/basis_state.h"

#include <cstring>
#include <stdexcept>

namespace milp {

BasisState BasisState::clone() const { return *this; }

std::vector<std::uint8_t> BasisState::serialize() const {
    const std::uint64_t variableCount = variableStatus.size();
    const std::uint64_t constraintCount = constraintStatus.size();

    std::vector<std::uint8_t> bytes(
        sizeof(variableCount) + sizeof(constraintCount) + variableCount + constraintCount);

    std::size_t offset = 0;
    std::memcpy(bytes.data() + offset, &variableCount, sizeof(variableCount));
    offset += sizeof(variableCount);
    std::memcpy(bytes.data() + offset, &constraintCount, sizeof(constraintCount));
    offset += sizeof(constraintCount);

    for (std::uint64_t i = 0; i < variableCount; ++i) {
        bytes[offset++] = static_cast<std::uint8_t>(variableStatus[i]);
    }
    for (std::uint64_t i = 0; i < constraintCount; ++i) {
        bytes[offset++] = static_cast<std::uint8_t>(constraintStatus[i]);
    }

    return bytes;
}

BasisState BasisState::deserialize(const std::vector<std::uint8_t> &bytes) {
    constexpr std::size_t headerSize = sizeof(std::uint64_t) * 2;
    if (bytes.size() < headerSize) {
        throw std::invalid_argument("BasisState::deserialize: truncated header");
    }

    std::uint64_t variableCount = 0;
    std::uint64_t constraintCount = 0;
    std::size_t offset = 0;
    std::memcpy(&variableCount, bytes.data() + offset, sizeof(variableCount));
    offset += sizeof(variableCount);
    std::memcpy(&constraintCount, bytes.data() + offset, sizeof(constraintCount));
    offset += sizeof(constraintCount);

    if (bytes.size() != headerSize + variableCount + constraintCount) {
        throw std::invalid_argument("BasisState::deserialize: size does not match header");
    }

    BasisState state;
    state.variableStatus.resize(variableCount);
    state.constraintStatus.resize(constraintCount);

    for (std::uint64_t i = 0; i < variableCount; ++i) {
        state.variableStatus[i] = static_cast<BasisStatus>(bytes[offset++]);
    }
    for (std::uint64_t i = 0; i < constraintCount; ++i) {
        state.constraintStatus[i] = static_cast<BasisStatus>(bytes[offset++]);
    }

    return state;
}

bool operator==(const BasisState &a, const BasisState &b) {
    return a.variableStatus == b.variableStatus && a.constraintStatus == b.constraintStatus;
}

} // namespace milp
