/*
 * libheif-hwdec - hardware HEVC and AV1 decoder plugin for libheif
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "frame_copy.h"

#include "config.h"

#include <cstring>
#include <vector>

extern "C" {
#include <libavutil/common.h>
#include <libavutil/pixdesc.h>
}

namespace hwdec {

namespace {

heif_error make_error(heif_error_code code, heif_suberror_code subcode, const char* message)
{
  return heif_error{code, subcode, message};
}

// Copies the region of one component starting at component coordinates (x0, y0) into a heif
// plane of w x h samples. bytes_per_sample is 1 for depth <= 8 and 2 otherwise, matching the
// storage libheif uses for that plane. extra_shift is how many low bits of the component's
// value, as its descriptor defines it, are padding below the coded bit depth.
void copy_component(const AVFrame* frame, const AVPixFmtDescriptor* desc, int c,
                    int x0, int y0, int w, int h,
                    uint8_t* dst, int dst_stride, int bytes_per_sample, int extra_shift)
{
  const AVComponentDescriptor& comp = desc->comp[c];
  const uint8_t* src_base = frame->data[comp.plane];
  const int src_stride = frame->linesize[comp.plane];
  const bool big_endian = (desc->flags & AV_PIX_FMT_FLAG_BE) != 0;
  const bool bitstream = (desc->flags & AV_PIX_FMT_FLAG_BITSTREAM) != 0;

  // Planar, one sample per element and no shift: rows are already in the heif layout.
  if (!bitstream && !big_endian && comp.shift == 0 && extra_shift == 0 && comp.offset == 0 &&
      comp.step == bytes_per_sample) {
    for (int y = 0; y < h; y++) {
      const uint8_t* src = src_base + size_t(y0 + y) * src_stride + size_t(x0) * comp.step;
      memcpy(dst + size_t(y) * dst_stride, src, size_t(w) * bytes_per_sample);
    }
    return;
  }

  // Interleaved samples in single bytes (nv12 chroma, vuyx) or in little-endian 16-bit words
  // with the value in the high bits (p010le stores 10 bits shifted up by 6).
  if (!bitstream && !big_endian && comp.shift + comp.depth <= 16) {
    const uint16_t mask = uint16_t((1u << comp.depth) - 1);
    const bool byte_sized = comp.shift + comp.depth <= 8;

    for (int y = 0; y < h; y++) {
      const uint8_t* src = src_base + size_t(y0 + y) * src_stride + comp.offset + size_t(x0) * comp.step;
      uint8_t* row = dst + size_t(y) * dst_stride;

      if (byte_sized) {
        for (int x = 0; x < w; x++) {
          row[x] = uint8_t(((src[size_t(x) * comp.step] >> comp.shift) & mask) >> extra_shift);
        }
      }
      else {
        uint16_t* row16 = reinterpret_cast<uint16_t*>(row);
        for (int x = 0; x < w; x++) {
          const uint8_t* p = src + size_t(x) * comp.step;
          uint16_t v = uint16_t(p[0] | (p[1] << 8));
          v = uint16_t(((v >> comp.shift) & mask) >> extra_shift);
          if (bytes_per_sample == 1) {
            row[x] = uint8_t(v);
          }
          else {
            row16[x] = v;
          }
        }
      }
    }
    return;
  }

  // Anything else, such as 10-bit samples packed into 32-bit words (xv30le), goes through
  // libavutil's generic reader, which knows every layout a pixel format descriptor can express.
  const uint8_t* data[4] = {frame->data[0], frame->data[1], frame->data[2], frame->data[3]};
  std::vector<uint16_t> line(static_cast<size_t>(w));
  for (int y = 0; y < h; y++) {
    av_read_image_line2(line.data(), data, frame->linesize, desc, x0, y0 + y, c, w, 0, 2);
    uint8_t* row = dst + size_t(y) * dst_stride;
    if (bytes_per_sample == 1) {
      for (int x = 0; x < w; x++) {
        row[x] = uint8_t(line[size_t(x)] >> extra_shift);
      }
    }
    else {
      uint16_t* row16 = reinterpret_cast<uint16_t*>(row);
      for (int x = 0; x < w; x++) {
        row16[x] = uint16_t(line[size_t(x)] >> extra_shift);
      }
    }
  }
}

} // namespace

heif_error copy_frame_to_heif_image(const AVFrame* frame, int bit_depth, const heif_security_limits* limits,
                                    heif_image** out_image)
{
  *out_image = nullptr;

  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AVPixelFormat(frame->format));
  if (!desc || (desc->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL))) {
    return make_error(heif_error_Unsupported_feature, heif_suberror_Unsupported_color_conversion,
                      "hwdec: frame is not in a CPU pixel format");
  }

  const bool is_rgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
  const bool has_alpha = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
  const int colour_components = desc->nb_components - (has_alpha ? 1 : 0);

  heif_colorspace colorspace;
  heif_chroma chroma;
  // Index into desc->comp[] of the component that becomes heif's Y, Cb and Cr channel.
  int comp_for_channel[3] = {0, 1, 2};
  int n_channels;

  if (colour_components == 1) {
    colorspace = heif_colorspace_monochrome;
    chroma = heif_chroma_monochrome;
    n_channels = 1;
  }
  else if (colour_components == 3) {
    colorspace = heif_colorspace_YCbCr;
    n_channels = 3;

    if (is_rgb) {
      // gbrp: comp[0] is R, comp[1] is G, comp[2] is B.
      comp_for_channel[0] = 1;
      comp_for_channel[1] = 2;
      comp_for_channel[2] = 0;
    }

    if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1) {
      chroma = heif_chroma_420;
    }
    else if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0) {
      chroma = heif_chroma_422;
    }
    else if (desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0) {
      chroma = heif_chroma_444;
    }
    else {
      return make_error(heif_error_Unsupported_feature, heif_suberror_Unsupported_color_conversion,
                        "hwdec: unsupported chroma subsampling");
    }
  }
  else {
    return make_error(heif_error_Unsupported_feature, heif_suberror_Unsupported_color_conversion,
                      "hwdec: unsupported number of colour components");
  }

  const int stored_depth = desc->comp[comp_for_channel[0]].depth;
  for (int i = 1; i < n_channels; i++) {
    if (desc->comp[comp_for_channel[i]].depth != stored_depth) {
      return make_error(heif_error_Unsupported_feature, heif_suberror_Unsupported_color_conversion,
                        "hwdec: channels have different bit depths");
    }
  }
  const int depth = (bit_depth > 0) ? bit_depth : stored_depth;
  if (depth < 1 || depth > 16 || depth > stored_depth) {
    return make_error(heif_error_Unsupported_feature, heif_suberror_Unsupported_color_conversion,
                      "hwdec: unsupported bit depth");
  }

  const size_t crop_w = frame->crop_left + frame->crop_right;
  const size_t crop_h = frame->crop_top + frame->crop_bottom;
  if (frame->width <= 0 || frame->height <= 0 || crop_w >= size_t(frame->width) || crop_h >= size_t(frame->height)) {
    return make_error(heif_error_Decoder_plugin_error, heif_suberror_Invalid_image_size,
                      "hwdec: invalid frame size or cropping");
  }

  const int width = frame->width - int(crop_w);
  const int height = frame->height - int(crop_h);

  heif_image* image = nullptr;
  heif_error err = heif_image_create(width, height, colorspace, chroma, &image);
  if (err.code) {
    return err;
  }

  static const heif_channel channels[3] = {heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
  const int bytes_per_sample = depth > 8 ? 2 : 1;

  for (int i = 0; i < n_channels; i++) {
    const bool is_chroma = (i > 0);
    const int sx = is_chroma ? desc->log2_chroma_w : 0;
    const int sy = is_chroma ? desc->log2_chroma_h : 0;

    const int plane_w = AV_CEIL_RSHIFT(width, sx);
    const int plane_h = AV_CEIL_RSHIFT(height, sy);
    const int x0 = int(frame->crop_left >> sx);
    const int y0 = int(frame->crop_top >> sy);

#if HWDEC_HAVE_ADD_PLANE_SAFE
    err = heif_image_add_plane_safe(image, channels[i], plane_w, plane_h, depth,
                                    limits ? limits : heif_get_global_security_limits());
#else
    (void) limits;
    err = heif_image_add_plane(image, channels[i], plane_w, plane_h, depth);
#endif
    if (err.code) {
      heif_image_release(image);
      return err;
    }

    int dst_stride = 0;
    uint8_t* dst = heif_image_get_plane(image, channels[i], &dst_stride);
    if (!dst) {
      heif_image_release(image);
      return make_error(heif_error_Memory_allocation_error, heif_suberror_Unspecified,
                        "hwdec: cannot get heif image plane");
    }

    copy_component(frame, desc, comp_for_channel[i], x0, y0, plane_w, plane_h, dst, dst_stride,
                   bytes_per_sample, stored_depth - depth);
  }

  *out_image = image;
  return heif_error{heif_error_Ok, heif_suberror_Unspecified, "Success"};
}

} // namespace hwdec
