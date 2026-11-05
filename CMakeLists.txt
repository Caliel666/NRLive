cmake_minimum_required(VERSION 3.24)
project(NRLive LANGUAGES CXX)
include(CTest)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/utf-8) # sources are UTF-8 (HUD uses symbols such as the degree sign)
endif()

# AMD FSR SDK 2.3.0. Headers come from the SDK source archive; signed runtime
# providers come from the release's prebuilt package.
set(FFX_SDK "" CACHE PATH "AMD FSR SDK 2.3.0 source root")
set(FFX_BIN "" CACHE PATH "Directory containing the signed FSR SDK 2.3.0 DX12 DLLs")
if(NOT FFX_SDK)
  message(FATAL_ERROR "Set -DFFX_SDK= to the extracted FidelityFX-SDK v2.3.0 source root.")
endif()
get_filename_component(FFX_SDK "${FFX_SDK}" ABSOLUTE)
if(NOT FFX_BIN)
  set(FFX_BIN "${FFX_SDK}/PrebuiltSignedDLL")
endif()
get_filename_component(FFX_BIN "${FFX_BIN}" ABSOLUTE)

set(FFX_INCLUDE_API "${FFX_SDK}/Kits/FidelityFX/api/include")
set(FFX_INCLUDE_FG "${FFX_SDK}/Kits/FidelityFX/framegeneration/include")
set(FFX_INCLUDE_UPSCALER "${FFX_SDK}/Kits/FidelityFX/upscalers/include")
foreach(_include "${FFX_INCLUDE_API}" "${FFX_INCLUDE_FG}" "${FFX_INCLUDE_UPSCALER}")
  if(NOT EXISTS "${_include}")
    message(FATAL_ERROR "FSR SDK 2.3.0 include directory missing: ${_include}")
  endif()
endforeach()

# Intel XeSS 3.0.2 public D3D12/XeLL headers. Runtime DLLs and the AMD
# compatibility resource pack are user-provided and are never bundled.
set(XESS_SDK_ROOT "" CACHE PATH "Extracted Intel XeSS SDK 3.0.2 root")
if(NOT XESS_SDK_ROOT)
  message(FATAL_ERROR "Set -DXESS_SDK_ROOT= to the extracted Intel XeSS SDK 3.0.2 root.")
endif()
get_filename_component(XESS_SDK_ROOT "${XESS_SDK_ROOT}" ABSOLUTE)
set(XESS_INCLUDE_FG "${XESS_SDK_ROOT}/inc/xess_fg")
set(XESS_INCLUDE_LL "${XESS_SDK_ROOT}/inc/xell")
foreach(_include "${XESS_INCLUDE_FG}" "${XESS_INCLUDE_LL}")
  if(NOT EXISTS "${_include}")
    message(FATAL_ERROR "XeSS SDK 3.0.2 include directory missing: ${_include}")
  endif()
endforeach()
if(NOT EXISTS "${XESS_INCLUDE_FG}/xefg_swapchain_d3d12.h" OR
   NOT EXISTS "${XESS_INCLUDE_LL}/xell_d3d12.h")
  message(FATAL_ERROR "XeSS SDK 3.0.2 D3D12/XeLL headers are incomplete.")
endif()

set(_FFX_DLL_SEARCH_PATHS "${FFX_BIN}" "${FFX_SDK}/PrebuiltSignedDLL"
    "${FFX_SDK}/Kits/FidelityFX/bin" "${FFX_SDK}/bin")
find_file(FFX_DLL_LOADER NAMES amd_fidelityfx_loader_dx12.dll
  PATHS ${_FFX_DLL_SEARCH_PATHS} NO_DEFAULT_PATH)
find_file(FFX_DLL_FRAMEGEN NAMES amd_fidelityfx_framegeneration_dx12.dll
  PATHS ${_FFX_DLL_SEARCH_PATHS} NO_DEFAULT_PATH)
find_file(FFX_DLL_UPSCALER NAMES amd_fidelityfx_upscaler_dx12.dll
  PATHS ${_FFX_DLL_SEARCH_PATHS} NO_DEFAULT_PATH)
foreach(_required_var FFX_DLL_LOADER FFX_DLL_FRAMEGEN FFX_DLL_UPSCALER)
  if(NOT ${_required_var})
    message(FATAL_ERROR "Required FSR SDK 2.3.0 runtime missing: ${_required_var}. Set -DFFX_BIN= to the folder containing the signed DX12 DLLs.")
  endif()
  message(STATUS "FSR runtime ${_required_var}: ${${_required_var}}")
endforeach()

# ---------------------------------------------------------------------------
add_executable(NRLive WIN32
  src/main.cpp
  src/graphics.cpp
  src/capture.cpp
  src/fsr.cpp
  src/xess_mfg.cpp
  third_party/xess_mfg/runtime_payload.cpp
  third_party/xess_mfg/host_unlock.cpp
  src/dlssnr.cpp
  src/dlssnr_stabilizer.cpp
  src/opennr.cpp
  src/upscaler.cpp
  src/amdof.cpp
  src/fastmv.cpp
  src/ui.cpp
  src/overlay.cpp
  src/menu_kit.cpp
  src/hud.cpp
  src/hud_config.cpp
  src/sysmon.cpp
  src/gpu_timer.cpp
  src/target.cpp
  src/frametrace.cpp
  src/frame_limiter.cpp
)

target_include_directories(NRLive PRIVATE
  "${FFX_INCLUDE_API}"
  "${FFX_INCLUDE_FG}"
  "${FFX_INCLUDE_UPSCALER}"
  "${XESS_INCLUDE_FG}"
  "${XESS_INCLUDE_LL}"
  "${CMAKE_SOURCE_DIR}/third_party/xess_mfg"
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}/third_party/opennr"
)

target_compile_definitions(NRLive PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN PLATFORM_WINDOWS
)

target_link_libraries(NRLive PRIVATE
  d3d11 d3d12 dxgi dxguid windowsapp shcore d3dcompiler dwmapi gdi32 windowscodecs shell32 ole32 comdlg32 gdiplus pdh bcrypt
)

set_target_properties(NRLive PROPERTIES
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:NRLive>"
)

foreach(_dll_path "${FFX_DLL_LOADER}" "${FFX_DLL_FRAMEGEN}" "${FFX_DLL_UPSCALER}")
  add_custom_command(TARGET NRLive POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${_dll_path}"
      "$<TARGET_FILE_DIR:NRLive>"
  )
endforeach()

# Optional: OpenNR's DLSSNR runtime (MochizukiNrRuntime.dll + dlssnr-amd\), not
# vendored. Point OPENNR_RUNTIME at a folder made by scripts\fetch_opennr_runtime.ps1
# (or an OpenNR build) and it is staged as opennr\ beside the EXE.
set(OPENNR_RUNTIME "" CACHE PATH "Folder with MochizukiNrRuntime.dll and dlssnr-amd (optional)")
if(OPENNR_RUNTIME)
  if(NOT EXISTS "${OPENNR_RUNTIME}/MochizukiNrRuntime.dll")
    message(FATAL_ERROR "OPENNR_RUNTIME has no MochizukiNrRuntime.dll: ${OPENNR_RUNTIME}")
  endif()
  add_custom_command(TARGET NRLive POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OPENNR_RUNTIME}" "$<TARGET_FILE_DIR:NRLive>/opennr"
  )
endif()

if(BUILD_TESTING)
  add_executable(upscaler_choice_test tests/upscaler_choice_test.cpp src/upscaler.cpp)
  target_include_directories(upscaler_choice_test PRIVATE "${CMAKE_SOURCE_DIR}/include")
  add_test(NAME upscaler_provider_choice COMMAND upscaler_choice_test)

  add_executable(frame_timing_test tests/frame_timing_test.cpp)
  target_include_directories(frame_timing_test PRIVATE "${CMAKE_SOURCE_DIR}/include")
  add_test(NAME capture_frame_timing_invariants COMMAND frame_timing_test)

  add_executable(hud_config_test tests/hud_config_test.cpp src/hud_config.cpp src/sysmon.cpp)
  target_include_directories(hud_config_test PRIVATE "${CMAKE_SOURCE_DIR}/include")
  target_compile_definitions(hud_config_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(hud_config_test PRIVATE dxgi pdh)
  add_test(NAME hud_config_presets_lows_and_sensors COMMAND hud_config_test)

  add_executable(opennr_settings_test tests/opennr_settings_test.cpp)
  target_include_directories(opennr_settings_test PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/third_party/opennr")
  target_compile_definitions(opennr_settings_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  add_test(NAME opennr_defaults_int4_and_build_progress COMMAND opennr_settings_test)

  add_executable(frame_limiter_test tests/frame_limiter_test.cpp)
  target_include_directories(frame_limiter_test PRIVATE "${CMAKE_SOURCE_DIR}/include")
  add_test(NAME frame_limiter_invariants COMMAND frame_limiter_test)


  add_executable(fastmv_shader_test
    tests/fastmv_shader_test.cpp
    src/fastmv.cpp
  )
  target_include_directories(fastmv_shader_test PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
  )
  target_compile_definitions(fastmv_shader_test PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  )
  target_link_libraries(fastmv_shader_test PRIVATE
    d3d12 d3dcompiler dxgi
  )
  add_test(NAME fastmv_shader_and_motion_invariants
           COMMAND fastmv_shader_test)
endif()
