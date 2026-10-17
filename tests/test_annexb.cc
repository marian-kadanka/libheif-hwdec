/*
 * libheif-hwdec - hardware HEVC and AV1 decoder plugin for libheif
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Unit tests for annexb.cc.
 */

#include "annexb.h"
#include "test_util.h"

#include <vector>

using hwdec::append_length_prefixed_as_annexb;
using hwdec::hevc_parameter_sets;

namespace {

// NAL unit header byte 0 for HEVC NAL unit type t: forbidden_zero_bit 0, then six bits of type.
uint8_t nal_header(int type)
{
  return uint8_t(type << 1);
}

std::vector<uint8_t> length_prefixed(const std::vector<std::vector<uint8_t>>& nals)
{
  std::vector<uint8_t> out;
  for (const auto& nal : nals) {
    uint32_t n = uint32_t(nal.size());
    out.push_back(uint8_t(n >> 24));
    out.push_back(uint8_t(n >> 16));
    out.push_back(uint8_t(n >> 8));
    out.push_back(uint8_t(n));
    out.insert(out.end(), nal.begin(), nal.end());
  }
  return out;
}

void test_converts_each_nal()
{
  std::vector<uint8_t> vps = {nal_header(32), 1, 0xaa};
  std::vector<uint8_t> slice = {nal_header(19), 1, 0x10, 0x20, 0x30};
  std::vector<uint8_t> in = length_prefixed({vps, slice});

  std::vector<uint8_t> out;
  CHECK(append_length_prefixed_as_annexb(in.data(), in.size(), out));
  std::vector<uint8_t> expected = {0, 0, 0, 1, nal_header(32), 1, 0xaa,
                                   0, 0, 0, 1, nal_header(19), 1, 0x10, 0x20, 0x30};
  CHECK(out == expected);
}

void test_appends_across_calls()
{
  std::vector<uint8_t> first = length_prefixed({{nal_header(33), 1, 7}});
  std::vector<uint8_t> second = length_prefixed({{nal_header(34), 1, 8}});

  std::vector<uint8_t> out;
  CHECK(append_length_prefixed_as_annexb(first.data(), first.size(), out));
  CHECK(append_length_prefixed_as_annexb(second.data(), second.size(), out));
  std::vector<uint8_t> expected = {0, 0, 0, 1, nal_header(33), 1, 7, 0, 0, 0, 1, nal_header(34), 1, 8};
  CHECK(out == expected);
}

void test_empty_input()
{
  std::vector<uint8_t> out;
  CHECK(append_length_prefixed_as_annexb(nullptr, 0, out));
  CHECK(out.empty());
}

void test_truncated_length_prefix()
{
  std::vector<uint8_t> in = length_prefixed({{nal_header(1), 1, 5}});
  in.push_back(0);
  in.push_back(0);

  std::vector<uint8_t> out;
  CHECK(!append_length_prefixed_as_annexb(in.data(), in.size(), out));
  // The complete NAL unit before the fault is kept.
  CHECK(out.size() == 4 + 3);
}

void test_length_past_end()
{
  std::vector<uint8_t> in = {0, 0, 0, 9, nal_header(1), 1, 5};
  std::vector<uint8_t> out;
  CHECK(!append_length_prefixed_as_annexb(in.data(), in.size(), out));
  CHECK(out.empty());
}

void test_parameter_sets_are_extracted_in_order()
{
  std::vector<uint8_t> vps = {nal_header(32), 1, 0x11};
  std::vector<uint8_t> sps = {nal_header(33), 1, 0x22, 0x23};
  std::vector<uint8_t> pps = {nal_header(34), 1, 0x33};
  std::vector<uint8_t> sei = {nal_header(39), 1, 0x44};
  std::vector<uint8_t> slice = {nal_header(19), 1, 0x55, 0x00, 0x00, 0x03, 0x01};
  std::vector<uint8_t> in = length_prefixed({vps, sps, pps, sei, slice});

  std::vector<uint8_t> annexb;
  CHECK(append_length_prefixed_as_annexb(in.data(), in.size(), annexb));

  std::vector<uint8_t> expected = {0, 0, 0, 1, nal_header(32), 1, 0x11,
                                   0, 0, 0, 1, nal_header(33), 1, 0x22, 0x23,
                                   0, 0, 0, 1, nal_header(34), 1, 0x33};
  CHECK(hevc_parameter_sets(annexb) == expected);
}

void test_parameter_sets_differ_when_sps_differs()
{
  std::vector<uint8_t> a, b;
  std::vector<uint8_t> in_a = length_prefixed({{nal_header(33), 1, 0x01}, {nal_header(19), 1, 9}});
  std::vector<uint8_t> in_b = length_prefixed({{nal_header(33), 1, 0x02}, {nal_header(19), 1, 9}});
  CHECK(append_length_prefixed_as_annexb(in_a.data(), in_a.size(), a));
  CHECK(append_length_prefixed_as_annexb(in_b.data(), in_b.size(), b));
  CHECK(hevc_parameter_sets(a) != hevc_parameter_sets(b));
}

void test_no_parameter_sets()
{
  std::vector<uint8_t> in = length_prefixed({{nal_header(19), 1, 9}});
  std::vector<uint8_t> annexb;
  CHECK(append_length_prefixed_as_annexb(in.data(), in.size(), annexb));
  CHECK(hevc_parameter_sets(annexb).empty());
  CHECK(hevc_parameter_sets({}).empty());
}

} // namespace

int main()
{
  test_converts_each_nal();
  test_appends_across_calls();
  test_empty_input();
  test_truncated_length_prefix();
  test_length_past_end();
  test_parameter_sets_are_extracted_in_order();
  test_parameter_sets_differ_when_sps_differs();
  test_no_parameter_sets();
  return test_result();
}
