cmake_minimum_required(VERSION 3.24)
project(NRLive LANGUAGES CXX)
include(CTest)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
  src/dlssnr.cpp
  src/dlssnr_stabilizer.cpp
  src/amdof.cpp
  src/fastmv.cpp
  src/ui.cpp
  src/overlay.cpp
  src/target.cpp
  src/frametrace.cpp
  src/frame_limiter.cpp
)

target_include_directories(NRLive PRIVATE
  "${FFX_INCLUDE_API}"
  "${FFX_INCLUDE_FG}"
  "${FFX_INCLUDE_UPSCALER}"
  "${CMAKE_SOURCE_DIR}/include"
)

target_compile_definitions(NRLive PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN PLATFORM_WINDOWS WINVER=0x0A00 _WIN32_WINNT=0x0A00
)

target_link_libraries(NRLive PRIVATE
  d3d11 d3d12 dxgi dxguid windowsapp shcore d3dcompiler dwmapi gdi32 windowscodecs shell32 ole32 comdlg32
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

if(BUILD_TESTING)
  add_executable(frame_timing_test tests/frame_timing_test.cpp)
  target_include_directories(frame_timing_test PRIVATE "${CMAKE_SOURCE_DIR}/include")
  add_test(NAME capture_frame_timing_invariants COMMAND frame_timing_test)

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
