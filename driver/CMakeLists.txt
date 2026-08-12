# SteamVR loads this by filename, so the output must be exactly
# driver_convr.so / driver_convr.dll - no "lib" prefix on Linux.
add_library(driver_convr SHARED
  src/driver_factory.cpp
  src/device_provider.cpp
  src/treadmill_device.cpp
)

target_include_directories(driver_convr PRIVATE
  "${CONVR_OPENVR_INCLUDE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(driver_convr PRIVATE convr_common)

set_target_properties(driver_convr PROPERTIES
  PREFIX ""
  OUTPUT_NAME "driver_convr"
  CXX_VISIBILITY_PRESET hidden
  LIBRARY_OUTPUT_DIRECTORY "${CONVR_STAGE_DIR}/bin/${CONVR_PLATFORM_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${CONVR_STAGE_DIR}/bin/${CONVR_PLATFORM_DIR}"
)
# Multi-config generators (Visual Studio) otherwise append Release/ or Debug/.
foreach(_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(driver_convr PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${_config} "${CONVR_STAGE_DIR}/bin/${CONVR_PLATFORM_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_${_config} "${CONVR_STAGE_DIR}/bin/${CONVR_PLATFORM_DIR}"
  )
endforeach()

if(MSVC)
  target_compile_definitions(driver_convr PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Stage the manifest and resource JSON next to the binary so the build tree is
# a ready-to-copy SteamVR driver folder.
add_custom_command(TARGET driver_convr POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/convr" "${CONVR_STAGE_DIR}"
  COMMENT "Staging driver resources into ${CONVR_STAGE_DIR}"
  VERBATIM
)
