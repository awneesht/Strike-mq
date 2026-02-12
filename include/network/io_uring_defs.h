#pragma once
#include <cstdint>

namespace strike {
namespace network {

/// io_uring operation types for user_data encoding.
enum class UringOp : uint8_t {
    Accept  = 0,
    Recv    = 1,
    Send    = 2,
    Close   = 3,
};

/// Pack operation type + file descriptor into 64-bit user_data.
/// Layout: [op:8][reserved:24][fd:32]
inline uint64_t encode_userdata(UringOp op, int fd) noexcept {
    return (static_cast<uint64_t>(op) << 56) |
           (static_cast<uint64_t>(static_cast<uint32_t>(fd)));
}

/// Extract operation type from user_data.
inline UringOp decode_op(uint64_t userdata) noexcept {
    return static_cast<UringOp>(userdata >> 56);
}

/// Extract file descriptor from user_data.
inline int decode_fd(uint64_t userdata) noexcept {
    return static_cast<int>(static_cast<uint32_t>(userdata));
}

} // namespace network
} // namespace strike
