/*
 * libheif-hwdec - hardware HEVC and AV1 decoder plugin for libheif
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef HWDEC_HW_DEVICE_H
#define HWDEC_HW_DEVICE_H

#include <string>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

namespace hwdec {

struct HwDevice
{
  AVBufferRef* device_ref = nullptr; // AVHWDeviceContext, shared by every decoder
  AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
  AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE; // pixel format of decoded frames in GPU memory
  std::string description;                    // e.g. "vaapi /dev/dri/renderD128 (Intel iHD driver ...)"
};

// Returns the hardware device all decoders share, opening it on the first call, or nullptr when
// no usable device exists or hardware decoding is disabled. Thread-safe. The device stays open
// for the life of the process.
//
// Environment:
//   LIBHEIF_HWDEC=0            disable hardware decoding (also "off", "no", "false")
//   LIBHEIF_HWDEC_BACKEND=...  comma-separated backends to try in order: vaapi, vdpau, cuda,
//                              vulkan. Default "vaapi,vdpau,cuda".
//   LIBHEIF_HWDEC_DEVICE=...   device to open: a DRM render node such as /dev/dri/renderD129 for
//                              vaapi, a GPU index such as 1 for cuda and vulkan. Default for vaapi:
//                              probe every /dev/dri/renderD* node, non-NVIDIA first, and take the
//                              first whose VAAPI driver can decode HEVC Main; for vulkan: the first
//                              device with an H.265 video decode queue.
const HwDevice* shared_device();

// True when LIBHEIF_HWDEC_DEBUG is set to a non-empty value other than "0".
bool debug_enabled();

// printf-style message to stderr prefixed with "libheif-hwdec[<pid>]: ", only when debug_enabled().
void debug_log(const char* format, ...) __attribute__((format(printf, 1, 2)));

} // namespace hwdec

#endif
