/*
 * libheif-hwdec - hardware HEVC and AV1 decoder plugin for libheif
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "annexb.h"

namespace hwdec {

bool append_length_prefixed_as_annexb(const uint8_t* data, size_t size, std::vector<uint8_t>& out)
{
  static const uint8_t start_code[4] = {0, 0, 0, 1};

  size_t pos = 0;
  while (pos < size) {
    if (size - pos < 4) {
      return false;
    }

    uint32_t nal_size = (uint32_t(data[pos]) << 24) | (uint32_t(data[pos + 1]) << 16) |
                        (uint32_t(data[pos + 2]) << 8) | uint32_t(data[pos + 3]);
    pos += 4;

    if (nal_size > size - pos) {
      return false;
    }

    out.insert(out.end(), start_code, start_code + 4);
    out.insert(out.end(), data + pos, data + pos + nal_size);
    pos += nal_size;
  }

  return true;
}

std::vector<uint8_t> hevc_parameter_sets(const std::vector<uint8_t>& annexb)
{
  std::vector<uint8_t> result;

  // Offsets of the first byte after each 00 00 01.
  std::vector<size_t> starts;
  for (size_t i = 0; i + 3 <= annexb.size(); i++) {
    if (annexb[i] == 0 && annexb[i + 1] == 0 && annexb[i + 2] == 1) {
      starts.push_back(i + 3);
      i += 2;
    }
  }

  for (size_t n = 0; n < starts.size(); n++) {
    size_t begin = starts[n];
    // The next NAL unit begins with 00 00 00 01; this one ends before those four bytes.
    size_t end = (n + 1 < starts.size()) ? starts[n + 1] - 4 : annexb.size();
    if (begin >= end) {
      continue;
    }
    int nal_type = (annexb[begin] >> 1) & 0x3f;
    if (nal_type >= 32 && nal_type <= 34) {
      static const uint8_t start_code[4] = {0, 0, 0, 1};
      result.insert(result.end(), start_code, start_code + 4);
      result.insert(result.end(), annexb.begin() + long(begin), annexb.begin() + long(end));
    }
  }
  return result;
}

} // namespace hwdec
