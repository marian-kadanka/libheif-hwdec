/*
 * libheif-hwdec - hardware HEVC and AV1 decoder plugin for libheif
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef HWDEC_ANNEXB_H
#define HWDEC_ANNEXB_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hwdec {

// libheif hands a decoder plugin its HEVC data as a sequence of NAL units, each preceded by a
// 4-byte big-endian length. FFmpeg's HEVC decoder, given no hvcC extradata, expects Annex B
// instead: each NAL unit preceded by the start code 00 00 00 01.
//
// Appends every NAL unit in data[0..size) to `out` in Annex B form. Returns false, leaving `out`
// with only the complete NAL units before the fault, when a length prefix is truncated or a
// length runs past the end of the data.
bool append_length_prefixed_as_annexb(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

// Returns the VPS, SPS and PPS NAL units (HEVC NAL unit types 32, 33 and 34) of an Annex B
// stream produced by append_length_prefixed_as_annexb(), concatenated with their start codes.
// Two streams with the same result have the same picture size, chroma format, bit depth and
// coding tools. HEVC's emulation prevention keeps 00 00 01 out of NAL unit payloads, so every
// such sequence in the stream is a start code.
std::vector<uint8_t> hevc_parameter_sets(const std::vector<uint8_t>& annexb);

} // namespace hwdec

#endif
