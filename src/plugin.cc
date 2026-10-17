/*
 * libheif-hwdec - hardware HEVC and AV1 decoder plugin for libheif
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Decodes HEIC (HEVC) and AVIF (AV1) images on the GPU through FFmpeg's hardware acceleration API.
 *
 * libheif creates one plugin decoder per coded image, so a photo stored as a grid of 512x512 tiles
 * gets one decoder per tile, possibly several at once on libheif's worker threads. A decoder
 * receives the image's data in one or more pushes; each push becomes one FFmpeg packet (HEVC is
 * converted to Annex B first). The packets are decoded by FFmpeg's native "hevc" or "av1" decoder
 * bound to the one hardware device shared by the whole process (hw_device.cc), in a codec context
 * taken from a pool so the GPU decoder is set up once rather than per tile. Decoded frames are
 * copied out of GPU memory into planar heif_images (frame_copy.cc).
 *
 * If the GPU fails on a stream (unsupported profile, chroma format, bit depth or size, or a driver
 * error), the decoder decodes all its packets again in software with FFmpeg and skips the frames
 * it already returned, so a file never fails to decode because of this plugin. When no hardware
 * device exists, the plugin declines both formats in does_support_format() and libheif uses its
 * regular decoders, normally libde265 and dav1d or aom.
 *
 * libheif's plugin interface has changed over time: libheif 1.21 and later only load plugins of
 * API version 5 or newer, older ones do not know those versions. HWDEC_PLUGIN_API, found by CMake
 * from the libheif headers the plugin is built against, selects which entry points are filled in.
 */

#include "annexb.h"
#include "av1_obu.h"
#include "av1_probe.h"
#include "config.h"
#include "frame_copy.h"
#include "hw_device.h"

#include <libheif/heif.h>
#include <libheif/heif_plugin.h>

#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}

namespace {

using hwdec::HwDevice;

enum class Codec
{
  unknown,
  hevc,
  av1,
};

const char* codec_name(Codec codec)
{
  return codec == Codec::av1 ? "av1" : "hevc";
}

heif_error success()
{
  return heif_error{heif_error_Ok, heif_suberror_Unspecified, "Success"};
}

// libheif keeps the message pointer of a returned heif_error, so it must outlive the call.
// One buffer per thread holds the text of the most recent failure on that thread.
heif_error decoder_error(const std::string& message)
{
  static thread_local char buffer[256];
  snprintf(buffer, sizeof(buffer), "hwdec: %s", message.c_str());
  return heif_error{heif_error_Decoder_plugin_error, heif_suberror_Unspecified, buffer};
}

heif_error out_of_memory()
{
  return heif_error{heif_error_Memory_allocation_error, heif_suberror_Unspecified, "hwdec: out of memory"};
}

heif_error internal_error()
{
  return heif_error{heif_error_Decoder_plugin_error, heif_suberror_Unspecified, "hwdec: internal error"};
}

std::string ffmpeg_error(const char* what, int ret)
{
  char text[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(ret, text, sizeof(text));
  return std::string(what) + ": " + text;
}

// FFmpeg get_format callback. It is offered the GPU pixel format first when the hwaccel supports
// the stream's profile and chroma format; anything else gets the software format. If initialising
// the hwaccel fails, FFmpeg drops the GPU format from the list and calls this again.
AVPixelFormat pick_pixel_format(const HwDevice* device, const AVPixelFormat* formats)
{
  for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; p++) {
    if (*p == device->hw_pix_fmt) {
      return *p;
    }
  }
  for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; p++) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
    if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
      return *p;
    }
  }
  return AV_PIX_FMT_NONE;
}

AVPixelFormat choose_pixel_format(AVCodecContext* ctx, const AVPixelFormat* formats)
{
  const auto* device = static_cast<const HwDevice*>(ctx->opaque);
  AVPixelFormat chosen = pick_pixel_format(device, formats);

  // FFmpeg's hevc decoder labels full-range 8-bit 4:2:0 streams yuvj420p, an old pixel format that
  // only differs from yuv420p in its range flag. Most hwaccels ask for an nv12 GPU frame pool
  // regardless, but VDPAU asks for one in the stream's format, and VDPAU's frame pools reject
  // yuvj420p, which would leave every full-range photo to software. For that case only, the plugin
  // creates the pool itself in yuv420p: same samples, and the range still reaches the decoded frame
  // from the stream, which is where attach_nclx() reads it.
  if (chosen == device->hw_pix_fmt && ctx->sw_pix_fmt == AV_PIX_FMT_YUVJ420P) {
    AVBufferRef* frames = nullptr;
    if (avcodec_get_hw_frames_parameters(ctx, ctx->hw_device_ctx, chosen, &frames) >= 0) {
      auto* frames_ctx = reinterpret_cast<AVHWFramesContext*>(frames->data);
      if (frames_ctx->sw_format == AV_PIX_FMT_YUVJ420P) {
        frames_ctx->sw_format = AV_PIX_FMT_YUV420P;
        if (av_hwframe_ctx_init(frames) >= 0) {
          av_buffer_unref(&ctx->hw_frames_ctx);
          ctx->hw_frames_ctx = av_buffer_ref(frames);
          hwdec::debug_log("get_format: GPU frame pool created in yuv420p for the yuvj420p stream");
        }
      }
      av_buffer_unref(&frames);
    }
  }

  if (hwdec::debug_enabled()) {
    std::string offered;
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; p++) {
      offered += std::string(offered.empty() ? "" : ", ") + av_get_pix_fmt_name(*p);
    }
    hwdec::debug_log("get_format: stream is %s, offered %s, chose %s", av_get_pix_fmt_name(ctx->sw_pix_fmt),
                     offered.c_str(), av_get_pix_fmt_name(chosen));
  }
  return chosen;
}

double ms_since(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// The FFmpeg decoder for `codec`: on the GPU, FFmpeg's native decoder, which hands the work to the
// hwaccel; in software, the native hevc decoder, or for AV1 libdav1d (FFmpeg's native av1 decoder
// only works with a hwaccel), falling back to libaom.
const AVCodec* find_codec(Codec codec, bool on_gpu)
{
  if (codec == Codec::hevc) {
    return avcodec_find_decoder_by_name("hevc");
  }
  if (on_gpu) {
    return avcodec_find_decoder_by_name("av1");
  }
  if (const AVCodec* dav1d = avcodec_find_decoder_by_name("libdav1d")) {
    return dav1d;
  }
  return avcodec_find_decoder_by_name("libaom-av1");
}

AVCodecContext* open_context(Codec codec, const HwDevice* device, std::string& error)
{
  const AVCodec* av_codec = find_codec(codec, device != nullptr);
  if (!av_codec) {
    error = std::string("FFmpeg has no ") + (device ? "" : "software ") + codec_name(codec) + " decoder";
    return nullptr;
  }

  AVCodecContext* ctx = avcodec_alloc_context3(av_codec);
  if (!ctx) {
    error = "out of memory";
    return nullptr;
  }

  // libheif already decodes the tiles of a grid image in parallel.
  ctx->thread_count = 1;
  // Cropping is applied while copying into the heif_image; see frame_copy.h.
  ctx->apply_cropping = 0;
  // A failed GPU attempt is retried in software, so its errors are not worth printing into the
  // host application's terminal. LIBHEIF_HWDEC_DEBUG=1 shows them.
  if (!hwdec::debug_enabled()) {
    ctx->log_level_offset = AV_LOG_DEBUG - AV_LOG_ERROR;
  }

  if (device) {
    ctx->hw_device_ctx = av_buffer_ref(device->device_ref);
    if (!ctx->hw_device_ctx) {
      avcodec_free_context(&ctx);
      error = "out of memory";
      return nullptr;
    }
    ctx->opaque = const_cast<HwDevice*>(device);
    ctx->get_format = choose_pixel_format;
  }

  int ret = avcodec_open2(ctx, av_codec, nullptr);
  if (ret < 0) {
    avcodec_free_context(&ctx);
    error = ffmpeg_error("avcodec_open2", ret);
    return nullptr;
  }
  return ctx;
}

// Codec contexts bound to the GPU, kept between images. FFmpeg sets up its hwaccel (on NVDEC: the
// decoder object and its pool of GPU surfaces) when a context sees its first sequence parameters.
// That setup costs tens of milliseconds on NVDEC, while decoding a 512x512 tile takes about two,
// and libheif makes a new plugin decoder for every tile of a grid image. Reusing contexts pays the
// setup once per worker thread instead of once per tile.
//
// A context is only reused for a stream whose sequence parameters are byte-identical to those of
// the stream it last decoded, as the tiles of one grid image are: the VPS, SPS and PPS for HEVC,
// the sequence header OBU for AV1. FFmpeg 6.1's hevc decoder can crash when a context that decoded
// 8-bit 4:2:0 on the GPU is given a 10-bit monochrome stream, and changed parameters would make
// FFmpeg redo the hwaccel setup anyway.
//
// A context goes back into the pool, flushed, only if its stream had no error. The pool holds at
// most kMaxPooledContexts across all keys; the oldest is freed to make room.
constexpr size_t kMaxPooledContexts = 16;

struct PooledContext
{
  std::vector<uint8_t> key; // codec, then the sequence parameters
  AVCodecContext* ctx;
};

std::mutex pool_mutex;
std::deque<PooledContext> context_pool;

AVCodecContext* take_gpu_context(Codec codec, const HwDevice* device, const std::vector<uint8_t>& key,
                                 std::string& error)
{
  {
    std::lock_guard<std::mutex> lock(pool_mutex);
    for (auto it = context_pool.rbegin(); it != context_pool.rend(); ++it) {
      if (it->key == key) {
        AVCodecContext* ctx = it->ctx;
        context_pool.erase(std::next(it).base());
        return ctx;
      }
    }
  }
  return open_context(codec, device, error);
}

void return_gpu_context(AVCodecContext* ctx, std::vector<uint8_t> key)
{
  avcodec_flush_buffers(ctx);

  AVCodecContext* evicted = nullptr;
  {
    std::lock_guard<std::mutex> lock(pool_mutex);
    context_pool.push_back({std::move(key), ctx});
    if (context_pool.size() > kMaxPooledContexts) {
      evicted = context_pool.front().ctx;
      context_pool.pop_front();
    }
  }
  avcodec_free_context(&evicted);
}

// Pool key for a stream whose first packet is `first`: the codec, then its sequence parameters.
// Empty when the packet has none, and then the context is not pooled.
std::vector<uint8_t> pool_key(Codec codec, const std::vector<uint8_t>& first)
{
  std::vector<uint8_t> parameters = (codec == Codec::hevc) ? hwdec::hevc_parameter_sets(first)
                                                           : hwdec::av1_sequence_header(first.data(), first.size());
  if (parameters.empty()) {
    return {};
  }
  parameters.insert(parameters.begin(), uint8_t(codec));
  return parameters;
}

struct Packet
{
  std::vector<uint8_t> data;
  uintptr_t user_data;
};

// A decoded frame in CPU memory with what is needed to turn it into a heif_image.
struct DecodedFrame
{
  AVFrame* frame = av_frame_alloc();
  int bit_depth = 0;     // coded bit depth, see copy_frame_to_heif_image()
  uintptr_t user_data = 0;
  bool on_gpu = false;
  double decode_ms = 0;
  double download_ms = 0;

  ~DecodedFrame() { av_frame_free(&frame); }
};

// One FFmpeg decode of a plugin decoder's packets, on the GPU when `device` is set, in software
// otherwise. Packets are sent in order as the decoder asks for frames; a packet's index is its pts,
// which carries libheif's user_data through to the frame decoded from it.
class Stream
{
public:
  Stream(Codec codec, const HwDevice* device) : codec_(codec), device_(device) {}

  ~Stream()
  {
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    if (!key_.empty() && !failed_) {
      return_gpu_context(ctx_, std::move(key_));
    }
    else {
      avcodec_free_context(&ctx_);
    }
  }

  bool open(const std::vector<Packet>& packets, size_t frames_to_skip, std::string& error)
  {
    frames_to_skip_ = frames_to_skip;
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!packet_ || !frame_) {
      error = "out of memory";
      return false;
    }
    if (device_) {
      key_ = pool_key(codec_, packets.front().data);
      ctx_ = key_.empty() ? open_context(codec_, device_, error) : take_gpu_context(codec_, device_, key_, error);
    }
    else {
      ctx_ = open_context(codec_, nullptr, error);
    }
    return ctx_ != nullptr;
  }

  bool on_gpu() const { return device_ != nullptr; }

  enum class Result
  {
    frame,     // `out` holds the next frame
    need_data, // every packet pushed so far is decoded and the stream is not flushed
    end,       // the stream is flushed and every frame has been returned
    error,
  };

  // Decodes until the next frame is available, sending packets as FFmpeg asks for them, and the
  // end-of-stream marker once `flushed` and all packets are sent.
  Result next_frame(const std::vector<Packet>& packets, bool flushed, DecodedFrame& out, std::string& error)
  {
    auto start = std::chrono::steady_clock::now();
    for (;;) {
      int ret = avcodec_receive_frame(ctx_, frame_);
      if (ret == 0) {
        if (frames_to_skip_ > 0) {
          frames_to_skip_--;
          av_frame_unref(frame_);
          continue;
        }
        out.decode_ms = ms_since(start);
        return take_frame(packets, out, error) ? Result::frame : fail();
      }
      if (ret == AVERROR_EOF) {
        return Result::end;
      }
      if (ret != AVERROR(EAGAIN)) {
        error = ffmpeg_error("avcodec_receive_frame", ret);
        return fail();
      }

      if (next_packet_ < packets.size()) {
        const Packet& p = packets[next_packet_];
        ret = av_new_packet(packet_, int(p.data.size()));
        if (ret < 0) {
          error = ffmpeg_error("av_new_packet", ret);
          return fail();
        }
        memcpy(packet_->data, p.data.data(), p.data.size());
        packet_->pts = int64_t(next_packet_);
        ret = avcodec_send_packet(ctx_, packet_);
        av_packet_unref(packet_);
        if (ret < 0) {
          error = ffmpeg_error("avcodec_send_packet", ret);
          return fail();
        }
        next_packet_++;
      }
      else if (flushed && !eof_sent_) {
        ret = avcodec_send_packet(ctx_, nullptr);
        if (ret < 0) {
          error = ffmpeg_error("avcodec_send_packet (flush)", ret);
          return fail();
        }
        eof_sent_ = true;
      }
      else {
        return Result::need_data;
      }
    }
  }

private:
  Result fail()
  {
    failed_ = true;
    return Result::error;
  }

  bool take_frame(const std::vector<Packet>& packets, DecodedFrame& out, std::string& error)
  {
    // `out` may hold a frame from an earlier attempt on the GPU.
    out.on_gpu = false;
    out.download_ms = 0;
    // sw_pix_fmt is the format the stream decodes to in software, so its depth is the coded one.
    if (const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(ctx_->sw_pix_fmt)) {
      out.bit_depth = desc->comp[0].depth;
    }
    if (frame_->pts >= 0 && size_t(frame_->pts) < packets.size()) {
      out.user_data = packets[size_t(frame_->pts)].user_data;
    }
    if (device_ && (frame_->flags & AV_FRAME_FLAG_CORRUPT)) {
      error = "decoder reported a corrupt picture";
      av_frame_unref(frame_);
      return false;
    }

    av_frame_unref(out.frame);
    if (device_ && frame_->format == device_->hw_pix_fmt) {
      auto start = std::chrono::steady_clock::now();
      int ret = av_hwframe_transfer_data(out.frame, frame_, 0);
      if (ret < 0) {
        error = ffmpeg_error("av_hwframe_transfer_data", ret);
        av_frame_unref(frame_);
        return false;
      }
      av_frame_copy_props(out.frame, frame_);
      out.frame->crop_left = frame_->crop_left;
      out.frame->crop_right = frame_->crop_right;
      out.frame->crop_top = frame_->crop_top;
      out.frame->crop_bottom = frame_->crop_bottom;
      out.on_gpu = true;
      out.download_ms = ms_since(start);
      av_frame_unref(frame_);
    }
    else {
      av_frame_move_ref(out.frame, frame_);
    }
    return true;
  }

  Codec codec_;
  const HwDevice* device_;
  AVCodecContext* ctx_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
  std::vector<uint8_t> key_; // pool key; empty if ctx_ is not pooled
  size_t next_packet_ = 0;
  size_t frames_to_skip_ = 0;
  bool eof_sent_ = false;
  bool failed_ = false;
};

// Whether the shared device decodes AV1. Decoding the embedded probe picture is the only reliable
// test: Intel's VAAPI driver 24.1 with FFmpeg 6.1 lists AV1 decoding and fails on every stream.
// Runs once per process, the first time libheif asks about AV1.
bool gpu_decodes_av1()
{
  static std::once_flag once;
  static bool result = false;
  std::call_once(once, [] {
    const HwDevice* device = hwdec::shared_device();
    if (!device) {
      return;
    }
    std::vector<Packet> packets(1);
    packets[0].data.assign(hwdec::av1_probe_picture, hwdec::av1_probe_picture + sizeof(hwdec::av1_probe_picture));
    Stream stream(Codec::av1, device);
    DecodedFrame frame;
    std::string error;
    if (stream.open(packets, 0, error) && stream.next_frame(packets, true, frame, error) == Stream::Result::frame) {
      result = frame.on_gpu && frame.frame->width == 192;
    }
    hwdec::debug_log("AV1 probe on %s: %s%s%s", device->description.c_str(), result ? "decoded on the GPU" : "failed",
                     error.empty() ? "" : ", ", error.c_str());
  });
  return result;
}

// Whether this FFmpeg has every decoder the plugin needs for `codec`: the GPU decoder and the one
// used when the GPU fails. Distributions that leave out patented codecs, such as Fedora's
// ffmpeg-free, build FFmpeg without an hevc decoder; claiming HEVC there would make every HEIC
// image fail instead of letting libheif use libde265.
bool ffmpeg_has_decoders(Codec codec)
{
  return find_codec(codec, true) != nullptr && find_codec(codec, false) != nullptr;
}

const HwDevice* device_for(Codec codec)
{
  static const bool have_hevc = ffmpeg_has_decoders(Codec::hevc);
  static const bool have_av1 = ffmpeg_has_decoders(Codec::av1);
  if (!(codec == Codec::av1 ? have_av1 : have_hevc)) {
    return nullptr;
  }
  const HwDevice* device = hwdec::shared_device();
  if (device && codec == Codec::av1 && !gpu_decodes_av1()) {
    return nullptr;
  }
  return device;
}

struct Decoder
{
  Codec codec = Codec::unknown;
  bool strict = false;
  const heif_security_limits* limits = nullptr;

  // Every packet pushed, kept so the stream can be decoded again in software if the GPU fails.
  std::vector<Packet> packets;
  size_t total_bytes = 0;
  bool flushed = false;

  std::unique_ptr<Stream> stream;
  size_t frames_returned = 0;
};

// Attaches the colour description from the stream's VUI to the image. FFmpeg's AVColorPrimaries,
// AVColorTransferCharacteristic and AVColorSpace use the code points of ISO/IEC 23091-2, the same
// ones heif_color_profile_nclx uses, so the values pass through unchanged.
heif_error attach_nclx(const AVFrame* frame, heif_image* image, bool strict)
{
  heif_color_profile_nclx* nclx = heif_nclx_color_profile_alloc();
  if (!nclx) {
    return heif_error{heif_error_Memory_allocation_error, heif_suberror_Unspecified, "hwdec: out of memory"};
  }

  heif_error results[3] = {
      heif_nclx_color_profile_set_color_primaries(nclx, uint16_t(frame->color_primaries)),
      heif_nclx_color_profile_set_transfer_characteristics(nclx, uint16_t(frame->color_trc)),
      heif_nclx_color_profile_set_matrix_coefficients(nclx, uint16_t(frame->colorspace)),
  };
  for (const heif_error& e : results) {
    if (e.code != heif_error_Ok) {
      if (strict) {
        heif_nclx_color_profile_free(nclx);
        return e;
      }
      heif_image_add_decoding_warning(image, e);
    }
  }
  nclx->full_range_flag = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;

  heif_error err = heif_image_set_nclx_color_profile(image, nclx);
  heif_nclx_color_profile_free(nclx);
  return err;
}

// Opens decoder->stream on the GPU if one can decode the codec, in software otherwise or if
// opening on the GPU fails. Frames already returned are skipped.
bool open_stream(Decoder* decoder, bool allow_gpu, std::string& error)
{
  const HwDevice* device = allow_gpu ? device_for(decoder->codec) : nullptr;
  if (device) {
    decoder->stream.reset(new Stream(decoder->codec, device));
    if (decoder->stream->open(decoder->packets, decoder->frames_returned, error)) {
      return true;
    }
    hwdec::debug_log("cannot open GPU decoder (%s), using software", error.c_str());
    error.clear();
  }
  decoder->stream.reset(new Stream(decoder->codec, nullptr));
  return decoder->stream->open(decoder->packets, decoder->frames_returned, error);
}

heif_error decode_next_unguarded(Decoder* decoder, heif_image** out_image, uintptr_t* out_user_data,
                                 const heif_security_limits* limits)
{
  *out_image = nullptr;
  if (decoder->packets.empty()) {
    return success();
  }

  const auto entry = std::chrono::steady_clock::now();
  std::string error;
  double open_ms = 0;
  if (!decoder->stream) {
    if (!open_stream(decoder, true, error)) {
      return decoder_error(error);
    }
    open_ms = ms_since(entry);
  }

  DecodedFrame decoded;
  if (!decoded.frame) {
    return out_of_memory();
  }

  heif_image* image = nullptr;
  std::chrono::steady_clock::time_point copy_start;
  for (;;) {
    Stream::Result result = decoder->stream->next_frame(decoder->packets, decoder->flushed, decoded, error);
    if (result == Stream::Result::need_data || result == Stream::Result::end) {
      return success();
    }
    if (result == Stream::Result::frame) {
      copy_start = std::chrono::steady_clock::now();
      heif_error err = hwdec::copy_frame_to_heif_image(decoded.frame, decoded.bit_depth,
                                                       limits ? limits : decoder->limits, &image);
      if (err.code == heif_error_Ok) {
        err = attach_nclx(decoded.frame, image, decoder->strict);
        if (err.code != heif_error_Ok) {
          heif_image_release(image);
          image = nullptr;
        }
      }
      if (err.code == heif_error_Ok) {
        break;
      }
      if (!decoded.on_gpu) {
        return err;
      }
      // The GPU can hand back a frame in a format that can't hold the stream's samples, for
      // example a 10-bit 4:4:4 stream downloaded into an 8-bit format. That counts as a failed
      // GPU decode: the picture is decoded again in software.
      error = std::string(err.message) + ", frame format " +
              av_get_pix_fmt_name(AVPixelFormat(decoded.frame->format));
    }
    else if (!decoder->stream->on_gpu()) {
      return decoder_error(error);
    }
    hwdec::debug_log("GPU decoding failed (%s), decoding again in software", error.c_str());
    error.clear();
    if (!open_stream(decoder, false, error)) {
      return decoder_error(error);
    }
  }

  hwdec::debug_log("decoded %dx%d %s on %s, frame format %s, open %.1f ms, decode %.1f ms, download %.1f ms, "
                   "copy %.1f ms, in plugin %.1f ms",
                   decoded.frame->width, decoded.frame->height, codec_name(decoder->codec),
                   decoded.on_gpu ? "gpu" : "cpu", av_get_pix_fmt_name(AVPixelFormat(decoded.frame->format)),
                   open_ms, decoded.decode_ms, decoded.download_ms, ms_since(copy_start), ms_since(entry));

  decoder->frames_returned++;
  if (out_user_data) {
    *out_user_data = decoded.user_data;
  }
  *out_image = image;
  return success();
}

// FFmpeg packet sizes are ints. Replacing each 4-byte length with a 4-byte start code keeps an
// Annex B packet exactly as long as the data pushed, so this bounds every packet; it also bounds
// the total kept for a software retry.
constexpr size_t kMaxStreamBytes = size_t(INT_MAX) - AV_INPUT_BUFFER_PADDING_SIZE;

heif_error push_unguarded(Decoder* decoder, const void* data, size_t size, uintptr_t user_data)
{
  const auto* bytes = static_cast<const uint8_t*>(data);
  if (size > kMaxStreamBytes - decoder->total_bytes) {
    return heif_error{heif_error_Memory_allocation_error, heif_suberror_Security_limit_exceeded,
                      "hwdec: image data larger than 2 GiB"};
  }
  if (size == 0) {
    return success();
  }

  // Plugin API 3 does not say which format a decoder is for. HEVC data starts with the 4-byte
  // big-endian length of its first NAL unit, a parameter set far shorter than 16 MiB, so its first
  // byte is 0; AV1 data starts with an OBU header, which is never 0 (obu_type 0 is reserved).
  if (decoder->codec == Codec::unknown) {
    decoder->codec = (bytes[0] == 0) ? Codec::hevc : Codec::av1;
  }

  Packet packet;
  packet.user_data = user_data;
  if (decoder->codec == Codec::hevc) {
    if (!hwdec::append_length_prefixed_as_annexb(bytes, size, packet.data)) {
      return heif_error{heif_error_Decoder_plugin_error, heif_suberror_End_of_data,
                        "hwdec: NAL unit length exceeds the available data"};
    }
  }
  else {
    packet.data.assign(bytes, bytes + size);
  }
  decoder->total_bytes += size;
  decoder->packets.push_back(std::move(packet));
  return success();
}

// --- Entry points. Each one catches every exception: it would otherwise unwind through libheif's
// C plugin interface into the host application.

const char* plugin_name()
{
  static std::once_flag once;
  static char name[256];
  std::call_once(once, [] {
    const HwDevice* device = hwdec::shared_device();
    snprintf(name, sizeof(name), "libheif-hwdec %s, FFmpeg %s, %s", HWDEC_VERSION, av_version_info(),
             device ? device->description.c_str() : "no hardware device");
  });
  return name;
}

void init_plugin()
{
}

void deinit_plugin()
{
}

int does_support_format(heif_compression_format format)
{
  try {
    if (format == heif_compression_HEVC) {
      return device_for(Codec::hevc) ? HWDEC_PLUGIN_PRIORITY : 0;
    }
    if (format == heif_compression_AV1) {
      return device_for(Codec::av1) ? HWDEC_AV1_PLUGIN_PRIORITY : 0;
    }
  }
  catch (...) {
  }
  return 0;
}

heif_error new_decoder_for(void** decoder, Codec codec, bool strict, const heif_security_limits* limits)
{
  *decoder = nullptr;
  try {
    auto* d = new Decoder();
    d->codec = codec;
    d->strict = strict;
    d->limits = limits;
    *decoder = d;
  }
  catch (...) {
    return out_of_memory();
  }
  return success();
}

heif_error new_decoder(void** decoder)
{
  return new_decoder_for(decoder, Codec::unknown, false, nullptr);
}

void free_decoder(void* decoder)
{
  delete static_cast<Decoder*>(decoder);
}

void set_strict_decoding(void* decoder, int flag)
{
  static_cast<Decoder*>(decoder)->strict = (flag != 0);
}

heif_error push_data2(void* decoder, const void* data, size_t size, uintptr_t user_data)
{
  try {
    return push_unguarded(static_cast<Decoder*>(decoder), data, size, user_data);
  }
  catch (const std::bad_alloc&) {
    return out_of_memory();
  }
  catch (...) {
    return internal_error();
  }
}

heif_error push_data(void* decoder, const void* data, size_t size)
{
  return push_data2(decoder, data, size, 0);
}

heif_error flush_data(void* decoder)
{
  static_cast<Decoder*>(decoder)->flushed = true;
  return success();
}

heif_error decode_next_image2(void* decoder, heif_image** out_image, uintptr_t* out_user_data,
                              const heif_security_limits* limits)
{
  *out_image = nullptr;
  try {
    return decode_next_unguarded(static_cast<Decoder*>(decoder), out_image, out_user_data, limits);
  }
  catch (const std::bad_alloc&) {
    return out_of_memory();
  }
  catch (...) {
    return internal_error();
  }
}

#if HWDEC_PLUGIN_API >= 4
// Plugin API 4 (libheif 1.20) has no flush_data: libheif pushes the whole image, then calls this
// once and expects the image, so the end of the stream is implied. libheif 1.21 and later call
// decode_next_image2() instead.
heif_error decode_next_image(void* decoder, heif_image** out_image, const heif_security_limits* limits)
{
  flush_data(decoder);
  return decode_next_image2(decoder, out_image, nullptr, limits);
}
#endif

// Plugin API 1-3: everything has been pushed; return the one image.
heif_error decode_image(void* decoder, heif_image** out_image)
{
  flush_data(decoder);
  heif_error err = decode_next_image2(decoder, out_image, nullptr, nullptr);
  if (err.code == heif_error_Ok && !*out_image) {
    return heif_error{heif_error_Decoder_plugin_error, heif_suberror_End_of_data,
                      "hwdec: the data did not decode to an image"};
  }
  return err;
}

#if HWDEC_PLUGIN_API >= 5
int does_support_format2(const heif_decoder_plugin_compressed_format_description* format)
{
  return does_support_format(format->format);
}

heif_error new_decoder2(void** decoder, const heif_decoder_plugin_options* options)
{
  Codec codec = options->format == heif_compression_AV1 ? Codec::av1 : Codec::hevc;
#if HWDEC_PLUGIN_API >= 6
  const heif_security_limits* limits = options->limits;
#else
  const heif_security_limits* limits = nullptr;
#endif
  return new_decoder_for(decoder, codec, options->strict_decoding != 0, limits);
}
#endif

heif_decoder_plugin make_plugin()
{
  heif_decoder_plugin p{};
  p.plugin_api_version = HWDEC_PLUGIN_API;
  p.get_plugin_name = plugin_name;
  p.init_plugin = init_plugin;
  p.deinit_plugin = deinit_plugin;
  p.does_support_format = does_support_format;
  p.new_decoder = new_decoder;
  p.free_decoder = free_decoder;
  p.push_data = push_data;
  p.decode_image = decode_image;
  p.set_strict_decoding = set_strict_decoding;
  p.id_name = "hwdec";
#if HWDEC_PLUGIN_API >= 4
  p.decode_next_image = decode_next_image;
#endif
#if HWDEC_PLUGIN_API >= 5
  // The libheif release that introduced the plugin API version this build uses.
  p.minimum_required_libheif_version = (HWDEC_PLUGIN_API >= 6) ? LIBHEIF_MAKE_VERSION(1, 22, 0)
                                                               : LIBHEIF_MAKE_VERSION(1, 21, 0);
  p.does_support_format2 = does_support_format2;
  p.new_decoder2 = new_decoder2;
  p.push_data2 = push_data2;
  p.flush_data = flush_data;
  p.decode_next_image2 = decode_next_image2;
#endif
  return p;
}

const heif_decoder_plugin decoder_plugin = make_plugin();

} // namespace

extern "C" {
__attribute__((visibility("default"))) heif_plugin_info plugin_info = {
    1,
    heif_plugin_type_decoder,
    &decoder_plugin,
    nullptr,
};
}
