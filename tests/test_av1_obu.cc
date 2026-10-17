/*
 * libheif-hwdec - hardware HEVC and AV1 decoder plugin for libheif
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Unit tests for av1_obu.cc.
 */

#include "av1_obu.h"
#include "test_util.h"

#include <vector>

using hwdec::av1_sequence_header;

namespace {

// OBU header byte for obu_type t with obu_has_size_field set.
uint8_t obu_header(int type)
{
  return uint8_t((type << 3) | 0x02);
}

void test_finds_sequence_header_after_temporal_delimiter()
{
  std::vector<uint8_t> stream = {obu_header(2), 0,                      // temporal delimiter
                                 obu_header(1), 3, 0xa1, 0xa2, 0xa3,    // sequence header
                                 obu_header(6), 2, 0xf1, 0xf2};         // frame
  std::vector<uint8_t> expected = {obu_header(1), 3, 0xa1, 0xa2, 0xa3};
  CHECK(av1_sequence_header(stream.data(), stream.size()) == expected);
}

void test_multi_byte_leb128_size()
{
  std::vector<uint8_t> stream = {obu_header(1), 0x81, 0x01}; // size 129
  stream.insert(stream.end(), 129, 0x55);
  stream.push_back(obu_header(6));
  stream.push_back(0);
  CHECK(av1_sequence_header(stream.data(), stream.size()).size() == 3 + 129);
}

void test_extension_byte_is_kept()
{
  std::vector<uint8_t> stream = {uint8_t(obu_header(1) | 0x04), 0x20, 2, 0xb1, 0xb2};
  CHECK(av1_sequence_header(stream.data(), stream.size()) == stream);
}

void test_no_size_field_runs_to_end()
{
  std::vector<uint8_t> stream = {uint8_t(1 << 3), 0xc1, 0xc2};
  CHECK(av1_sequence_header(stream.data(), stream.size()) == stream);
}

void test_rejects_malformed()
{
  std::vector<uint8_t> overrun = {obu_header(1), 9, 0x01};
  CHECK(av1_sequence_header(overrun.data(), overrun.size()).empty());

  std::vector<uint8_t> truncated_size = {obu_header(1), 0x80};
  CHECK(av1_sequence_header(truncated_size.data(), truncated_size.size()).empty());

  std::vector<uint8_t> forbidden = {uint8_t(0x80 | obu_header(1)), 0};
  CHECK(av1_sequence_header(forbidden.data(), forbidden.size()).empty());

  std::vector<uint8_t> no_sequence_header = {obu_header(2), 0, obu_header(6), 1, 0};
  CHECK(av1_sequence_header(no_sequence_header.data(), no_sequence_header.size()).empty());

  CHECK(av1_sequence_header(nullptr, 0).empty());
}

} // namespace

int main()
{
  test_finds_sequence_header_after_temporal_delimiter();
  test_multi_byte_leb128_size();
  test_extension_byte_is_kept();
  test_no_size_field_runs_to_end();
  test_rejects_malformed();
  return test_result();
}
