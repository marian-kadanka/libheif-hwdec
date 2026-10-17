/*
 * libheif-hwdec - hardware HEVC and AV1 decoder plugin for libheif
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef HWDEC_AV1_PROBE_H
#define HWDEC_AV1_PROBE_H

#include <cstdint>

namespace hwdec {

// A 192x192 mid-grey AV1 picture (8-bit 4:2:0, Main profile, reduced still picture header), as
// a temporal delimiter, a sequence header and a frame OBU. The plugin decodes it on the GPU once
// to find out whether that GPU, driver and FFmpeg can decode AV1 at all; drivers that list AV1
// decoding can still fail on every stream. 192 is above NVDEC's minimum size of 144.
//
// Made with:
//   ffmpeg -f lavfi -i color=gray:s=192x192 -frames:v 1 -c:v libaom-av1 -still-picture 1 -crf 40 -f obu probe.obu
const uint8_t av1_probe_picture[] = {
    0x12, 0x00, 0x0a, 0x07, 0x18, 0x1d, 0xef, 0xef, 0xdb, 0x00, 0x80, 0x32,
    0x0f, 0x12, 0x80, 0x00, 0x00, 0x14, 0x00, 0x00, 0x10, 0x01, 0xa0, 0xc6,
    0x90, 0xa1, 0x04, 0x14,
};

} // namespace hwdec

#endif
