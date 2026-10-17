/*
 * libheif-hwdec - hardware HEVC and AV1 decoder plugin for libheif
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Decodes HEIF files in one process, the way an image viewer does, and prints for each file the
 * decode time and an FNV-1a hash of the decoded Y, Cb and Cr planes in the codec's own format
 * (no colour conversion), so the output of two decoders can be compared exactly.
 *
 * Usage: heif-hwdec-bench [-d DECODER_ID] [-r REPEATS] FILE...
 *        heif-hwdec-bench --plugin-dir
 *   -d            libheif decoder id, e.g. hwdec or libde265 (default: libheif's choice)
 *   -r            decode the whole list this many times (default 1)
 *        heif-hwdec-bench --has-decoder ID heic|avif
 *   --plugin-dir  print the directory libheif loads plugins from when LIBHEIF_PLUGIN_PATH is unset
 *   --has-decoder exit with 0 if libheif has a decoder with that id that accepts the format
 *
 * Output, one line per decode:
 *   <file> <milliseconds> <width>x<height> <hash> nclx=<primaries>/<transfer>/<matrix>/<full range>
 * nclx is the colour profile attached to the decoded image, or "nclx=none".
 */

#include <libheif/heif.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

uint64_t fnv1a(uint64_t hash, const uint8_t* data, size_t size)
{
  for (size_t i = 0; i < size; i++) {
    hash = (hash ^ data[i]) * 0x100000001b3ULL;
  }
  return hash;
}

uint64_t hash_planes(const heif_image* image)
{
  uint64_t hash = 0xcbf29ce484222325ULL;
  const heif_channel channels[] = {heif_channel_Y, heif_channel_Cb, heif_channel_Cr, heif_channel_Alpha};
  for (heif_channel channel : channels) {
    if (!heif_image_has_channel(image, channel)) {
      continue;
    }
    int stride = 0;
    const uint8_t* plane = heif_image_get_plane_readonly(image, channel, &stride);
    int width = heif_image_get_width(image, channel);
    int height = heif_image_get_height(image, channel);
    int bytes = (heif_image_get_bits_per_pixel_range(image, channel) + 7) / 8;
    for (int y = 0; y < height; y++) {
      hash = fnv1a(hash, plane + size_t(y) * stride, size_t(width) * bytes);
    }
  }
  return hash;
}

} // namespace

int main(int argc, char** argv)
{
  const char* decoder_id = nullptr;
  int repeats = 1;
  std::vector<const char*> files;

  if (argc == 2 && strcmp(argv[1], "--plugin-dir") == 0) {
    unsetenv("LIBHEIF_PLUGIN_PATH");
    const char* const* dirs = heif_get_plugin_directories();
    const char* last = nullptr;
    for (const char* const* d = dirs; d && *d; d++) {
      last = *d;
    }
    if (last) {
      printf("%s\n", last);
    }
    heif_free_plugin_directories(dirs);
    return last ? 0 : 1;
  }

  if (argc == 4 && strcmp(argv[1], "--has-decoder") == 0) {
    heif_init(nullptr);
    heif_compression_format format = strcmp(argv[3], "avif") == 0 ? heif_compression_AV1 : heif_compression_HEVC;
    const heif_decoder_descriptor* descriptors[32];
    int n = heif_get_decoder_descriptors(format, descriptors, 32);
    bool found = false;
    for (int i = 0; i < n; i++) {
      found = found || strcmp(heif_decoder_descriptor_get_id_name(descriptors[i]), argv[2]) == 0;
    }
    heif_deinit();
    return found ? 0 : 1;
  }

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
      decoder_id = argv[++i];
    }
    else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
      repeats = atoi(argv[++i]);
    }
    else {
      files.push_back(argv[i]);
    }
  }
  if (files.empty() || repeats < 1) {
    fprintf(stderr, "usage: %s [-d DECODER_ID] [-r REPEATS] FILE...\n", argv[0]);
    return 2;
  }

  heif_init(nullptr);
  int failures = 0;

  for (int r = 0; r < repeats; r++) {
    for (const char* file : files) {
      auto start = std::chrono::steady_clock::now();

      heif_context* ctx = heif_context_alloc();
      heif_image_handle* handle = nullptr;
      heif_image* image = nullptr;
      heif_decoding_options* options = heif_decoding_options_alloc();
      options->decoder_id = decoder_id;

      heif_error err = heif_context_read_from_file(ctx, file, nullptr);
      if (!err.code) {
        err = heif_context_get_primary_image_handle(ctx, &handle);
      }
      if (!err.code) {
        err = heif_decode_image(handle, &image, heif_colorspace_undefined, heif_chroma_undefined, options);
      }

      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

      if (err.code) {
        printf("%s FAILED %s\n", file, err.message);
        failures++;
      }
      else {
        char nclx_text[64] = "none";
        heif_color_profile_nclx* nclx = nullptr;
        if (heif_image_get_nclx_color_profile(image, &nclx).code == heif_error_Ok && nclx) {
          snprintf(nclx_text, sizeof(nclx_text), "%d/%d/%d/%d", int(nclx->color_primaries),
                   int(nclx->transfer_characteristics), int(nclx->matrix_coefficients), int(nclx->full_range_flag));
          heif_nclx_color_profile_free(nclx);
        }
        printf("%s %.1f %dx%d %016llx nclx=%s\n", file, ms, heif_image_get_primary_width(image),
               heif_image_get_primary_height(image), (unsigned long long) hash_planes(image), nclx_text);
      }
      fflush(stdout);

      heif_image_release(image);
      heif_image_handle_release(handle);
      heif_decoding_options_free(options);
      heif_context_free(ctx);
    }
  }

  heif_deinit();
  return failures ? 1 : 0;
}
