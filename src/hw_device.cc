/*
 * libheif-hwdec - hardware HEVC and AV1 decoder plugin for libheif
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "hw_device.h"

#include "config.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#include <dirent.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#if HWDEC_HAVE_LIBVA
#include <libavutil/hwcontext_vaapi.h>
#include <va/va.h>
#endif
#if HWDEC_HAVE_VULKAN
#include <libavutil/hwcontext_vulkan.h>
#endif
}

namespace hwdec {

namespace {

bool env_is_false(const char* value)
{
  if (!value) {
    return false;
  }
  return strcmp(value, "0") == 0 || strcasecmp(value, "off") == 0 ||
         strcasecmp(value, "no") == 0 || strcasecmp(value, "false") == 0;
}

// av_err2str() is a C compound literal, which C++ does not accept.
std::string error_text(int ret)
{
  char text[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(ret, text, sizeof(text));
  return text;
}

std::vector<std::string> split_list(const std::string& value)
{
  std::vector<std::string> items;
  std::stringstream stream(value);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (!item.empty()) {
      items.push_back(item);
    }
  }
  return items;
}

// Pixel format FFmpeg's native HEVC decoder produces on this device type, or AV_PIX_FMT_NONE
// when the decoder cannot use it.
AVPixelFormat hevc_hw_pix_fmt(AVHWDeviceType type)
{
  const AVCodec* codec = avcodec_find_decoder_by_name("hevc");
  if (!codec) {
    return AV_PIX_FMT_NONE;
  }
  for (int i = 0;; i++) {
    const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
    if (!config) {
      return AV_PIX_FMT_NONE;
    }
    if (config->device_type == type && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
      return config->pix_fmt;
    }
  }
}

#if HWDEC_HAVE_LIBVA

struct RenderNode
{
  std::string path;
  bool is_nvidia;
};

// Every /dev/dri/renderD* node, sorted with NVIDIA nodes last. On a laptop with an integrated GPU
// and an NVIDIA GPU, opening the NVIDIA node powers that GPU up, which takes time and battery,
// so the integrated GPU is tried first and the NVIDIA node is only opened if it cannot decode.
std::vector<RenderNode> list_render_nodes()
{
  std::vector<RenderNode> nodes;

  DIR* dir = opendir("/dev/dri");
  if (!dir) {
    return nodes;
  }
  while (dirent* entry = readdir(dir)) {
    if (strncmp(entry->d_name, "renderD", 7) != 0) {
      continue;
    }
    std::string vendor;
    std::ifstream vendor_file(std::string("/sys/class/drm/") + entry->d_name + "/device/vendor");
    vendor_file >> vendor;
    nodes.push_back({std::string("/dev/dri/") + entry->d_name, vendor == "0x10de"});
  }
  closedir(dir);

  std::sort(nodes.begin(), nodes.end(), [](const RenderNode& a, const RenderNode& b) {
    if (a.is_nvidia != b.is_nvidia) {
      return !a.is_nvidia;
    }
    // renderD128 before renderD129, and renderD99 is not a name the kernel uses.
    return a.path.size() != b.path.size() ? a.path.size() < b.path.size() : a.path < b.path;
  });
  return nodes;
}

// True when the VAAPI driver behind the device offers HEVC Main decoding (VAEntrypointVLD).
// av_hwdevice_ctx_create succeeds for any working VAAPI driver, including ones without HEVC,
// so this is checked separately before the device is chosen.
bool vaapi_can_decode_hevc(AVBufferRef* device_ref)
{
  auto* device = reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
  auto* vaapi = static_cast<AVVAAPIDeviceContext*>(device->hwctx);

  int max_profiles = vaMaxNumProfiles(vaapi->display);
  std::vector<VAProfile> profiles(size_t(std::max(max_profiles, 1)));
  int n_profiles = 0;
  if (vaQueryConfigProfiles(vaapi->display, profiles.data(), &n_profiles) != VA_STATUS_SUCCESS) {
    return false;
  }
  if (std::find(profiles.begin(), profiles.begin() + n_profiles, VAProfileHEVCMain) == profiles.begin() + n_profiles) {
    return false;
  }

  int max_entrypoints = vaMaxNumEntrypoints(vaapi->display);
  std::vector<VAEntrypoint> entrypoints(size_t(std::max(max_entrypoints, 1)));
  int n_entrypoints = 0;
  if (vaQueryConfigEntrypoints(vaapi->display, VAProfileHEVCMain, entrypoints.data(), &n_entrypoints) != VA_STATUS_SUCCESS) {
    return false;
  }
  return std::find(entrypoints.begin(), entrypoints.begin() + n_entrypoints, VAEntrypointVLD) != entrypoints.begin() + n_entrypoints;
}

std::string vaapi_vendor_string(AVBufferRef* device_ref)
{
  auto* device = reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
  auto* vaapi = static_cast<AVVAAPIDeviceContext*>(device->hwctx);
  const char* vendor = vaQueryVendorString(vaapi->display);
  return vendor ? vendor : "unknown driver";
}

bool open_vaapi(const char* requested_device, HwDevice& out)
{
  std::vector<std::string> candidates;
  if (requested_device) {
    candidates.push_back(requested_device);
  }
  else {
    for (const RenderNode& node : list_render_nodes()) {
      candidates.push_back(node.path);
    }
  }

  for (const std::string& path : candidates) {
    AVBufferRef* ref = nullptr;
    int ret = av_hwdevice_ctx_create(&ref, AV_HWDEVICE_TYPE_VAAPI, path.c_str(), nullptr, 0);
    if (ret < 0) {
      debug_log("vaapi: cannot open %s (%s)", path.c_str(), error_text(ret).c_str());
      continue;
    }
    if (!vaapi_can_decode_hevc(ref)) {
      debug_log("vaapi: %s has no HEVC Main decoding (%s)", path.c_str(), vaapi_vendor_string(ref).c_str());
      av_buffer_unref(&ref);
      continue;
    }

    out.device_ref = ref;
    out.type = AV_HWDEVICE_TYPE_VAAPI;
    out.description = "vaapi " + path + " (" + vaapi_vendor_string(ref) + ")";
    return true;
  }
  return false;
}

#endif // HWDEC_HAVE_LIBVA

#if HWDEC_HAVE_VULKAN

// True when the Vulkan device has a video decode queue. Where FFmpeg reports the codecs of each
// queue family (libavutil 59.34, FFmpeg 7.1), the family must also decode H.265; FFmpeg checks AV1
// itself when it sets up the hwaccel, and the AV1 probe in plugin.cc catches a failure.
bool vulkan_can_decode(AVBufferRef* device_ref)
{
  auto* device = reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
  auto* vk = static_cast<AVVulkanDeviceContext*>(device->hwctx);
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 34, 100)
  for (int i = 0; i < vk->nb_qf; i++) {
    if ((vk->qf[i].flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) &&
        (vk->qf[i].video_caps & VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR)) {
      return true;
    }
  }
  return false;
#else
  return vk->queue_family_decode_index >= 0 && vk->nb_decode_queues > 0;
#endif
}

std::string vulkan_device_name(AVBufferRef* device_ref)
{
  auto* device = reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
  auto* vk = static_cast<AVVulkanDeviceContext*>(device->hwctx);
  auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      vk->get_proc_addr(vk->inst, "vkGetPhysicalDeviceProperties"));
  if (!get_properties) {
    return "unknown device";
  }
  VkPhysicalDeviceProperties properties{};
  get_properties(vk->phys_dev, &properties);
  return properties.deviceName;
}

// Opens the requested Vulkan device (an index), or else the first of devices 0-7 with a video
// decode queue. FFmpeg's own default is device 0, which may have none: Mesa's Intel driver only
// exposes video decoding with ANV_DEBUG=video-decode up to at least Mesa 25.3.
bool open_vulkan(const char* requested_device, HwDevice& out)
{
  std::vector<std::string> candidates;
  if (requested_device) {
    candidates.push_back(requested_device);
  }
  else {
    for (int i = 0; i < 8; i++) {
      candidates.push_back(std::to_string(i));
    }
  }

  for (const std::string& index : candidates) {
    AVBufferRef* ref = nullptr;
    int ret = av_hwdevice_ctx_create(&ref, AV_HWDEVICE_TYPE_VULKAN, index.c_str(), nullptr, 0);
    if (ret < 0) {
      // Devices are numbered from 0 without gaps, so the first index that fails is past the last.
      debug_log("vulkan: cannot open device %s (%s)", index.c_str(), error_text(ret).c_str());
      break;
    }
    std::string name = vulkan_device_name(ref);
    if (!vulkan_can_decode(ref)) {
      debug_log("vulkan: device %s (%s) has no H.265 video decode queue", index.c_str(), name.c_str());
      av_buffer_unref(&ref);
      continue;
    }
    out.device_ref = ref;
    out.type = AV_HWDEVICE_TYPE_VULKAN;
    out.description = "vulkan " + index + " (" + name + ")";
    return true;
  }
  return false;
}

#endif // HWDEC_HAVE_VULKAN

bool open_generic(AVHWDeviceType type, const char* requested_device, HwDevice& out)
{
  AVBufferRef* ref = nullptr;
  int ret = av_hwdevice_ctx_create(&ref, type, requested_device, nullptr, 0);
  if (ret < 0) {
    debug_log("%s: cannot open device %s (%s)", av_hwdevice_get_type_name(type),
              requested_device ? requested_device : "(default)", error_text(ret).c_str());
    return false;
  }
  out.device_ref = ref;
  out.type = type;
  out.description = std::string(av_hwdevice_get_type_name(type)) + " " + (requested_device ? requested_device : "default device");
  return true;
}

HwDevice* open_device()
{
  if (env_is_false(getenv("LIBHEIF_HWDEC"))) {
    debug_log("disabled by LIBHEIF_HWDEC");
    return nullptr;
  }

  const char* backend_env = getenv("LIBHEIF_HWDEC_BACKEND");
  const char* device_env = getenv("LIBHEIF_HWDEC_DEVICE");
  if (device_env && !*device_env) {
    device_env = nullptr;
  }

  for (const std::string& name : split_list(backend_env && *backend_env ? backend_env : "vaapi,cuda")) {
    AVHWDeviceType type = av_hwdevice_find_type_by_name(name.c_str());
    if (type == AV_HWDEVICE_TYPE_NONE) {
      debug_log("backend \"%s\" is not known to this FFmpeg build", name.c_str());
      continue;
    }

    AVPixelFormat hw_pix_fmt = hevc_hw_pix_fmt(type);
    if (hw_pix_fmt == AV_PIX_FMT_NONE) {
      debug_log("FFmpeg's hevc decoder cannot use backend \"%s\"", name.c_str());
      continue;
    }

    HwDevice device;
    bool opened;
#if HWDEC_HAVE_LIBVA
    if (type == AV_HWDEVICE_TYPE_VAAPI) {
      opened = open_vaapi(device_env, device);
    }
    else
#endif
#if HWDEC_HAVE_VULKAN
    if (type == AV_HWDEVICE_TYPE_VULKAN) {
      opened = open_vulkan(device_env, device);
    }
    else
#endif
    {
      opened = open_generic(type, device_env, device);
    }

    if (opened) {
      device.hw_pix_fmt = hw_pix_fmt;
      debug_log("using %s", device.description.c_str());
      return new HwDevice(device);
    }
  }

  debug_log("no hardware device found, libheif will use another HEVC decoder");
  return nullptr;
}

std::once_flag device_once;

// Never freed. The plugin is linked with -z nodelete, so it is not unmapped when libheif unloads
// its plugins, and the device survives any number of heif_init()/heif_deinit() cycles. Freeing it
// from a destructor at process exit is not safe: by then the CUDA driver may already have shut
// itself down, and cuCtxDestroy() fails. The kernel releases the GPU context when the process ends.
HwDevice* device = nullptr;

} // namespace

const HwDevice* shared_device()
{
  std::call_once(device_once, [] {
    auto start = std::chrono::steady_clock::now();
    device = open_device();
    debug_log("device setup took %.1f ms",
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  });
  return device;
}

bool debug_enabled()
{
  static const bool enabled = [] {
    const char* value = getenv("LIBHEIF_HWDEC_DEBUG");
    return value && *value && strcmp(value, "0") != 0;
  }();
  return enabled;
}

void debug_log(const char* format, ...)
{
  if (!debug_enabled()) {
    return;
  }
  char message[512];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  fprintf(stderr, "libheif-hwdec[%d]: %s\n", int(getpid()), message);
}

} // namespace hwdec
