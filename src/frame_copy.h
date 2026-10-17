/*
 * libheif-hwdec - hardware HEVC and AV1 decoder plugin for libheif
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef HWDEC_FRAME_COPY_H
#define HWDEC_FRAME_COPY_H

#include <libheif/heif.h>

extern "C" {
#include <libavutil/frame.h>
}

// libheif before 1.19 has no security limits; only pointers to them are passed around here.
struct heif_security_limits;

namespace hwdec {

// Copies a decoded frame in CPU memory into a new heif_image with one plane per colour channel
// (Y, Cb, Cr, or Y alone for monochrome), the layout libheif expects from an HEVC decoder.
//
// Accepts any FFmpeg pixel format that holds YUV 4:0:0, 4:2:0, 4:2:2 or 4:4:4 with all channels
// at the same bit depth, whether planar (yuv420p, yuv420p10le, gray10le), semi-planar (nv12,
// p010le) or packed (y210le, vuyx, xv30le). Samples stored in the high bits of a 16-bit word,
// as p010le stores its 10 bits, are shifted down to their real value. gbrp* frames, which FFmpeg
// produces for 4:4:4 streams coded with the identity matrix, are mapped G to Y, B to Cb and R to
// Cr, which is how such a stream is coded.
//
// `bit_depth` is the bit depth of the coded stream. Some formats FFmpeg downloads GPU frames into
// hold fewer significant bits than their descriptor says: NVDEC returns 12-bit 4:2:0 as p016le
// and 10- or 12-bit 4:4:4 as yuv444p16le, with the value in the top bits of each 16-bit word.
// Samples are shifted down to `bit_depth` and the heif planes get that depth. Pass 0 to use the
// depth from the pixel format descriptor.
//
// The frame's crop_left/top/right/bottom fields are applied here, so the decoder may be run
// with apply_cropping = 0, which is necessary because FFmpeg cannot crop the top-left corner of
// frames that are still in GPU memory.
// `limits` bounds the planes allocated for the image when this libheif has
// heif_image_add_plane_safe(); it may be nullptr, and is ignored otherwise.
heif_error copy_frame_to_heif_image(const AVFrame* frame, int bit_depth, const heif_security_limits* limits,
                                    heif_image** out_image);

} // namespace hwdec

#endif
