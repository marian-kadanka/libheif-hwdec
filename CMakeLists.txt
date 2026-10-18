cmake_minimum_required(VERSION 3.16)

project(libheif-hwdec VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

# libheif 1.15 introduced decoder plugin API version 3, the version this plugin implements.
# libheif's CMake package only accepts an exactly matching version request, so the minimum is
# checked here instead of in find_package().
find_package(libheif REQUIRED CONFIG)
if(libheif_VERSION VERSION_LESS 1.15)
  message(FATAL_ERROR "libheif ${libheif_VERSION} found, 1.15 or newer is required")
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec>=59.37 libavutil>=57.28)
pkg_check_modules(LIBVA IMPORTED_TARGET libva)
# Only the headers: FFmpeg's hwcontext_vulkan.h needs them, and the plugin loads libvulkan.so.1
# itself at run time to create its Vulkan device (src/hw_device.cc), with FFmpeg 6.1 and later.
pkg_check_modules(VULKAN vulkan)

# Which plugin API the libheif headers offer. libheif 1.21 and later refuse plugins older than API
# version 5; libheif 1.19 and older do not know version 5. Version 6 adds security limits.
include(CheckCXXSourceCompiles)
get_target_property(HWDEC_HEIF_INCLUDES heif INTERFACE_INCLUDE_DIRECTORIES)
set(CMAKE_REQUIRED_INCLUDES ${HWDEC_HEIF_INCLUDES})
set(CMAKE_REQUIRED_LIBRARIES heif)
check_cxx_source_compiles("
  #include <libheif/heif_plugin.h>
  int main() { heif_decoder_plugin_options o{}; return o.limits != nullptr; }" HWDEC_HEIF_PLUGIN_API_6)
check_cxx_source_compiles("
  #include <libheif/heif_plugin.h>
  int main() { heif_decoder_plugin p{}; return p.push_data2 != nullptr; }" HWDEC_HEIF_PLUGIN_API_5)
check_cxx_source_compiles("
  #include <libheif/heif_plugin.h>
  int main() { heif_decoder_plugin p{}; return p.decode_next_image != nullptr; }" HWDEC_HEIF_PLUGIN_API_4)
check_cxx_source_compiles("
  #include <libheif/heif.h>
  int main() { return heif_image_add_plane_safe != nullptr; }" HWDEC_HAVE_ADD_PLANE_SAFE)
if(HWDEC_HEIF_PLUGIN_API_6)
  set(HWDEC_PLUGIN_API 6)
elseif(HWDEC_HEIF_PLUGIN_API_5)
  set(HWDEC_PLUGIN_API 5)
elseif(HWDEC_HEIF_PLUGIN_API_4)
  set(HWDEC_PLUGIN_API 4)
else()
  set(HWDEC_PLUGIN_API 3)
endif()
if(NOT HWDEC_HAVE_ADD_PLANE_SAFE)
  set(HWDEC_HAVE_ADD_PLANE_SAFE 0)
endif()
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)
message(STATUS "libheif ${libheif_VERSION}: building decoder plugin API version ${HWDEC_PLUGIN_API}")

# The directory this libheif loads plugins from, as libheif itself reports it. Distributions
# differ: Debian uses <libdir>/libheif/plugins, Arch /usr/lib/libheif.
set(HWDEC_DEFAULT_PLUGIN_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/libheif/plugins")
if(NOT CMAKE_CROSSCOMPILING)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/plugin_dir.cc" "
    #include <libheif/heif.h>
    #include <cstdio>
    int main() {
      const char* const* dirs = heif_get_plugin_directories();
      const char* last = nullptr;
      for (const char* const* d = dirs; d && *d; d++) last = *d;
      if (last) printf(\"%s\", last);
      heif_free_plugin_directories(dirs);
      return last ? 0 : 1;
    }")
  set(HWDEC_SAVED_PLUGIN_PATH "$ENV{LIBHEIF_PLUGIN_PATH}")
  unset(ENV{LIBHEIF_PLUGIN_PATH})
  try_run(HWDEC_PLUGIN_DIR_RUN HWDEC_PLUGIN_DIR_COMPILE "${CMAKE_CURRENT_BINARY_DIR}/plugin_dir"
          SOURCES "${CMAKE_CURRENT_BINARY_DIR}/plugin_dir.cc"
          LINK_LIBRARIES heif
          RUN_OUTPUT_VARIABLE HWDEC_DETECTED_PLUGIN_DIR)
  set(ENV{LIBHEIF_PLUGIN_PATH} "${HWDEC_SAVED_PLUGIN_PATH}")
  if(HWDEC_PLUGIN_DIR_COMPILE AND HWDEC_PLUGIN_DIR_RUN EQUAL 0 AND HWDEC_DETECTED_PLUGIN_DIR)
    set(HWDEC_DEFAULT_PLUGIN_DIR "${HWDEC_DETECTED_PLUGIN_DIR}")
  endif()
endif()
set(HWDEC_PLUGIN_DIR "${HWDEC_DEFAULT_PLUGIN_DIR}" CACHE PATH "Directory libheif loads plugins from")
message(STATUS "Plugin install directory: ${HWDEC_PLUGIN_DIR}")

# libheif picks the decoder with the highest priority: libde265 registers HEVC at 100, aom AV1 at
# 100 and dav1d AV1 at 150.
set(HWDEC_PLUGIN_PRIORITY 110 CACHE STRING "Priority returned for HEVC when a GPU is available")
set(HWDEC_AV1_PLUGIN_PRIORITY 160 CACHE STRING "Priority returned for AV1 when the GPU decodes it")
option(HWDEC_BUILD_TESTS "Build the test programs" ON)

set(HWDEC_HAVE_LIBVA ${LIBVA_FOUND})
if(NOT LIBVA_FOUND)
  message(STATUS "libva not found: VAAPI render node probing disabled, CUDA only")
  set(HWDEC_HAVE_LIBVA 0)
else()
  set(HWDEC_HAVE_LIBVA 1)
endif()
if(VULKAN_FOUND)
  set(HWDEC_HAVE_VULKAN 1)
else()
  message(STATUS "Vulkan headers not found: the vulkan backend uses FFmpeg's default device")
  set(HWDEC_HAVE_VULKAN 0)
endif()
configure_file(src/config.h.in "${CMAKE_CURRENT_BINARY_DIR}/config.h")

add_library(hwdec_core STATIC src/annexb.cc src/av1_obu.cc src/frame_copy.cc)
set_target_properties(hwdec_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(hwdec_core PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(hwdec_core PUBLIC heif PkgConfig::FFMPEG)

# libheif dlopen()s every *.so in its plugin directory and looks up `plugin_info`.
add_library(heif-hwdec MODULE src/plugin.cc src/hw_device.cc)
target_link_libraries(heif-hwdec PRIVATE hwdec_core)
if(LIBVA_FOUND)
  target_link_libraries(heif-hwdec PRIVATE PkgConfig::LIBVA)
endif()
if(VULKAN_FOUND)
  target_include_directories(heif-hwdec PRIVATE ${VULKAN_INCLUDE_DIRS})
endif()
# libheif unloads all plugins on its last heif_deinit(). Keeping this one mapped keeps the GPU
# device and the loaded VAAPI driver alive between images instead of reopening them each time.
target_link_options(heif-hwdec PRIVATE "LINKER:-z,nodelete" "LINKER:--no-undefined"
                    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/plugin.map")
set_target_properties(heif-hwdec PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin.map")
target_compile_options(heif-hwdec PRIVATE -Wall -Wextra)
install(TARGETS heif-hwdec LIBRARY DESTINATION "${HWDEC_PLUGIN_DIR}")

if(HWDEC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
