cmake_minimum_required(VERSION 3.22)
project(NetherSX2PatchValidation LANGUAGES C)

find_path(EGL_INCLUDE_DIR EGL/egl.h
  HINTS "$ENV{DEVKITPRO}/portlibs/switch/include" REQUIRED)

add_executable(core_patch_validation
  validation.c
  ../../source/hooks/patches.c
  ../../source/prefs.c)
target_compile_features(core_patch_validation PRIVATE c_std_11)
target_compile_definitions(core_patch_validation PRIVATE EGL_NO_X11)
target_include_directories(core_patch_validation PRIVATE "${EGL_INCLUDE_DIR}")

enable_testing()
add_test(NAME core_patch_validation COMMAND core_patch_validation)

foreach(renderer IN ITEMS gl vk)
  add_executable(resolution_${renderer}_validation
    resolution.c
    ../../source/hooks/gs.c)
  target_compile_features(resolution_${renderer}_validation PRIVATE c_std_11)
  target_compile_definitions(resolution_${renderer}_validation PRIVATE EGL_NO_X11)
  target_include_directories(resolution_${renderer}_validation PRIVATE "${EGL_INCLUDE_DIR}")
  if(renderer STREQUAL "vk")
    target_compile_definitions(resolution_${renderer}_validation PRIVATE USE_VULKAN)
  endif()
  add_test(NAME resolution_${renderer}_validation COMMAND resolution_${renderer}_validation)
endforeach()
