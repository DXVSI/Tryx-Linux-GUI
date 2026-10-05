#include "panorama/protocol.hpp"

#include <algorithm>
#include <numeric>
#include <sstream>

namespace panorama {

uint8_t calculate_crc(const std::vector<uint8_t>& data) {
  // Accumulate all byte values, then truncate to the lowest 8 bits
  unsigned int total = std::accumulate(data.begin(), data.end(), 0u);
  return static_cast<uint8_t>(total & 0xFF);
}

std::vector<uint8_t> escape_data(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> output;
  output.reserve(data.size() * 2);  // worst case: every byte needs escaping

  for (auto it = data.begin(); it != data.end(); ++it) {
    switch (*it) {
      case FRAME_MARKER:
        output.push_back(ESCAPE_MARKER);
        output.push_back(0x01);
        break;
      case ESCAPE_MARKER:
        output.push_back(ESCAPE_MARKER);
        output.push_back(0x02);
        break;
      default:
        output.push_back(*it);
        break;
    }
  }

  return output;
}

std::vector<uint8_t> unescape_data(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> output;
  output.reserve(data.size());

  size_t pos = 0;
  while (pos < data.size()) {
    if (data[pos] == ESCAPE_MARKER && pos + 1 < data.size()) {
      uint8_t next_byte = data[pos + 1];
      if (next_byte == 0x01) {
        output.push_back(FRAME_MARKER);
        pos += 2;
        continue;
      }
      if (next_byte == 0x02) {
        output.push_back(ESCAPE_MARKER);
        pos += 2;
        continue;
      }
    }
    output.push_back(data[pos]);
    ++pos;
  }

  return output;
}

std::vector<uint8_t> build_frame(const std::string& request_state,
                                 const std::string& cmd_type,
                                 const std::string& content,
                                 const std::string& version, int ack_number) {
  // Compose the text portion: request line, headers, blank line, payload
  std::string request_line =
      request_state + " " + cmd_type + " " + version + "\r\n";

  std::string headers;
  headers += "ContentType=json\r\n";
  headers += "ContentLength=" + std::to_string(content.size()) + "\r\n";
  headers += "AckNumber=" + std::to_string(ack_number) + "\r\n";

  std::string text_payload = request_line + headers + "\r\n" + content;

  // Wire length includes the text plus 5 bytes of framing overhead
  uint16_t wire_length = static_cast<uint16_t>(text_payload.size() + 5);

  // Pack length (big-endian) followed by the message text
  std::vector<uint8_t> raw_packet;
  raw_packet.reserve(2 + text_payload.size() + 1);
  raw_packet.push_back(static_cast<uint8_t>(wire_length >> 8));
  raw_packet.push_back(static_cast<uint8_t>(wire_length & 0xFF));

  std::transform(text_payload.begin(), text_payload.end(),
                 std::back_inserter(raw_packet),
                 [](char ch) { return static_cast<uint8_t>(ch); });

  // Append checksum byte
  raw_packet.push_back(calculate_crc(raw_packet));

  // Apply byte-stuffing to protect sentinel values inside the payload
  std::vector<uint8_t> stuffed = escape_data(raw_packet);

  // Wrap with start and end frame markers
  std::vector<uint8_t> wire_frame;
  wire_frame.reserve(stuffed.size() + 2);
  wire_frame.push_back(FRAME_MARKER);
  wire_frame.insert(wire_frame.end(), stuffed.begin(), stuffed.end());
  wire_frame.push_back(FRAME_MARKER);

  return wire_frame;
}

std::optional<Response> parse_response(const std::vector<uint8_t>& data) {
  // Minimum viable frame: marker + at least 2 inner bytes + marker
  if (data.size() < 4) {
    return std::nullopt;
  }

  // Verify both boundary markers are present
  if (data.front() != FRAME_MARKER || data.back() != FRAME_MARKER) {
    return std::nullopt;
  }

  // Strip markers and reverse the byte-stuffing
  std::vector<uint8_t> inner(data.begin() + 1, data.end() - 1);
  std::vector<uint8_t> decoded = unescape_data(inner);

  // Need at least 2 bytes for length + 1 byte for CRC
  if (decoded.size() < 3) {
    return std::nullopt;
  }

  // Extract the message text between the length prefix and the trailing CRC
  std::string msg_text(decoded.begin() + 2, decoded.end() - 1);

  Response resp;
  resp.raw = msg_text;

  // Locate the header/body boundary (double CRLF)
  const std::string delimiter = "\r\n\r\n";
  auto boundary_pos = msg_text.find(delimiter);

  if (boundary_pos != std::string::npos) {
    std::string header_block = msg_text.substr(0, boundary_pos);
    resp.body = msg_text.substr(boundary_pos + delimiter.size());

    // Attempt JSON deserialization of the body
    if (!resp.body.empty()) {
      picojson::value parsed_val;
      std::string parse_err = picojson::parse(parsed_val, resp.body);
      if (parse_err.empty()) {
        resp.json = std::move(parsed_val);
      }
    }

    // The first line of the header block carries version and status
    auto first_newline = header_block.find("\r\n");
    std::string status_line = (first_newline != std::string::npos)
                                  ? header_block.substr(0, first_newline)
                                  : header_block;

    std::istringstream tokenizer(status_line);
    tokenizer >> resp.version >> resp.status;
  }

  return resp;
}

}  // namespace panorama
