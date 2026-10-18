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
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

#include <dirent.h>
#include <dlfcn.h>
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
    // nvidia-vaapi-driver runs NVIDIA GPUs through VAAPI. Its "egl" backend never returns from
    // vaSyncSurface after a decode, so every image would hang; it is the default in version 0.0.8,
    // the one Ubuntu 24.04 ships. The "direct" backend works, and is the default from 0.0.14 on.
    // Without an explicit LIBHEIF_HWDEC_DEVICE the egl backend is skipped, and the cuda backend
    // reaches the same GPU.
    if (!requested_device && vaapi_vendor_string(ref).find("NVDEC driver [egl backend]") != std::string::npos) {
      debug_log("vaapi: skipping %s (%s): its decodes hang; nvidia-vaapi-driver 0.0.14 and later default to "
                "the working direct backend", path.c_str(), vaapi_vendor_string(ref).c_str());
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

// From FFmpeg 6.1 (libavutil 58.29), the first version that decodes video through Vulkan, the plugin
// creates the Vulkan device itself, see create_own_vulkan(); with older versions FFmpeg creates it.
// What the plugin fills in follows the changes to FFmpeg's public Vulkan API, listed in FFmpeg's
// doc/APIchanges:
//   libavutil 59.34 (FFmpeg 7.1)  AVVulkanDeviceContext.qf[] and nb_qf describe the queue families;
//                                 the per-type fields (queue_family_comp_index, nb_comp_queues,
//                                 ...) are deprecated and removed in libavutil 61 (FFmpeg 9.0).
//   libavutil 60.20 (FFmpeg 8.1)  av_vk_get_optional_device_extensions() lists the device
//                                 extensions FFmpeg can use. The plugin looks it up at run time,
//                                 see ffmpeg_extension_list(): openSUSE's build-only ffmpeg-8-mini
//                                 8.1.3 reports libavutil 60.26 but leaves it out.
//   libavutil 60.32 (FFmpeg 9.0)  AVVulkanDeviceContext.queue_flags holds the flags the queues were
//                                 created with.
#define HWDEC_OWN_VULKAN_DEVICE (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 29, 100))
#define HWDEC_VK_QF_LIST (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 34, 100))
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(59, 34, 100) || (defined(FF_API_VULKAN_FIXED_QUEUES) && FF_API_VULKAN_FIXED_QUEUES)
#define HWDEC_VK_FIXED_QUEUES 1
#else
#define HWDEC_VK_FIXED_QUEUES 0
#endif
#define HWDEC_VK_QUEUE_FLAGS (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 32, 100))

#if !HWDEC_OWN_VULKAN_DEVICE

// True when the Vulkan device has a video decode queue.
bool vulkan_can_decode(AVBufferRef* device_ref)
{
  auto* device = reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
  auto* vk = static_cast<AVVulkanDeviceContext*>(device->hwctx);
  return vk->queue_family_decode_index >= 0 && vk->nb_decode_queues > 0;
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

#else

// FFmpeg creates a Vulkan device with every queue of each queue family it uses, unless its
// limit_queues option (applied from FFmpeg 8.1) caps them, and even then with a graphics and an
// encode queue. NVIDIA's driver offers 16 graphics, 8 compute and 2 transfer queues, and setting
// up each takes time: on an NVIDIA T1200 the device took 570-800 ms to create with FFmpeg 6.1,
// about 270 ms with 8.0 and about 190 ms with 8.1 and limit_queues=1, against about 115 ms with one
// queue each for compute, transfers and decoding, and no graphics or encode queue, which is what
// the plugin creates. It enables the device extensions and features FFmpeg enables, as below, and
// hands the device to FFmpeg with av_hwdevice_ctx_init().

// Every device feature structure FFmpeg 6.1 to 9.0 passes to vkCreateDevice(), for the extensions
// the Vulkan headers the plugin is built with know; chain_features() links the core ones and those
// whose extension is enabled.
struct FeatureSet
{
  VkPhysicalDeviceFeatures2 device{};
  VkPhysicalDeviceVulkan11Features vulkan_1_1{};
  VkPhysicalDeviceVulkan12Features vulkan_1_2{};
  VkPhysicalDeviceVulkan13Features vulkan_1_3{};
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline_semaphore{};
#ifdef VK_EXT_descriptor_buffer
  VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_buffer{};
#endif
#ifdef VK_EXT_shader_atomic_float
  VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomic_float{};
#endif
#ifdef VK_KHR_cooperative_matrix
  VkPhysicalDeviceCooperativeMatrixFeaturesKHR cooperative_matrix{};
#endif
#ifdef VK_EXT_shader_object
  VkPhysicalDeviceShaderObjectFeaturesEXT shader_object{};
#endif
#ifdef VK_KHR_video_maintenance1
  VkPhysicalDeviceVideoMaintenance1FeaturesKHR video_maintenance_1{};
#endif
#ifdef VK_KHR_video_maintenance2
  VkPhysicalDeviceVideoMaintenance2FeaturesKHR video_maintenance_2{};
#endif
#ifdef VK_KHR_video_decode_vp9
  VkPhysicalDeviceVideoDecodeVP9FeaturesKHR vp9_decode{};
#endif
#ifdef VK_KHR_video_encode_av1
  VkPhysicalDeviceVideoEncodeAV1FeaturesKHR av1_encode{};
#endif
#ifdef VK_KHR_shader_subgroup_rotate
  VkPhysicalDeviceShaderSubgroupRotateFeaturesKHR subgroup_rotate{};
#endif
#ifdef VK_KHR_shader_expect_assume
  VkPhysicalDeviceShaderExpectAssumeFeaturesKHR expect_assume{};
#endif
#ifdef VK_EXT_host_image_copy
  VkPhysicalDeviceHostImageCopyFeaturesEXT host_image_copy{};
#endif
#ifdef VK_KHR_workgroup_memory_explicit_layout
  VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR explicit_mem_layout{};
#endif
#ifdef VK_KHR_shader_relaxed_extended_instruction
  VkPhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR relaxed_extended_instruction{};
#endif
#ifdef VK_EXT_shader_long_vector
  VkPhysicalDeviceShaderLongVectorFeaturesEXT long_vector{};
#endif
#ifdef VK_EXT_shader_replicated_composites
  VkPhysicalDeviceShaderReplicatedCompositesFeaturesEXT replicated_composites{};
#endif
#ifdef VK_EXT_zero_initialize_device_memory
  VkPhysicalDeviceZeroInitializeDeviceMemoryFeaturesEXT zero_initialize{};
#endif
#ifdef VK_KHR_internally_synchronized_queues
  VkPhysicalDeviceInternallySynchronizedQueuesFeaturesKHR internal_queue_sync{};
#endif
};

// What FFmpeg's context points into: the device features it reads and the extension names. Freed
// with the device by free_own_vulkan().
struct OwnVulkan
{
  VkInstance instance = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  PFN_vkDestroyInstance destroy_instance = nullptr;
  PFN_vkDestroyDevice destroy_device = nullptr;
  FeatureSet enabled;
  std::vector<const char*> extensions;
};

// Where the loaded libavutil has no av_vk_get_optional_device_extensions(): the device extensions
// FFmpeg 6.1 to 9.0 enable when the device supports them (optional_device_exts in
// libavutil/hwcontext_vulkan.c), together. An extension a given version does not know is enabled
// but left unused by it. VK_NV_optical_flow, which FFmpeg 7.1 lists for its filters, is left out.
const char* const ffmpeg_device_extensions[] = {
  "VK_KHR_portability_subset",
  "VK_KHR_push_descriptor",
  "VK_KHR_sampler_ycbcr_conversion",
  "VK_EXT_descriptor_buffer",
  "VK_EXT_physical_device_drm",
  "VK_EXT_shader_atomic_float",
  "VK_KHR_cooperative_matrix",
  "VK_EXT_shader_object",
  "VK_KHR_shader_subgroup_rotate",
  "VK_KHR_shader_expect_assume",
  "VK_KHR_video_maintenance1",
  "VK_KHR_video_maintenance2",
  "VK_KHR_external_memory_fd",
  "VK_EXT_external_memory_dma_buf",
  "VK_EXT_image_drm_format_modifier",
  "VK_KHR_external_semaphore_fd",
  "VK_EXT_external_memory_host",
  "VK_KHR_video_queue",
  "VK_KHR_video_encode_queue",
  "VK_KHR_video_decode_queue",
  "VK_KHR_video_encode_h264",
  "VK_KHR_video_decode_h264",
  "VK_KHR_video_encode_h265",
  "VK_KHR_video_decode_h265",
  "VK_KHR_video_decode_vp9",
  "VK_KHR_video_encode_av1",
  "VK_KHR_video_decode_av1",
  "VK_MESA_video_decode_av1",
  "VK_EXT_host_image_copy",
  "VK_KHR_workgroup_memory_explicit_layout",
  "VK_KHR_shader_relaxed_extended_instruction",
  "VK_EXT_shader_long_vector",
  "VK_EXT_shader_replicated_composites",
  "VK_EXT_zero_initialize_device_memory",
  "VK_KHR_internally_synchronized_queues",
};

// The device extensions FFmpeg can use: its own list from av_vk_get_optional_device_extensions()
// where the libavutil this process loaded has that function, else ffmpeg_device_extensions. The
// function is looked up instead of called, so the plugin also builds and loads with a libavutil
// whose headers declare it but whose library lacks it.
std::vector<const char*> ffmpeg_extension_list()
{
  using ListFn = const char** (*)(int*);
  ListFn list_fn = nullptr;
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(&av_hwdevice_ctx_alloc), &info) && info.dli_fname) {
    if (void* avutil = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD)) {
      list_fn = reinterpret_cast<ListFn>(dlsym(avutil, "av_vk_get_optional_device_extensions"));
      dlclose(avutil);  // only drops the reference RTLD_NOLOAD took; libavutil stays loaded
    }
  }
  std::vector<const char*> names;
  if (list_fn) {
    int n = 0;
    if (const char** list = list_fn(&n)) {
      names.assign(list, list + n);  // the names themselves are FFmpeg's constants
      av_free(list);
      debug_log("vulkan: using FFmpeg's list of %d optional device extensions", n);
      return names;
    }
  }
  debug_log("vulkan: libavutil has no av_vk_get_optional_device_extensions(), using the plugin's list");
  names.assign(std::begin(ffmpeg_device_extensions), std::end(ffmpeg_device_extensions));
  return names;
}

// A zeroed Vulkan structure of type T with its sType and pNext set.
template <typename T>
T vk_struct(VkStructureType type, void* next = nullptr)
{
  T value{};
  value.sType = type;
  value.pNext = next;
  return value;
}

bool has_extension(const std::vector<const char*>& extensions, const char* name)
{
  for (const char* e : extensions) {
    if (!strcmp(e, name)) {
      return true;
    }
  }
  return false;
}

// Sets the sType of each structure in `f` and links into f.device the core ones and those whose
// extension is enabled; the timeline semaphore structure only with VK_KHR_portability_subset, as
// FFmpeg does, since Vulkan 1.2 has timeline semaphores in its core features.
void chain_features(FeatureSet& f, const std::vector<const char*>& extensions)
{
  void** tail = &f.device.pNext;
  auto link = [&tail](auto& s, VkStructureType type) {
    s.sType = type;
    s.pNext = nullptr;
    *tail = &s;
    tail = &s.pNext;
  };
  auto link_if = [&](auto& s, VkStructureType type, const char* extension) {
    if (has_extension(extensions, extension)) {
      link(s, type);
    }
  };
  f.device.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  f.device.pNext = nullptr;
  link(f.vulkan_1_1, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
  link(f.vulkan_1_2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
  link(f.vulkan_1_3, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
  link_if(f.timeline_semaphore, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, "VK_KHR_portability_subset");
#ifdef VK_EXT_descriptor_buffer
  link_if(f.descriptor_buffer, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
#endif
#ifdef VK_EXT_shader_atomic_float
  link_if(f.atomic_float, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT, VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
#endif
#ifdef VK_KHR_cooperative_matrix
  link_if(f.cooperative_matrix, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
#endif
#ifdef VK_EXT_shader_object
  link_if(f.shader_object, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
#endif
#ifdef VK_KHR_video_maintenance1
  link_if(f.video_maintenance_1, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR, VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME);
#endif
#ifdef VK_KHR_video_maintenance2
  link_if(f.video_maintenance_2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR, VK_KHR_VIDEO_MAINTENANCE_2_EXTENSION_NAME);
#endif
#ifdef VK_KHR_video_decode_vp9
  link_if(f.vp9_decode, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_DECODE_VP9_FEATURES_KHR, VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME);
#endif
#ifdef VK_KHR_video_encode_av1
  link_if(f.av1_encode, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_AV1_FEATURES_KHR, VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME);
#endif
#ifdef VK_KHR_shader_subgroup_rotate
  link_if(f.subgroup_rotate, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_ROTATE_FEATURES_KHR, VK_KHR_SHADER_SUBGROUP_ROTATE_EXTENSION_NAME);
#endif
#ifdef VK_KHR_shader_expect_assume
  link_if(f.expect_assume, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_EXPECT_ASSUME_FEATURES_KHR, VK_KHR_SHADER_EXPECT_ASSUME_EXTENSION_NAME);
#endif
#ifdef VK_EXT_host_image_copy
  link_if(f.host_image_copy, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT, VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME);
#endif
#ifdef VK_KHR_workgroup_memory_explicit_layout
  link_if(f.explicit_mem_layout, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR, VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME);
#endif
#ifdef VK_KHR_shader_relaxed_extended_instruction
  link_if(f.relaxed_extended_instruction, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_RELAXED_EXTENDED_INSTRUCTION_FEATURES_KHR, VK_KHR_SHADER_RELAXED_EXTENDED_INSTRUCTION_EXTENSION_NAME);
#endif
#ifdef VK_EXT_shader_long_vector
  link_if(f.long_vector, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_LONG_VECTOR_FEATURES_EXT, VK_EXT_SHADER_LONG_VECTOR_EXTENSION_NAME);
#endif
#ifdef VK_EXT_shader_replicated_composites
  link_if(f.replicated_composites, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_REPLICATED_COMPOSITES_FEATURES_EXT, VK_EXT_SHADER_REPLICATED_COMPOSITES_EXTENSION_NAME);
#endif
#ifdef VK_EXT_zero_initialize_device_memory
  link_if(f.zero_initialize, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_DEVICE_MEMORY_FEATURES_EXT, VK_EXT_ZERO_INITIALIZE_DEVICE_MEMORY_EXTENSION_NAME);
#endif
#ifdef VK_KHR_internally_synchronized_queues
  link_if(f.internal_queue_sync, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INTERNALLY_SYNCHRONIZED_QUEUES_FEATURES_KHR, VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME);
#endif
}

// Copies into `dst` the supported values of every feature FFmpeg 6.1 to 9.0 enables (the feature
// assignments in vulkan_device_create_internal() up to 7.1, device_features_copy_needed() from 8.0).
// Structures not in the chain are copied too and stay unused.
void copy_features(FeatureSet& dst, const FeatureSet& src)
{
#define HWDEC_COPY(field) dst.field = src.field
  HWDEC_COPY(device.features.shaderImageGatherExtended);
  HWDEC_COPY(device.features.shaderStorageImageReadWithoutFormat);
  HWDEC_COPY(device.features.shaderStorageImageWriteWithoutFormat);
  HWDEC_COPY(device.features.fragmentStoresAndAtomics);
  HWDEC_COPY(device.features.vertexPipelineStoresAndAtomics);
  HWDEC_COPY(device.features.shaderInt64);
  HWDEC_COPY(device.features.shaderInt16);
  HWDEC_COPY(device.features.shaderFloat64);
  HWDEC_COPY(vulkan_1_1.samplerYcbcrConversion);
  HWDEC_COPY(vulkan_1_1.storagePushConstant16);
  HWDEC_COPY(vulkan_1_1.storageBuffer16BitAccess);
  HWDEC_COPY(vulkan_1_1.uniformAndStorageBuffer16BitAccess);
  HWDEC_COPY(vulkan_1_2.timelineSemaphore);
  HWDEC_COPY(vulkan_1_2.scalarBlockLayout);
  HWDEC_COPY(vulkan_1_2.bufferDeviceAddress);
  HWDEC_COPY(vulkan_1_2.hostQueryReset);
  HWDEC_COPY(vulkan_1_2.storagePushConstant8);
  HWDEC_COPY(vulkan_1_2.shaderInt8);
  HWDEC_COPY(vulkan_1_2.storageBuffer8BitAccess);
  HWDEC_COPY(vulkan_1_2.uniformAndStorageBuffer8BitAccess);
  HWDEC_COPY(vulkan_1_2.shaderFloat16);
  HWDEC_COPY(vulkan_1_2.shaderBufferInt64Atomics);
  HWDEC_COPY(vulkan_1_2.shaderSharedInt64Atomics);
  HWDEC_COPY(vulkan_1_2.vulkanMemoryModel);
  HWDEC_COPY(vulkan_1_2.vulkanMemoryModelDeviceScope);
  HWDEC_COPY(vulkan_1_2.vulkanMemoryModelAvailabilityVisibilityChains);
  HWDEC_COPY(vulkan_1_2.uniformBufferStandardLayout);
  HWDEC_COPY(vulkan_1_2.runtimeDescriptorArray);
  HWDEC_COPY(vulkan_1_2.shaderSubgroupExtendedTypes);
  HWDEC_COPY(vulkan_1_2.shaderUniformBufferArrayNonUniformIndexing);
  HWDEC_COPY(vulkan_1_2.shaderSampledImageArrayNonUniformIndexing);
  HWDEC_COPY(vulkan_1_2.shaderStorageBufferArrayNonUniformIndexing);
  HWDEC_COPY(vulkan_1_2.shaderStorageImageArrayNonUniformIndexing);
  HWDEC_COPY(vulkan_1_3.dynamicRendering);
  HWDEC_COPY(vulkan_1_3.maintenance4);
  HWDEC_COPY(vulkan_1_3.synchronization2);
  HWDEC_COPY(vulkan_1_3.computeFullSubgroups);
  HWDEC_COPY(vulkan_1_3.subgroupSizeControl);
  HWDEC_COPY(vulkan_1_3.shaderZeroInitializeWorkgroupMemory);
  HWDEC_COPY(timeline_semaphore.timelineSemaphore);
#ifdef VK_EXT_descriptor_buffer
  HWDEC_COPY(descriptor_buffer.descriptorBuffer);
  HWDEC_COPY(descriptor_buffer.descriptorBufferPushDescriptors);
#endif
#ifdef VK_EXT_shader_atomic_float
  HWDEC_COPY(atomic_float.shaderBufferFloat32Atomics);
  HWDEC_COPY(atomic_float.shaderBufferFloat32AtomicAdd);
#endif
#ifdef VK_KHR_cooperative_matrix
  HWDEC_COPY(cooperative_matrix.cooperativeMatrix);
#endif
#ifdef VK_EXT_shader_object
  HWDEC_COPY(shader_object.shaderObject);
#endif
#ifdef VK_KHR_video_maintenance1
  HWDEC_COPY(video_maintenance_1.videoMaintenance1);
#endif
#ifdef VK_KHR_video_maintenance2
  HWDEC_COPY(video_maintenance_2.videoMaintenance2);
#endif
#ifdef VK_KHR_video_decode_vp9
  HWDEC_COPY(vp9_decode.videoDecodeVP9);
#endif
#ifdef VK_KHR_video_encode_av1
  HWDEC_COPY(av1_encode.videoEncodeAV1);
#endif
#ifdef VK_KHR_shader_subgroup_rotate
  HWDEC_COPY(subgroup_rotate.shaderSubgroupRotate);
#endif
#ifdef VK_KHR_shader_expect_assume
  HWDEC_COPY(expect_assume.shaderExpectAssume);
#endif
#ifdef VK_EXT_host_image_copy
  HWDEC_COPY(host_image_copy.hostImageCopy);
#endif
#ifdef VK_KHR_workgroup_memory_explicit_layout
  HWDEC_COPY(explicit_mem_layout.workgroupMemoryExplicitLayout);
  HWDEC_COPY(explicit_mem_layout.workgroupMemoryExplicitLayoutScalarBlockLayout);
  HWDEC_COPY(explicit_mem_layout.workgroupMemoryExplicitLayout8BitAccess);
  HWDEC_COPY(explicit_mem_layout.workgroupMemoryExplicitLayout16BitAccess);
#endif
#ifdef VK_KHR_shader_relaxed_extended_instruction
  HWDEC_COPY(relaxed_extended_instruction.shaderRelaxedExtendedInstruction);
#endif
#ifdef VK_EXT_shader_long_vector
  HWDEC_COPY(long_vector.longVector);
#endif
#ifdef VK_EXT_shader_replicated_composites
  HWDEC_COPY(replicated_composites.shaderReplicatedComposites);
#endif
#ifdef VK_EXT_zero_initialize_device_memory
  HWDEC_COPY(zero_initialize.zeroInitializeDeviceMemory);
#endif
#ifdef VK_KHR_internally_synchronized_queues
  HWDEC_COPY(internal_queue_sync.internallySynchronizedQueues);
#endif
#undef HWDEC_COPY
}

void free_own_vulkan(AVHWDeviceContext* ctx)
{
  auto* own = static_cast<OwnVulkan*>(ctx->user_opaque);
  if (own->device != VK_NULL_HANDLE) {
    own->destroy_device(own->device, nullptr);
  }
  if (own->instance != VK_NULL_HANDLE) {
    own->destroy_instance(own->instance, nullptr);
  }
  delete own;
}

// The queue family with `flag` and the fewest other capabilities, plus one for each time it was
// already picked (`used`), as FFmpeg's pick_queue_family() chooses it, so that compute, transfer and
// decode get separate families where the device has them. On an NVIDIA T1200 that puts transfers
// on the transfer-only family instead of the decode family, where downloads would wait behind
// decoding. Graphics and compute families can always transfer, whether they report it or not.
int pick_queue_family(const std::vector<VkQueueFamilyProperties>& families, std::vector<int>& used, VkQueueFlags flag)
{
  int best = -1;
  unsigned best_score = ~0u;
  for (size_t i = 0; i < families.size(); i++) {
    VkQueueFlags flags = families[i].queueFlags;
    if ((flag & VK_QUEUE_TRANSFER_BIT) && (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
      flags |= VK_QUEUE_TRANSFER_BIT;
    }
    if (flags & flag) {
      unsigned score = unsigned(__builtin_popcount(flags)) + unsigned(used[i]);
      if (score < best_score) {
        best = int(i);
        best_score = score;
      }
    }
  }
  if (best >= 0) {
    used[size_t(best)]++;
  }
  return best;
}

// Creates the Vulkan instance and, on the requested device (an index into the instance's devices)
// or else the first device whose video decode queue family decodes H.265, the device described
// above. Returns an FFmpeg device context, or nullptr.
AVBufferRef* create_own_vulkan(const char* requested_device, std::string& description)
{
  static void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!loader) {
    debug_log("vulkan: cannot load libvulkan.so.1 (%s)", dlerror());
    return nullptr;
  }
  auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
  if (!get_proc) {
    return nullptr;
  }
#define HWDEC_VK_FN(instance, name) reinterpret_cast<PFN_##name>(get_proc(instance, #name))

  auto own = std::make_unique<OwnVulkan>();
  auto app = vk_struct<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
  app.pApplicationName = "libheif-hwdec";
  app.pEngineName = "libavutil";
  app.apiVersion = VK_API_VERSION_1_3;
  auto instance_info = vk_struct<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
  instance_info.pApplicationInfo = &app;
  auto create_instance = HWDEC_VK_FN(VK_NULL_HANDLE, vkCreateInstance);
  if (!create_instance || create_instance(&instance_info, nullptr, &own->instance) != VK_SUCCESS) {
    debug_log("vulkan: cannot create an instance");
    return nullptr;
  }
  VkInstance inst = own->instance;
  own->destroy_instance = HWDEC_VK_FN(inst, vkDestroyInstance);
  auto enumerate_devices = HWDEC_VK_FN(inst, vkEnumeratePhysicalDevices);
  auto get_properties = HWDEC_VK_FN(inst, vkGetPhysicalDeviceProperties);
  auto get_memory_properties = HWDEC_VK_FN(inst, vkGetPhysicalDeviceMemoryProperties);
  auto get_families2 = HWDEC_VK_FN(inst, vkGetPhysicalDeviceQueueFamilyProperties2);
  auto get_features2 = HWDEC_VK_FN(inst, vkGetPhysicalDeviceFeatures2);
  auto enumerate_extensions = HWDEC_VK_FN(inst, vkEnumerateDeviceExtensionProperties);
  auto create_device = HWDEC_VK_FN(inst, vkCreateDevice);
  own->destroy_device = HWDEC_VK_FN(inst, vkDestroyDevice);
  if (!enumerate_devices || !get_properties || !get_memory_properties || !get_families2 || !get_features2 ||
      !enumerate_extensions || !create_device || !own->destroy_device || !own->destroy_instance) {
    own->destroy_instance(inst, nullptr);
    return nullptr;
  }
#undef HWDEC_VK_FN

  // Ends with the instance destroyed when no device is created; own is freed on return.
  auto fail = [&](const char* what) -> AVBufferRef* {
    debug_log("vulkan: %s", what);
    own->destroy_instance(inst, nullptr);
    return nullptr;
  };

  uint32_t n_devices = 0;
  enumerate_devices(inst, &n_devices, nullptr);
  std::vector<VkPhysicalDevice> devices(n_devices);
  enumerate_devices(inst, &n_devices, devices.data());

  VkPhysicalDevice chosen = VK_NULL_HANDLE;
  std::vector<VkQueueFamilyProperties> families;
  std::vector<VkVideoCodecOperationFlagsKHR> family_codecs;
  for (uint32_t i = 0; i < n_devices; i++) {
    if (requested_device && std::to_string(i) != requested_device) {
      continue;
    }
    VkPhysicalDeviceProperties props{};
    get_properties(devices[i], &props);
    uint32_t n = 0;
    get_families2(devices[i], &n, nullptr);
    std::vector<VkQueueFamilyVideoPropertiesKHR> video(n, vk_struct<VkQueueFamilyVideoPropertiesKHR>(VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR));
    std::vector<VkQueueFamilyProperties2> props2(n, vk_struct<VkQueueFamilyProperties2>(VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2));
    for (uint32_t k = 0; k < n; k++) {
      props2[k].pNext = &video[k];
    }
    get_families2(devices[i], &n, props2.data());
    bool decodes_h265 = false;
    for (uint32_t k = 0; k < n; k++) {
      decodes_h265 |= (props2[k].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) &&
                      (video[k].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR);
    }
    if (!decodes_h265) {
      debug_log("vulkan: device %u (%s) has no H.265 video decode queue", i, props.deviceName);
      continue;
    }
    chosen = devices[i];
    for (uint32_t k = 0; k < n; k++) {
      families.push_back(props2[k].queueFamilyProperties);
      family_codecs.push_back(video[k].videoCodecOperations);
    }
    description = "vulkan " + std::to_string(i) + " (" + props.deviceName + ")";
    break;
  }
  if (chosen == VK_NULL_HANDLE) {
    return fail("no device with an H.265 video decode queue");
  }

  // Extensions first: they decide which feature structures are in the chain.
  OwnVulkan& o = *own;
  uint32_t n_ext = 0;
  enumerate_extensions(chosen, nullptr, &n_ext, nullptr);
  std::vector<VkExtensionProperties> supported_ext(n_ext);
  enumerate_extensions(chosen, nullptr, &n_ext, supported_ext.data());
  const std::vector<const char*> wanted = ffmpeg_extension_list();
  // FFmpeg enables VK_EXT_host_image_copy only when the whole of the largest device-local memory
  // heap is also visible to the CPU (resizable BAR; vulkan_device_has_rebar(), with its 1 kB
  // tolerance).
  VkPhysicalDeviceMemoryProperties memory{};
  get_memory_properties(chosen, &memory);
  VkDeviceSize max_vram = 0, max_visible_vram = 0;
  for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
    const VkMemoryType& type = memory.memoryTypes[i];
    if (type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
      max_vram = std::max(max_vram, memory.memoryHeaps[type.heapIndex].size);
      if (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        max_visible_vram = std::max(max_visible_vram, memory.memoryHeaps[type.heapIndex].size);
      }
    }
  }
  const bool rebar = max_vram - max_visible_vram < 1024;
  for (const char* name : wanted) {
    if (!rebar && !strcmp(name, "VK_EXT_host_image_copy")) {
      continue;
    }
    // Queues of a device with this extension are created internally synchronized, and FFmpeg can
    // only fetch them as such where it has AVVulkanDeviceContext.queue_flags.
    if (!HWDEC_VK_QUEUE_FLAGS && !strcmp(name, "VK_KHR_internally_synchronized_queues")) {
      continue;
    }
    for (const auto& e : supported_ext) {
      if (!strcmp(e.extensionName, name)) {
        o.extensions.push_back(name);
        break;
      }
    }
  }

  // Device features: what FFmpeg enables when the device supports it. Timeline semaphores are the
  // one feature FFmpeg cannot work without.
  FeatureSet supported;
  chain_features(supported, o.extensions);
  get_features2(chosen, &supported.device);
  if (!supported.vulkan_1_2.timelineSemaphore) {
    return fail("device has no timeline semaphores, which FFmpeg requires");
  }
  chain_features(o.enabled, o.extensions);
  copy_features(o.enabled, supported);

  // One queue each for compute (FFmpeg requires a compute family), transfers (downloads) and
  // decoding, from families picked as FFmpeg picks them; a family serving several roles is
  // created once.
  std::vector<int> used(families.size(), 0);
  int comp_index = pick_queue_family(families, used, VK_QUEUE_COMPUTE_BIT);
  int tx_index = pick_queue_family(families, used, VK_QUEUE_TRANSFER_BIT);
  int dec_index = pick_queue_family(families, used, VK_QUEUE_VIDEO_DECODE_BIT_KHR);
  if (tx_index < 0) {
    tx_index = comp_index;
  }
  if (comp_index < 0 || dec_index < 0) {
    return fail("device lacks a compute or video decode queue family");
  }
  VkDeviceQueueCreateFlags queue_flags = 0;
#if HWDEC_VK_QUEUE_FLAGS
  if (has_extension(o.extensions, "VK_KHR_internally_synchronized_queues")) {
    queue_flags = VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR;
  }
#endif
  const float priority = 1.0f;
  std::vector<VkDeviceQueueCreateInfo> queues;
  for (int family : {comp_index, tx_index, dec_index}) {
    bool listed = false;
    for (const auto& q : queues) {
      listed |= int(q.queueFamilyIndex) == family;
    }
    if (!listed) {
      auto q = vk_struct<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
      q.flags = queue_flags;
      q.queueFamilyIndex = uint32_t(family);
      q.queueCount = 1;
      q.pQueuePriorities = &priority;
      queues.push_back(q);
    }
  }

  auto device_info = vk_struct<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &o.enabled.device);
  device_info.queueCreateInfoCount = uint32_t(queues.size());
  device_info.pQueueCreateInfos = queues.data();
  device_info.enabledExtensionCount = uint32_t(o.extensions.size());
  device_info.ppEnabledExtensionNames = o.extensions.data();
  VkResult vr = create_device(chosen, &device_info, nullptr, &o.device);
  if (vr != VK_SUCCESS) {
    debug_log("vulkan: vkCreateDevice failed (VkResult %d)", int(vr));
    return fail("cannot create the device");
  }

  AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
  if (!ref) {
    o.destroy_device(o.device, nullptr);
    return fail("out of memory");
  }
  auto* ctx = reinterpret_cast<AVHWDeviceContext*>(ref->data);
  auto* hw = static_cast<AVVulkanDeviceContext*>(ctx->hwctx);
  hw->get_proc_addr = get_proc;
  hw->inst = inst;
  hw->phys_dev = chosen;
  hw->act_dev = o.device;
  hw->device_features = o.enabled.device;
  hw->enabled_dev_extensions = o.extensions.data();
  hw->nb_enabled_dev_extensions = int(o.extensions.size());
#if HWDEC_VK_FIXED_QUEUES
  // The per-type queue fields: the only ones before libavutil 59.34, and still required (compute
  // and transfer) by FFmpeg 7.1 to 8.1 alongside qf[].
#if HWDEC_VK_QF_LIST
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  hw->queue_family_index = -1;
  hw->nb_graphics_queues = 0;
  hw->queue_family_comp_index = comp_index;
  hw->nb_comp_queues = 1;
  hw->queue_family_tx_index = tx_index;
  hw->nb_tx_queues = 1;
  hw->queue_family_decode_index = dec_index;
  hw->nb_decode_queues = 1;
  hw->queue_family_encode_index = -1;
  hw->nb_encode_queues = 0;
#if HWDEC_VK_QF_LIST
#pragma GCC diagnostic pop
#endif
#endif
#if HWDEC_VK_QF_LIST
  // qf[]: each family once, with every role it serves and, for video families, its codecs.
  hw->nb_qf = 0;
  for (auto [family, flag] : {std::pair<int, VkQueueFlagBits>{comp_index, VK_QUEUE_COMPUTE_BIT},
                              {tx_index, VK_QUEUE_TRANSFER_BIT}, {dec_index, VK_QUEUE_VIDEO_DECODE_BIT_KHR}}) {
    int i = 0;
    while (i < hw->nb_qf && hw->qf[i].idx != family) {
      i++;
    }
    if (i == hw->nb_qf) {
      hw->qf[i] = AVVulkanDeviceQueueFamily{};
      hw->qf[i].idx = family;
      hw->qf[i].num = 1;
      hw->nb_qf++;
    }
    hw->qf[i].flags = VkQueueFlagBits(hw->qf[i].flags | flag);
    if (flag == VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
      hw->qf[i].video_caps = VkVideoCodecOperationFlagBitsKHR(family_codecs[size_t(family)]);
    }
  }
#endif
#if HWDEC_VK_QUEUE_FLAGS
  hw->queue_flags = queue_flags;
#endif
  ctx->user_opaque = own.release();
  ctx->free = free_own_vulkan;
  int ret = av_hwdevice_ctx_init(ref);
  if (ret < 0) {
    debug_log("vulkan: FFmpeg does not accept the device (%s)", error_text(ret).c_str());
    av_buffer_unref(&ref);  // calls free_own_vulkan()
    return nullptr;
  }
  return ref;
}

#endif

// Opens the requested Vulkan device (an index), or else the first of devices 0-7 with a video
// decode queue. FFmpeg's own default is device 0, which may have none: Mesa's Intel driver only
// exposes video decoding with ANV_DEBUG=video-decode up to at least Mesa 25.3.
bool open_vulkan(const char* requested_device, HwDevice& out)
{
#if HWDEC_OWN_VULKAN_DEVICE
  std::string description;
  if (AVBufferRef* ref = create_own_vulkan(requested_device, description)) {
    out.device_ref = ref;
    out.type = AV_HWDEVICE_TYPE_VULKAN;
    out.description = description;
    return true;
  }
  return false;
#else
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
#endif
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

// True when the machine has a GPU device node: a DRM render node, or NVIDIA's /dev/nvidia0 for its
// driver without the nvidia-drm module. Every hardware Vulkan driver needs one; without it only a
// software Vulkan driver such as lavapipe could answer, which decodes no video.
bool has_gpu_device_node()
{
  if (access("/dev/nvidia0", F_OK) == 0) {
    return true;
  }
  DIR* dir = opendir("/dev/dri");
  if (!dir) {
    return false;
  }
  bool found = false;
  while (dirent* entry = readdir(dir)) {
    if (strncmp(entry->d_name, "renderD", 7) == 0) {
      found = true;
      break;
    }
  }
  closedir(dir);
  return found;
}

// The backends tried when LIBHEIF_HWDEC_BACKEND is not set. vulkan comes last, and only with
// FFmpeg 8.1 or later at run time (libavcodec 62.28.100; a binary built against FFmpeg 8.0 also
// runs with 8.1, whose libavcodec has the same major version): on an NVIDIA T1200, Vulkan decoded
// HEIC photos about 37% slower with FFmpeg 6.1 to 8.0.1 than with 8.1.0, 8.1.2 and 9.0.2, where it
// was as fast as VDPAU and CUDA. It is also left out when there is no GPU device node, which saves
// creating a Vulkan instance on machines without a GPU.
std::string default_backends()
{
  std::string list = "vaapi,vdpau,cuda";
#if HWDEC_HAVE_VULKAN
  if (avcodec_version() < AV_VERSION_INT(62, 28, 100)) {
    debug_log("vulkan left out of the default backends: FFmpeg's libavcodec %u.%u.%u is older than 8.1's 62.28.100",
              AV_VERSION_MAJOR(avcodec_version()), AV_VERSION_MINOR(avcodec_version()), AV_VERSION_MICRO(avcodec_version()));
  }
  else if (!has_gpu_device_node()) {
    debug_log("vulkan left out of the default backends: no GPU device node (/dev/dri/renderD*, /dev/nvidia0)");
  }
  else {
    list += ",vulkan";
  }
#endif
  return list;
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

  for (const std::string& name : split_list(backend_env && *backend_env ? std::string(backend_env) : default_backends())) {
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
