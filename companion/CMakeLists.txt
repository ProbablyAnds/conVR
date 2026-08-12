# ---------------------------------------------------------------------------
# SDL2: prefer whatever the system already has (typical on Linux), fall back to
# fetching and building it (typical on Windows).
# ---------------------------------------------------------------------------
set(CONVR_SDL2_TARGETS "")
find_package(SDL2 QUIET)

if(TARGET SDL2::SDL2)
  message(STATUS "conVR: using system SDL2")
  list(APPEND CONVR_SDL2_TARGETS SDL2::SDL2)
  if(TARGET SDL2::SDL2main)
    list(APPEND CONVR_SDL2_TARGETS SDL2::SDL2main)
  endif()
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2PC IMPORTED_TARGET QUIET sdl2)
  endif()
  if(TARGET PkgConfig::SDL2PC)
    message(STATUS "conVR: using SDL2 from pkg-config")
    list(APPEND CONVR_SDL2_TARGETS PkgConfig::SDL2PC)
  else()
    message(STATUS "conVR: fetching and building SDL2")
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
      URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
      URL_HASH SHA256=24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4
    )
    FetchContent_MakeAvailable(SDL2)
    list(APPEND CONVR_SDL2_TARGETS SDL2::SDL2)
    if(TARGET SDL2::SDL2main)
      list(APPEND CONVR_SDL2_TARGETS SDL2::SDL2main)
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Dear ImGui ships no build system; compile the core plus the two backends we
# use into a small static library.
# ---------------------------------------------------------------------------
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.5
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(convr_imgui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp"
)
target_include_directories(convr_imgui PUBLIC
  "${imgui_SOURCE_DIR}"
  "${imgui_SOURCE_DIR}/backends"
)
target_link_libraries(convr_imgui PUBLIC ${CONVR_SDL2_TARGETS})

# ---------------------------------------------------------------------------
# The companion itself.
# ---------------------------------------------------------------------------
add_executable(convr_companion
  src/main.cpp
  src/config.cpp
)
target_link_libraries(convr_companion PRIVATE
  convr_common
  convr_imgui
  ${CONVR_SDL2_TARGETS}
)
set_target_properties(convr_companion PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
foreach(_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(convr_companion PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${_config} "${CMAKE_BINARY_DIR}/bin"
  )
endforeach()

if(MSVC)
  target_compile_definitions(convr_companion PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Windows needs SDL2.dll beside the exe when SDL2 was fetched and built here.
if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
  add_custom_command(TARGET convr_companion POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:convr_companion>
            $<TARGET_RUNTIME_DLLS:convr_companion>
    COMMAND_EXPAND_LISTS
  )
endif()

install(TARGETS convr_companion
        RUNTIME DESTINATION bin
        COMPONENT companion)
