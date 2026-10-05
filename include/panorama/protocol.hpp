#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "picojson.h"

namespace panorama {

// Sentinel bytes used to delimit and protect frame boundaries
constexpr uint8_t FRAME_MARKER = 0x5A;
constexpr uint8_t ESCAPE_MARKER = 0x5B;

// Holds the decoded contents of a protocol response
struct Response {
  std::string raw;
  std::string body;
  std::optional<picojson::value> json;
  std::string version;
  std::string status;
};

// Produce a single-byte checksum by summing all elements modulo 256
uint8_t calculate_crc(const std::vector<uint8_t>& data);

// Replace occurrences of sentinel bytes with two-byte escape sequences
std::vector<uint8_t> escape_data(const std::vector<uint8_t>& data);

// Reverse the escape encoding, restoring original byte values
std::vector<uint8_t> unescape_data(const std::vector<uint8_t>& data);

// Assemble a framed protocol packet ready for transmission
std::vector<uint8_t> build_frame(const std::string& request_state,
                                 const std::string& cmd_type,
                                 const std::string& content = "",
                                 const std::string& version = "1",
                                 int ack_number = 0);

// Decode a raw frame into a structured Response, if valid
std::optional<Response> parse_response(const std::vector<uint8_t>& data);

}  // namespace panorama
