add_executable(heif-hwdec-bench heif-hwdec-bench.cc)
target_link_libraries(heif-hwdec-bench PRIVATE heif)

add_executable(test_annexb test_annexb.cc)
target_link_libraries(test_annexb PRIVATE hwdec_core)
add_test(NAME annexb COMMAND test_annexb)

add_executable(test_av1_obu test_av1_obu.cc)
target_link_libraries(test_av1_obu PRIVATE hwdec_core)
add_test(NAME av1_obu COMMAND test_av1_obu)

add_executable(test_frame_copy test_frame_copy.cc)
target_link_libraries(test_frame_copy PRIVATE hwdec_core)
add_test(NAME frame_copy COMMAND test_frame_copy)

# End-to-end comparison against libde265 on the files in tests/data/, once per GPU backend.
# Labelled "gpu": they need a GPU and are skipped (exit code 77) when it is missing.
file(GLOB HWDEC_TEST_FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/*.heic" "${CMAKE_CURRENT_SOURCE_DIR}/data/*.avif")
foreach(backend vaapi cuda vulkan)
  add_test(NAME compare_${backend}
           COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/compare-decoders.sh" $<TARGET_FILE:heif-hwdec-bench>
                   $<TARGET_FILE_DIR:heif-hwdec> ${backend} ${HWDEC_TEST_FILES})
  set_tests_properties(compare_${backend} PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
endforeach()
