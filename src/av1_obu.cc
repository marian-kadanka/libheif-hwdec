/*
 * libheif-hwdec - hardware HEVC and AV1 decoder plugin for libheif
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "av1_obu.h"

namespace hwdec {

namespace {

// Reads a leb128 value (AV1 spec 4.10.5) at data[pos]; advances pos. False if truncated or longer
// than the 8 bytes the spec allows.
bool read_leb128(const uint8_t* data, size_t size, size_t& pos, uint64_t& value)
{
  value = 0;
  for (int i = 0; i < 8; i++) {
    if (pos >= size) {
      return false;
    }
    uint8_t byte = data[pos++];
    value |= uint64_t(byte & 0x7f) << (7 * i);
    if (!(byte & 0x80)) {
      return true;
    }
  }
  return false;
}

} // namespace

std::vector<uint8_t> av1_sequence_header(const uint8_t* data, size_t size)
{
  size_t pos = 0;
  while (pos < size) {
    const size_t start = pos;
    const uint8_t header = data[pos++];
    const int obu_type = (header >> 3) & 0x0f;
    const bool has_extension = (header >> 2) & 1;
    const bool has_size_field = (header >> 1) & 1;

    if (header & 0x80) {
      return {}; // forbidden bit
    }
    if (has_extension) {
      pos++;
    }

    uint64_t payload_size;
    if (has_size_field) {
      if (!read_leb128(data, size, pos, payload_size)) {
        return {};
      }
    }
    else {
      // Without a size field the OBU extends to the end of the data.
      payload_size = (pos <= size) ? size - pos : 0;
    }
    if (pos > size || payload_size > size - pos) {
      return {};
    }

    const size_t end = pos + size_t(payload_size);
    if (obu_type == 1) {
      return std::vector<uint8_t>(data + start, data + end);
    }
    pos = end;
  }
  return {};
}

} // namespace hwdec
