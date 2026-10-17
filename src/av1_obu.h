/*
 * libheif-hwdec - hardware HEVC and AV1 decoder plugin for libheif
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef HWDEC_AV1_OBU_H
#define HWDEC_AV1_OBU_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hwdec {

// Returns the first sequence header OBU (obu_type 1) in an AV1 low-overhead bitstream, header
// included, or an empty vector when there is none or an OBU is malformed. libheif passes AVIF
// images to a decoder as the av1C box's configOBUs followed by the item data, both in this format.
// Two streams with the same sequence header have the same size, profile, bit depth and chroma
// format.
std::vector<uint8_t> av1_sequence_header(const uint8_t* data, size_t size);

} // namespace hwdec

#endif
