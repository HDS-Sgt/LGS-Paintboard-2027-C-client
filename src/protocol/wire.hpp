#pragma once

#include "paintboard/types.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace paintboard::protocol {

Rules parse_rules(std::span<const std::uint8_t> payload);
void parse_tick(std::span<const std::uint8_t> payload, TickEvent& out);

std::vector<std::uint8_t> encode_batch(std::span<const Bind> binds,
                                       std::span<const Paint> paints);

std::vector<std::uint8_t> zstd_decode_one(std::span<const std::uint8_t> wire);
std::vector<std::uint8_t> zstd_encode(std::span<const std::uint8_t> payload);

} // namespace paintboard::protocol
