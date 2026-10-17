/*
 * libheif-hwdec - hardware HEVC and AV1 decoder plugin for libheif
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Unit tests for frame_copy.cc. Each case fills an AVFrame of one pixel format with a known
 * pattern through libavutil's av_write_image_line2(), which knows every layout a pixel format
 * descriptor can express, then checks that copy_frame_to_heif_image() puts every sample of every
 * plane, after cropping, where the pattern says it belongs.
 */

#include "frame_copy.h"
#include "test_util.h"

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/common.h>
#include <libavutil/pixdesc.h>
}

namespace {

// Sample value of component `c` at component coordinates (x, y), at `bits` bits.
int pattern(int c, int x, int y, int bits)
{
  return (x * 7 + y * 13 + c * 101 + 5) & ((1 << bits) - 1);
}

struct Crop
{
  int left, top, right, bottom;
};

struct Case
{
  const char* pix_fmt;
  int bits;          // coded bit depth passed to copy_frame_to_heif_image; 0 = descriptor's
  heif_chroma chroma;
};

// Fills `frame` with pattern(): each component holds the pattern at the coded bit depth, placed
// in the top bits when the format's container is wider, as a GPU download delivers it.
void fill_frame(AVFrame* frame, const AVPixFmtDescriptor* desc, int bits)
{
  for (int c = 0; c < desc->nb_components; c++) {
    const bool chroma = (c == 1 || c == 2) && !(desc->flags & AV_PIX_FMT_FLAG_RGB);
    const int w = chroma ? AV_CEIL_RSHIFT(frame->width, desc->log2_chroma_w) : frame->width;
    const int h = chroma ? AV_CEIL_RSHIFT(frame->height, desc->log2_chroma_h) : frame->height;
    const int stored = desc->comp[c].depth;
    const int coded = bits ? bits : stored;

    std::vector<uint16_t> line(static_cast<size_t>(w));
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        line[size_t(x)] = uint16_t(pattern(c, x, y, coded) << (stored - coded));
      }
      av_write_image_line2(line.data(), frame->data, frame->linesize, desc, 0, y, c, w, 2);
    }
  }
}

void run_case(const Case& test, const Crop& crop)
{
  const std::string label = std::string(test.pix_fmt) + " bits=" + std::to_string(test.bits) +
                            " crop=" + std::to_string(crop.left) + "," + std::to_string(crop.top) + "," +
                            std::to_string(crop.right) + "," + std::to_string(crop.bottom);

  AVPixelFormat fmt = av_get_pix_fmt(test.pix_fmt);
  if (fmt == AV_PIX_FMT_NONE) {
    printf("skip %s: not known to this libavutil\n", label.c_str());
    return;
  }
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);

  AVFrame* frame = av_frame_alloc();
  frame->format = fmt;
  frame->width = 37;
  frame->height = 23;
  CHECK(av_frame_get_buffer(frame, 0) == 0);
  // av_write_image_line2() ORs bits into the buffer, and av_frame_get_buffer() leaves it
  // uninitialised.
  for (AVBufferRef* buf : frame->buf) {
    if (buf) {
      memset(buf->data, 0, buf->size);
    }
  }
  fill_frame(frame, desc, test.bits);
  frame->crop_left = size_t(crop.left);
  frame->crop_top = size_t(crop.top);
  frame->crop_right = size_t(crop.right);
  frame->crop_bottom = size_t(crop.bottom);

  heif_image* image = nullptr;
  heif_error err = hwdec::copy_frame_to_heif_image(frame, test.bits, nullptr, &image);
  if (err.code != heif_error_Ok || !image) {
    fprintf(stderr, "%s: copy failed: %s\n", label.c_str(), err.message);
    test_failures()++;
    av_frame_free(&frame);
    return;
  }

  const bool mono = (test.chroma == heif_chroma_monochrome);
  const bool rgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
  const int bits = test.bits ? test.bits : desc->comp[rgb ? 1 : 0].depth;
  const int width = 37 - crop.left - crop.right;
  const int height = 23 - crop.top - crop.bottom;

  CHECK(heif_image_get_chroma_format(image) == test.chroma);
  CHECK(heif_image_get_colorspace(image) == (mono ? heif_colorspace_monochrome : heif_colorspace_YCbCr));

  static const heif_channel channels[3] = {heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
  // gbrp* holds R, G, B as components 0, 1, 2; heif's Y, Cb, Cr take G, B, R.
  const int rgb_component[3] = {1, 2, 0};

  int bad_samples = 0;
  for (int ch = 0; ch < (mono ? 1 : 3); ch++) {
    const int comp = rgb ? rgb_component[ch] : ch;
    const int sx = ch ? desc->log2_chroma_w : 0;
    const int sy = ch ? desc->log2_chroma_h : 0;
    const int plane_w = AV_CEIL_RSHIFT(width, sx);
    const int plane_h = AV_CEIL_RSHIFT(height, sy);

    CHECK(heif_image_get_width(image, channels[ch]) == plane_w);
    CHECK(heif_image_get_height(image, channels[ch]) == plane_h);
    CHECK(heif_image_get_bits_per_pixel_range(image, channels[ch]) == bits);

    int stride = 0;
    const uint8_t* plane = heif_image_get_plane_readonly(image, channels[ch], &stride);
    for (int y = 0; y < plane_h; y++) {
      for (int x = 0; x < plane_w; x++) {
        int expected = pattern(comp, x + (crop.left >> sx), y + (crop.top >> sy), bits);
        int actual = (bits > 8) ? reinterpret_cast<const uint16_t*>(plane + size_t(y) * stride)[x]
                                : plane[size_t(y) * stride + size_t(x)];
        if (actual != expected) {
          if (bad_samples < 3) {
            fprintf(stderr, "%s: channel %d sample (%d,%d) is %d, expected %d\n", label.c_str(), ch, x, y,
                    actual, expected);
          }
          bad_samples++;
        }
      }
    }
  }
  if (bad_samples) {
    fprintf(stderr, "%s: %d wrong samples\n", label.c_str(), bad_samples);
    test_failures()++;
  }
  else {
    printf("ok   %s\n", label.c_str());
  }

  heif_image_release(image);
  av_frame_free(&frame);
}

void expect_rejected(const char* pix_fmt, int bits = 0, Crop crop = {0, 0, 0, 0})
{
  AVPixelFormat fmt = av_get_pix_fmt(pix_fmt);
  CHECK(fmt != AV_PIX_FMT_NONE);

  AVFrame* frame = av_frame_alloc();
  frame->format = fmt;
  frame->width = 16;
  frame->height = 16;
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
  if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
    CHECK(av_frame_get_buffer(frame, 0) == 0);
  }
  frame->crop_left = size_t(crop.left);
  frame->crop_right = size_t(crop.right);

  heif_image* image = nullptr;
  heif_error err = hwdec::copy_frame_to_heif_image(frame, bits, nullptr, &image);
  if (err.code == heif_error_Ok || image) {
    fprintf(stderr, "%s bits=%d: expected an error\n", pix_fmt, bits);
    test_failures()++;
    heif_image_release(image);
  }
  else {
    printf("ok   %s bits=%d rejected: %s\n", pix_fmt, bits, err.message);
  }
  av_frame_free(&frame);
}

} // namespace

int main()
{
  heif_init(nullptr);

  const Case cases[] = {
      // Software decoder output.
      {"yuv420p", 0, heif_chroma_420},
      {"yuvj420p", 0, heif_chroma_420},
      {"yuv420p10le", 0, heif_chroma_420},
      {"yuv420p12le", 0, heif_chroma_420},
      {"yuv422p", 0, heif_chroma_422},
      {"yuv422p10le", 0, heif_chroma_422},
      {"yuv444p", 0, heif_chroma_444},
      {"yuv444p10le", 0, heif_chroma_444},
      {"yuv444p12le", 0, heif_chroma_444},
      {"gray", 0, heif_chroma_monochrome},
      {"gray10le", 0, heif_chroma_monochrome},
      {"gray12le", 0, heif_chroma_monochrome},
      {"gbrp", 0, heif_chroma_444},
      {"gbrp10le", 0, heif_chroma_444},
      // GPU downloads: semi-planar, packed, and 16-bit containers holding fewer coded bits.
      {"nv12", 0, heif_chroma_420},
      {"p010le", 0, heif_chroma_420},
      {"p012le", 0, heif_chroma_420},
      {"p016le", 12, heif_chroma_420},
      {"p016le", 10, heif_chroma_420},
      {"yuv444p16le", 10, heif_chroma_444},
      {"yuv444p16le", 12, heif_chroma_444},
      {"yuv420p16le", 8, heif_chroma_420},
      {"y210le", 0, heif_chroma_422},
      {"y212le", 0, heif_chroma_422},
      {"vuyx", 0, heif_chroma_444},
      {"xv30le", 0, heif_chroma_444},
      {"xv36le", 0, heif_chroma_444},
  };
  const Crop crops[] = {{0, 0, 0, 0}, {2, 2, 3, 1}, {0, 0, 1, 1}};

  for (const Case& test : cases) {
    for (const Crop& crop : crops) {
      run_case(test, crop);
    }
  }

  expect_rejected("vaapi");
  expect_rejected("yuv410p");
  expect_rejected("rgb8");
  expect_rejected("yuv420p", 12);
  expect_rejected("yuv420p", 0, {8, 0, 8, 0});

  heif_deinit();
  return test_result();
}
