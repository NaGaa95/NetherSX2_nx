cmake_minimum_required(VERSION 3.22)
project(NetherSX2LauncherDependencies LANGUAGES C CXX)

include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBKRB5 OFF CACHE BOOL "" FORCE)
set(ENABLE_GSSAPI OFF CACHE BOOL "" FORCE)

FetchContent_Declare(libsmb2
  GIT_REPOSITORY https://github.com/ITotalJustice/libsmb2.git
  GIT_TAG 867beea093f2863dfddea01945204f724afd6c45)
FetchContent_MakeAvailable(libsmb2)
target_compile_options(smb2 PRIVATE -UNEED_READV -UNEED_WRITEV)
target_compile_definitions(smb2 PRIVATE AES128_ECB_encrypt=smb2_AES128_ECB_encrypt)

set(USBHSFS_GPL OFF CACHE BOOL "" FORCE)
set(USBHSFS_NTFS OFF CACHE BOOL "" FORCE)
set(USBHSFS_EXT4 OFF CACHE BOOL "" FORCE)
set(USBHSFS_SXOS_DISABLE ON CACHE BOOL "" FORCE)
set(USBHSFS_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(libusbhsfs
  GIT_REPOSITORY https://github.com/ITotalJustice/libusbhsfs.git
  GIT_TAG 625269b7725a6e2a3f2724e8d45b602c1b20ead5)
FetchContent_MakeAvailable(libusbhsfs)

# Keep UASP support reproducible while libusbhsfs remains pinned.  The
# transport implementation supports command/status IUs, tagged commands,
# streams when the device advertises them, timeout recovery and BOT fallback.
set(_libusbhsfs_pinned_revision 625269b7725a6e2a3f2724e8d45b602c1b20ead5)
find_package(Git REQUIRED)
execute_process(
  COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${libusbhsfs_SOURCE_DIR}" rev-parse HEAD
  WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
  RESULT_VARIABLE _libusbhsfs_revision_result
  OUTPUT_VARIABLE _libusbhsfs_revision
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT _libusbhsfs_revision_result EQUAL 0 OR
   NOT "${_libusbhsfs_revision}" STREQUAL "${_libusbhsfs_pinned_revision}")
  message(FATAL_ERROR "libusbhsfs source is not the pinned ${_libusbhsfs_pinned_revision} revision")
endif()

set(_libusbhsfs_uasp_patches
  "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs-0001-add-uasp-transport-hooks.patch"
  "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs-0002-fallback-to-bot-interface.patch"
  "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs-0003-cleanup-after-uasp-reset.patch")
foreach(_patch IN LISTS _libusbhsfs_uasp_patches)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${libusbhsfs_SOURCE_DIR}"
            apply --ignore-space-change --check "${_patch}"
    WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
    RESULT_VARIABLE _patch_check OUTPUT_QUIET ERROR_QUIET)
  if(_patch_check EQUAL 0)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${libusbhsfs_SOURCE_DIR}"
              apply --ignore-space-change "${_patch}"
      WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
      RESULT_VARIABLE _patch_result)
    if(NOT _patch_result EQUAL 0)
      message(FATAL_ERROR "Failed to apply ${_patch}")
    endif()
  else()
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${libusbhsfs_SOURCE_DIR}"
              apply --ignore-space-change --reverse --check "${_patch}"
      WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
      RESULT_VARIABLE _reverse_check OUTPUT_QUIET ERROR_QUIET)
    if(NOT _reverse_check EQUAL 0)
      message(FATAL_ERROR "Pinned libusbhsfs no longer matches ${_patch}")
    endif()
  endif()
endforeach()

target_sources(libusbhsfs PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/usbhsfs_uasp.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/usbhsfs_uasp.h")
target_include_directories(libusbhsfs PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${libusbhsfs_SOURCE_DIR}/source")
target_compile_definitions(libusbhsfs PRIVATE get_fattime=usbhsfs_get_fattime)
