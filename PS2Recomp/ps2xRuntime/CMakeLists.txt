cmake_minimum_required(VERSION 3.20)

project(PS2Runtime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PS2X_ENABLE_RUNNER_UNITY_BUILD "Build ps2EntryRunner with CMake unity build" ON)
set(PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE 32 CACHE STRING "Unity build batch size for ps2EntryRunner")
option(PS2X_ENABLE_RUNNER_PCH "Precompile the heavy runtime headers for ps2EntryRunner" ON)
option(PS2X_ENABLE_SCCACHE "Use sccache as compiler launcher when available" ON)

option(PS2X_ENABLE_RUNTIME_LOGS "Enable PS2 runtime logs" ON)
option(PS2X_ENABLE_AGRESSIVE_LOGS "Enable very verbose/agressive PS2 runtime logs" OFF)
option(PS2X_ENABLE_IOP_RPC_TRACE "Log unhandled IOP/SIF RPC trace suggestions" ON)
option(PS2X_STRICT_RETURN_DIAGNOSTICS "Route generated JR $ra returns through runtime branch diagnostics" OFF)
option(PS2X_SHOW_WINDOWS_CONSOLE "Show a console window for ps2EntryRunner on Windows release builds" ON)
option(PS2X_ENABLE_DEBUG_UI "Build the desktop runtime debug UI" ON)

if(PS2X_ENABLE_SCCACHE)
    find_program(PS2X_SCCACHE_PROGRAM sccache)

    if(PS2X_SCCACHE_PROGRAM)
        if(CMAKE_C_COMPILER)
            set(CMAKE_C_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        endif()

        set(CMAKE_CXX_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        message(STATUS "Using sccache: ${PS2X_SCCACHE_PROGRAM}")
    else()
        message(STATUS "sccache not found; continuing without compiler launcher")
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReleaseMode.cmake")

set(PS2X_IS_VITA OFF)

if((CMAKE_C_COMPILER MATCHES "arm-vita-eabi") OR
    (CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi"))
    set(PS2X_IS_VITA ON)
endif()

set(PS2X_IS_ANDROID OFF)

if(ANDROID)
    set(PS2X_IS_ANDROID ON)
endif()

option(PS2X_VITA_CREATE_PACKAGE "Create Vita eboot/vpk targets" ON)
set(PS2X_VITA_APP_NAME "PS2 Retro X" CACHE STRING "Display name for the Vita bubble")
set(PS2X_VITA_TITLEID "RANJ00001" CACHE STRING "9-character Vita title id")
set(PS2X_VITA_VERSION "01.00" CACHE STRING "Vita app version")
set(PS2X_DEFAULT_BOOT_ELF "" CACHE STRING "Guest ELF path passed directly to main() when argv is unavailable")

if(PS2X_IS_VITA)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "VITASDK is not defined. Configure inside the VitaSDK environment or pass the Vita toolchain file.")
    endif()

    set(PS2X_VITASDK "$ENV{VITASDK}")

    if(NOT EXISTS "${PS2X_VITASDK}/share/vita.cmake")
        message(FATAL_ERROR "Could not find vita.cmake under ${PS2X_VITASDK}/share.")
    endif()

    include("${PS2X_VITASDK}/share/vita.cmake" REQUIRED)
else()
    include(FetchContent)
    set(FETCHCONTENT_QUIET FALSE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

    if(PS2X_IS_ANDROID)
        set(PLATFORM "Android" CACHE STRING "" FORCE)
    endif()

    FetchContent_Declare(
        raylib
        GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
        GIT_TAG "5.5" # we will migrate to 4.2.0 later, trust me it will be better
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(raylib)

    if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_ANDROID)
        FetchContent_Declare(
            imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG "v1.92.7-docking"
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(imgui)

        if(NOT imgui_POPULATED)
            FetchContent_Populate(imgui)
        endif()

        set(PS2X_IMGUI_SOURCE_DIR "${imgui_SOURCE_DIR}")

        add_library(imgui STATIC
            "${PS2X_IMGUI_SOURCE_DIR}/imgui.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
        )
        target_include_directories(imgui PUBLIC "${PS2X_IMGUI_SOURCE_DIR}")

        FetchContent_Declare(
            rlImGui
            GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
            GIT_TAG "Raylib_5_5"
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(rlImGui)

        if(NOT rlimgui_POPULATED)
            FetchContent_Populate(rlImGui)
        endif()

        set(PS2X_RLIMGUI_SOURCE_DIR "${rlimgui_SOURCE_DIR}")

        add_library(rlImGui STATIC
            "${PS2X_RLIMGUI_SOURCE_DIR}/rlImGui.cpp"
        )
        target_include_directories(rlImGui PUBLIC "${PS2X_RLIMGUI_SOURCE_DIR}")
        target_link_libraries(rlImGui PUBLIC raylib imgui)
        # rlImGui drops the mouse while the window is unfocused, so the click that focuses a window never reached
        # ImGui and every button in an unfocused launcher took two clicks. Track it always: one click works.
        target_compile_definitions(rlImGui PRIVATE RLIMGUI_ALWAYS_TRACK_MOUSE)
    endif()
endif()

add_library(ps2_host_backend INTERFACE)

set(PS2X_DEFAULT_BOOT_ELF_DEFINE "")

if(PS2X_DEFAULT_BOOT_ELF)
    set(PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF}")
    string(REPLACE "\\" "\\\\" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
    string(REPLACE "\"" "\\\"" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
endif()

if(PS2X_IS_VITA)
    set(PS2X_VITA_PREFIX "${PS2X_VITASDK}/arm-vita-eabi")
    set(PS2X_VITA_EXTRA_INCLUDE_DIRS "" CACHE STRING "Extra include directories for Vita host dependencies")
    set(PS2X_VITA_LIBRARY_DIR "${PS2X_VITA_PREFIX}/lib")
    set(PS2X_VITA_REQUIRED_LIBRARY_NAMES
        raylib
        SDL2
        OpenSLES
        taihen_stub
        SceAppMgr_stub
        SceCtrl_stub
        SceGxm_stub
        SceCommonDialog_stub
        SceLibKernel_stub
        SceAudio_stub
        SceTouch_stub
        SceHid_stub
        SceMotion_stub
        SceSysmodule_stub
        SceIofilemgr_stub
        SceNetCtl_stub
        SceNet_stub
        SceDisplay_stub
        SceAppUtil_stub
        SceAudioIn_stub
        ScePower_stub
        SceProcessmgr_stub
        SceIme_stub
        libIMGEGL_stub_weak
        libgpu_es4_ext_stub_weak
        libGLESv2_stub_weak
        libGL_stub_weak
        libGLESv1_CM_stub_weak
    )

    set(PS2X_VITA_INCLUDE_DIRS
        "${PS2X_VITA_PREFIX}/include"
    )

    foreach(PS2X_VITA_OPTIONAL_INCLUDE_DIR IN ITEMS
        "${PS2X_VITA_PREFIX}/include/raylib"
        "${PS2X_VITA_PREFIX}/include/SDL2")
        if(EXISTS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
            list(APPEND PS2X_VITA_INCLUDE_DIRS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
        endif()
    endforeach()

    if(PS2X_VITA_EXTRA_INCLUDE_DIRS)
        list(APPEND PS2X_VITA_INCLUDE_DIRS ${PS2X_VITA_EXTRA_INCLUDE_DIRS})
    endif()

    if(NOT EXISTS "${PS2X_VITA_PREFIX}/include/SDL2/SDL.h")
        message(FATAL_ERROR
            "Missing SDL2 headers under ${PS2X_VITA_PREFIX}/include/SDL2. "
            "Quenom/raylib-5.5-vita requires SDL2 with PVR support installed into VitaSDK.")
    endif()

    if(NOT EXISTS "${PS2X_VITA_LIBRARY_DIR}")
        message(FATAL_ERROR "Missing Vita library directory: ${PS2X_VITA_LIBRARY_DIR}")
    endif()

    function(ps2x_find_vita_library out_var library_name)
        string(MAKE_C_IDENTIFIER "${library_name}" library_id)
        set(cache_var "PS2X_VITA_LIB_${library_id}")

        find_library(${cache_var}
            NAMES "${library_name}" "${library_name}.a" "lib${library_name}.a"
            PATHS "${PS2X_VITA_LIBRARY_DIR}"
            NO_DEFAULT_PATH
        )

        if(NOT ${cache_var})
            message(FATAL_ERROR
                "Could not find Vita library '${library_name}' under ${PS2X_VITA_LIBRARY_DIR}. "
                "Install Quenom/raylib-5.5-vita and its SDL2/PVR dependencies into VitaSDK first.")
        endif()

        set(${out_var} "${${cache_var}}" PARENT_SCOPE)
    endfunction()

    set(PS2X_VITA_RESOLVED_LIBRARIES "")

    foreach(PS2X_VITA_LIBRARY_NAME IN LISTS PS2X_VITA_REQUIRED_LIBRARY_NAMES)
        ps2x_find_vita_library(PS2X_VITA_LIBRARY_PATH "${PS2X_VITA_LIBRARY_NAME}")
        list(APPEND PS2X_VITA_RESOLVED_LIBRARIES "${PS2X_VITA_LIBRARY_PATH}")
    endforeach()

    target_compile_definitions(ps2_host_backend INTERFACE PLATFORM_VITA)
    target_include_directories(ps2_host_backend INTERFACE ${PS2X_VITA_INCLUDE_DIRS})
    target_link_libraries(ps2_host_backend INTERFACE
        ${PS2X_VITA_RESOLVED_LIBRARIES}
        m
        c
        pthread
    )
else()
    target_link_libraries(ps2_host_backend INTERFACE raylib)

    if(PS2X_IS_ANDROID)
        target_link_libraries(ps2_host_backend INTERFACE log android)
    endif()
endif()

if(PS2X_IS_ANDROID)
    set(PS2X_ENABLE_FFMPEG_DEFAULT OFF)
else()
    set(PS2X_ENABLE_FFMPEG_DEFAULT ON)
endif()

option(PS2X_ENABLE_FFMPEG "Enable FFmpeg-backed MPEG video decoding" ${PS2X_ENABLE_FFMPEG_DEFAULT})

if(NOT PS2X_ENABLE_FFMPEG)
    message(STATUS "FFmpeg disabled; MPEG video decode falls back to stub frames")
    add_library(ffmpeg INTERFACE)
elseif(WIN32)
    include(ExternalProject)

    set(FFMPEG_PREBUILT_TAG "n7.1-241205")
    set(FFMPEG_PREBUILT_URL
        "https://github.com/System233/ffmpeg-msvc-prebuilt/releases/download/${FFMPEG_PREBUILT_TAG}/ffmpeg-${FFMPEG_PREBUILT_TAG}-lgpl-amd64-shared.zip")

    set(FFMPEG_PREFIX_DIR "${CMAKE_BINARY_DIR}/ThirdParty/ffmpeg-prefix")
    set(FFMPEG_SOURCE_DIR "${FFMPEG_PREFIX_DIR}/src/ffmpeg_external")
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_SOURCE_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_SOURCE_DIR}/bin")
    set(FFMPEG_BIN_DIR "${FFMPEG_SOURCE_DIR}/bin")

    ExternalProject_Add(ffmpeg_external
        URL "${FFMPEG_PREBUILT_URL}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${FFMPEG_PREFIX_DIR}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
        "${FFMPEG_LIB_DIR}/avcodec.lib"
        "${FFMPEG_LIB_DIR}/avformat.lib"
        "${FFMPEG_LIB_DIR}/avutil.lib"
        "${FFMPEG_LIB_DIR}/swresample.lib"
        "${FFMPEG_LIB_DIR}/swscale.lib"
    )

    # Link against the import libraries shipped with the Windows DLLs.
    add_library(ffmpeg_avcodec STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avcodec.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avcodec ffmpeg_external)

    add_library(ffmpeg_avformat STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avformat.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avformat ffmpeg_external)

    add_library(ffmpeg_avutil STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avutil.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avutil ffmpeg_external)

    add_library(ffmpeg_swresample STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swresample.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swresample ffmpeg_external)

    add_library(ffmpeg_swscale STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swscale.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swscale ffmpeg_external)

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE
        ffmpeg_avcodec
        ffmpeg_avformat
        ffmpeg_avutil
        ffmpeg_swresample
        ffmpeg_swscale
        bcrypt
        secur32
        ws2_32
        user32
        advapi32
        ole32
        shell32
    )

    set(PS2X_FFMPEG_BIN_DIR "${FFMPEG_BIN_DIR}" CACHE INTERNAL "Directory containing FFmpeg runtime DLLs")
    set(PS2X_COPY_FFMPEG_DLLS_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyFfmpegDlls.cmake"
        CACHE INTERNAL
        "Script used to stage FFmpeg runtime DLLs beside executables")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
        libavcodec
        libavformat
        libavutil
        libswresample
        libswscale
    )

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

function(ps2x_stage_ffmpeg_runtime_dlls target_name)
    if(NOT WIN32)
        return()
    endif()

    if(NOT PS2X_FFMPEG_BIN_DIR OR NOT PS2X_COPY_FFMPEG_DLLS_SCRIPT)
        return()
    endif()

    if(TARGET ffmpeg_external)
        add_dependencies(${target_name} ffmpeg_external)
    endif()

    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${PS2X_FFMPEG_BIN_DIR}"
        "-Ddest_dir=$<TARGET_FILE_DIR:${target_name}>"
        -P "${PS2X_COPY_FFMPEG_DLLS_SCRIPT}"
        VERBATIM
    )
endfunction()

add_library(ps2_runtime STATIC
    src/lib/game_overrides.cpp
    src/lib/gs/ps2_gif_arbiter.cpp
    src/lib/ps2_audio.cpp
    src/lib/ps2_audio_vag.cpp
    src/lib/ps2_spu2_audio.cpp
    src/lib/gs/ps2_gs_memory.cpp
    src/lib/gs/gs_frontend.cpp
    src/lib/gs/gs_cpu_backend.cpp
    src/lib/gs/gs_threaded_backend.cpp
    src/lib/gs/gs_record_replay.cpp
    src/lib/gs/gs_gpu_backend.cpp
    src/lib/gs/gs_texture_replace.cpp
    src/lib/gs/gs_texture_hd_png.cpp
    src/lib/ps2_iop_host.cpp
    src/lib/ps2_memory.cpp
    src/lib/ps2_pad.cpp
    src/lib/ps2_netplay.cpp
    src/lib/ps2_rom_device.cpp
    src/lib/ps2_vfs.cpp
    src/lib/ps2_mod_loader.cpp
    src/lib/ps2_runtime.cpp
    src/lib/ps2_framegen.cpp
    src/lib/ps2_vif1_interpreter.cpp
    src/lib/vu/ps2_vu1_core.cpp
    src/lib/vu/ps2_vu1_upper.cpp
    src/lib/vu/ps2_vu1_lower.cpp
    src/lib/vu/ps2_vu1_replay.cpp
    src/lib/games_database.cpp
    src/lib/ps2_sampler.cpp
)

# Opt-in GPU GS renderer (PS2X_GS_GPU=1): GL 3.3 through raylib's GLFW and glad headers.
if(NOT PS2X_IS_VITA AND NOT PS2X_IS_ANDROID AND DEFINED raylib_SOURCE_DIR)
    set_source_files_properties(src/lib/gs/gs_gpu_backend.cpp PROPERTIES
        COMPILE_DEFINITIONS "PS2X_GS_GPU_AVAILABLE=1"
        INCLUDE_DIRECTORIES "${raylib_SOURCE_DIR}/src;${raylib_SOURCE_DIR}/src/external/glfw/include"
    )
endif()

# Controllers through SDL2's game controller API (ps2_pad.cpp): XInput, DualShock 4 / DualSense, Switch
# and most other pads read directly (HIDAPI), so no x360ce / DS4Windows / Steam Input is needed. Static,
# and only ps2_pad.cpp sees its headers (a ps2_runtime-wide flag would rebuild everything).
# ps2xStudio declares the same SDL2 (shared); whichever comes first is used, so both kinds are built.
if(NOT PS2X_IS_VITA AND NOT PS2X_IS_ANDROID)
    FetchContent_Declare(
        SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-2.32.10
        GIT_SHALLOW    TRUE
    )
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL2)
    set_source_files_properties(src/lib/ps2_pad.cpp PROPERTIES
        COMPILE_DEFINITIONS "PS2X_SDL_PAD=1"
        INCLUDE_DIRECTORIES "$<TARGET_PROPERTY:SDL2-static,INTERFACE_INCLUDE_DIRECTORIES>"
    )
    target_link_libraries(ps2_runtime PRIVATE $<LINK_ONLY:SDL2::SDL2-static>)
endif()

if(PS2X_ENABLE_RUNTIME_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2_RUNTIME_LOGS=1
    )
endif()

if(PS2X_ENABLE_AGRESSIVE_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        AGRESSIVE_LOGS=1
        PS2_FUNCTION_LOG_TRACKER=1
    )
endif()

if(PS2X_ENABLE_IOP_RPC_TRACE)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=1
    )
else()
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=0
    )
endif()

if(PS2X_STRICT_RETURN_DIAGNOSTICS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_STRICT_RETURN_DIAGNOSTICS=1
    )
endif()

target_compile_definitions(ps2_runtime PRIVATE
    PS2X_HAS_FFMPEG=$<BOOL:${PS2X_ENABLE_FFMPEG}>
)

file(GLOB_RECURSE KERNEL_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel/*.cpp"
)

target_sources(ps2_runtime PRIVATE
    ${KERNEL_SRC_FILES}
)

# The program icon (assets/, made by tools/app_icon.py). Windows exes carry assets/s2recomp.rc, which GLFW picks
# up by itself; on Linux the PNG is compiled in and set with SetWindowIcon (PS2X_APP_ICON_PNG).
set(PS2X_APP_ICON_RC "${CMAKE_CURRENT_SOURCE_DIR}/assets/s2recomp.rc")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/assets/s2recomp.png" _hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
    string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" _bytes "${_bytes}")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/app_icon_png.cpp" CONTENT
        "// Generated by ps2xRuntime/CMakeLists.txt from assets/s2recomp.png.\nextern const unsigned char kAppIconPng[];\nextern const unsigned int kAppIconPng_size;\nconst unsigned char kAppIconPng[] = {\n${_bytes}};\nconst unsigned int kAppIconPng_size = sizeof(kAppIconPng);\n" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/s2recomp.png")
    target_sources(ps2_runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app_icon_png.cpp")
    target_compile_definitions(ps2_runtime PRIVATE PS2X_APP_ICON_PNG=1)
endif()

# Statically recompiled VU1 microprograms (ps2EntryRunner --vu-gen writes src/lib/vu/vu1gen/).
file(GLOB VU1GEN_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/vu1gen/*.cpp"
)
target_sources(ps2_runtime PRIVATE
    src/lib/vu/ps2_vu1_compiled.cpp
    src/lib/vu/ps2_vu1_gen.cpp
    ${VU1GEN_SRC_FILES}
)
# ps2xVuGen: the generator alone, so a player's build writes vu1gen/ from their own game files
# (vu1_locations.toml) before it compiles the game. Needs nothing of the runtime but the VU headers.
if(NOT PS2X_IS_VITA AND NOT PS2X_IS_ANDROID)
    add_executable(ps2xVuGen src/lib/vu/ps2_vu1_gen_main.cpp)
    target_include_directories(ps2xVuGen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(NOT MSVC AND NOT PS2X_IS_ARM_TARGET AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        target_compile_options(ps2xVuGen PRIVATE -msse4.2 -mpopcnt)
    endif()
endif()
# The VU float semantics must not depend on the inlining context (Release is /fp:fast, which lets the
# optimizer contract acc + a * b into an FMA or not, per call site): /fp:precise for the interpreter
# and the generated programs, so a pair computes the same bits wherever its code is inlined. MSVC
# does not contract under /fp:precise (no /fp:contract); the real VU does not fuse either.
if(MSVC)
    # clang-cl contracts a * b + c into an FMA under /fp:precise unless told not to.
    set(PS2X_VU_FP_OPTIONS "/fp:precise")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(PS2X_VU_FP_OPTIONS "/fp:precise;/clang:-ffp-contract=off")
    endif()
    set_source_files_properties(
        src/lib/vu/ps2_vu1_core.cpp
        src/lib/vu/ps2_vu1_upper.cpp
        src/lib/vu/ps2_vu1_lower.cpp
        src/lib/vu/ps2_vu1_replay.cpp
        src/lib/vu/ps2_vu1_compiled.cpp
        src/lib/vu/ps2_vu1_gen.cpp
        ${VU1GEN_SRC_FILES}
        PROPERTIES COMPILE_OPTIONS "${PS2X_VU_FP_OPTIONS}")
elseif(NOT PS2X_IS_ARM_TARGET AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    # GCC/Clang only allow the SSE4 intrinsics MSVC accepts anywhere when the target enables them. The
    # VU1 sources get AVX2 like the clang-cl objects of the Windows build, with no FMA contraction.
    target_compile_options(ps2_runtime PUBLIC -msse4.2 -mpopcnt)
    set_source_files_properties(
        src/lib/vu/ps2_vu1_core.cpp
        src/lib/vu/ps2_vu1_upper.cpp
        src/lib/vu/ps2_vu1_lower.cpp
        src/lib/vu/ps2_vu1_replay.cpp
        src/lib/vu/ps2_vu1_compiled.cpp
        src/lib/vu/ps2_vu1_gen.cpp
        ${VU1GEN_SRC_FILES}
        PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma;-ffp-contract=off")
endif()

# The VU1 interpreter and generated programs build with clang-cl (from the same Visual Studio install)
# while the rest stays MSVC: clang inlines the big generated block functions that MSVC calls out of
# line, ~17% less VU1 time, same bits (replays: 0 mismatches). Building the whole game with clang-cl
# was 2-3x slower in game, so only these objects; clang-cl is MSVC-ABI compatible and uses the same STL.
# PS2X_VU_CLANG=OFF or no clang-cl found = MSVC for them as well.
option(PS2X_VU_CLANG "Compile the VU1 sources with clang-cl in MSVC builds (clang++ in GCC builds)" ON)
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND PS2X_VU_CLANG)
    get_filename_component(PS2X_MSVC_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY) # VC/Tools/MSVC/<v>/bin/Hostx64/x64
    find_program(PS2X_CLANG_CL clang-cl HINTS "${PS2X_MSVC_BIN_DIR}/../../../../../Llvm/x64/bin" NO_DEFAULT_PATH)
    if(PS2X_CLANG_CL)
        message(STATUS "VU1 sources: clang-cl (${PS2X_CLANG_CL})")
        set(PS2X_VU_CLANG_SOURCES
            src/lib/vu/ps2_vu1_core.cpp
            src/lib/vu/ps2_vu1_upper.cpp
            src/lib/vu/ps2_vu1_lower.cpp
            src/lib/vu/ps2_vu1_replay.cpp
            src/lib/vu/ps2_vu1_compiled.cpp
            src/lib/vu/ps2_vu1_gen.cpp
            ${VU1GEN_SRC_FILES})
        file(GLOB_RECURSE PS2X_VU_CLANG_DEPENDS CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/*.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/*.inl"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/*.inc"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
        # PS2X_VU_PGO: clang profile-guided optimisation of the VU1 objects only (code layout: the common
        # path of every generated pair falls through, the rest moves out of the way; same float semantics).
        # AUTO = use src/lib/vu/vu1gen/vu1.profdata if it exists, OFF, GEN = instrumented build (run the
        # VU replays with it, then llvm-profdata merge; tools/vu1pgo.ps1 does all of it), or a .profdata
        # path. The profile must not live under %TEMP%: MSBuild then re-runs every VU compile on each build.
        set(PS2X_VU_PGO "AUTO" CACHE STRING "VU1 clang PGO: AUTO, OFF, GEN or <file.profdata>")
        set(PS2X_VU_PGO_FLAGS)
        set(PS2X_VU_PGO_DEPENDS)
        set(PS2X_VU_PGO_PROFILE "${PS2X_VU_PGO}")
        if(PS2X_VU_PGO STREQUAL "AUTO")
            set(PS2X_VU_PGO_PROFILE "")
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/vu1gen/vu1.profdata")
                set(PS2X_VU_PGO_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/vu1gen/vu1.profdata")
            endif()
        endif()
        if(PS2X_VU_PGO STREQUAL "GEN")
            set(PS2X_VU_PGO_FLAGS /clang:-fprofile-instr-generate)
            get_filename_component(PS2X_CLANG_BIN_DIR "${PS2X_CLANG_CL}" DIRECTORY)
            file(GLOB PS2X_CLANG_PROFILE_LIB "${PS2X_CLANG_BIN_DIR}/../lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib")
            target_link_libraries(ps2_runtime PUBLIC ${PS2X_CLANG_PROFILE_LIB})
        elseif(PS2X_VU_PGO_PROFILE)
            set(PS2X_VU_PGO_FLAGS "/clang:-fprofile-instr-use=${PS2X_VU_PGO_PROFILE}")
            set(PS2X_VU_PGO_DEPENDS "${PS2X_VU_PGO_PROFILE}")
            message(STATUS "VU1 sources: PGO profile ${PS2X_VU_PGO_PROFILE}")
        endif()
        set(PS2X_VU_CLANG_FLAGS
            /nologo /c /std:c++20 /EHsc /arch:AVX2 /fp:precise /clang:-ffp-contract=off /GS- /Gy /Gw
            -Wno-everything ${PS2X_VU_PGO_FLAGS}
            $<IF:$<CONFIG:Debug>,/Od,/O2> $<IF:$<CONFIG:Debug>,/MDd,/MD>
            $<$<NOT:$<CONFIG:Debug>>:/Ob2> $<$<NOT:$<CONFIG:Debug>>:/Oi> $<$<NOT:$<CONFIG:Debug>>:/DNDEBUG>
            /DWIN32 /D_WINDOWS
            "$<LIST:TRANSFORM,$<TARGET_PROPERTY:ps2_runtime,INCLUDE_DIRECTORIES>,PREPEND,/I>"
            "$<LIST:TRANSFORM,$<TARGET_PROPERTY:ps2_runtime,COMPILE_DEFINITIONS>,PREPEND,/D>")
        set(PS2X_VU_CLANG_OBJECTS)
        foreach(src IN LISTS PS2X_VU_CLANG_SOURCES)
            get_filename_component(name "${src}" NAME_WE)
            get_filename_component(abs "${src}" ABSOLUTE)
            set(obj "${CMAKE_CURRENT_BINARY_DIR}/vu_clang/$<CONFIG>/${name}.obj")
            add_custom_command(OUTPUT "${obj}"
                COMMAND "${PS2X_CLANG_CL}" ${PS2X_VU_CLANG_FLAGS} "/Fo${obj}" "${abs}"
                DEPENDS "${abs}" ${PS2X_VU_CLANG_DEPENDS} ${PS2X_VU_PGO_DEPENDS}
                COMMENT "clang-cl ${name}.cpp"
                COMMAND_EXPAND_LISTS VERBATIM)
            list(APPEND PS2X_VU_CLANG_OBJECTS "${obj}")
        endforeach()
        set_source_files_properties(${PS2X_VU_CLANG_SOURCES} PROPERTIES HEADER_FILE_ONLY ON)
        set_source_files_properties(${PS2X_VU_CLANG_OBJECTS} PROPERTIES EXTERNAL_OBJECT ON GENERATED ON)
        target_sources(ps2_runtime PRIVATE ${PS2X_VU_CLANG_OBJECTS})
    else()
        message(STATUS "VU1 sources: MSVC (clang-cl not found; install the C++ Clang tools for Windows)")
    endif()
endif()

# The same split on Linux: GCC for the game, clang++ (15 or newer: Ubuntu 22.04 has 15) for the VU1 sources,
# with the profile of the Linux build: src/lib/vu/pgo/vu1_linux.profdata (not in vu1gen, which build_game.sh
# replaces; clang's Itanium names, the Windows profile has MSVC ones; tools/vu1pgo_linux.sh trains it).
# PS2X_VU_CLANG_CXX = which clang++; PS2X_VU_PGO as above.
if(NOT MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND PS2X_VU_CLANG
        AND NOT PS2X_IS_ARM_TARGET AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    find_program(PS2X_VU_CLANG_CXX NAMES clang++ clang++-20 clang++-19 clang++-18 clang++-17 clang++-16 clang++-15)
    set(PS2X_VU_CLANG_VERSION 0)
    if(PS2X_VU_CLANG_CXX)
        execute_process(COMMAND "${PS2X_VU_CLANG_CXX}" -dumpversion OUTPUT_VARIABLE PS2X_VU_CLANG_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(PS2X_VU_CLANG_VERSION VERSION_GREATER_EQUAL 15)
        message(STATUS "VU1 sources: clang++ ${PS2X_VU_CLANG_VERSION} (${PS2X_VU_CLANG_CXX})")
        set(PS2X_VU_CLANG_SOURCES
            src/lib/vu/ps2_vu1_core.cpp
            src/lib/vu/ps2_vu1_upper.cpp
            src/lib/vu/ps2_vu1_lower.cpp
            src/lib/vu/ps2_vu1_replay.cpp
            src/lib/vu/ps2_vu1_compiled.cpp
            src/lib/vu/ps2_vu1_gen.cpp
            ${VU1GEN_SRC_FILES})
        file(GLOB_RECURSE PS2X_VU_CLANG_DEPENDS CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/*.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/*.inl"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/*.inc"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
        set(PS2X_VU_PGO "AUTO" CACHE STRING "VU1 clang PGO: AUTO, OFF, GEN or <file.profdata>")
        set(PS2X_VU_PGO_FLAGS)
        set(PS2X_VU_PGO_DEPENDS)
        set(PS2X_VU_PGO_PROFILE "${PS2X_VU_PGO}")
        if(PS2X_VU_PGO STREQUAL "AUTO")
            set(PS2X_VU_PGO_PROFILE "")
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/pgo/vu1_linux.profdata")
                set(PS2X_VU_PGO_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/pgo/vu1_linux.profdata")
            endif()
        endif()
        if(PS2X_VU_PGO STREQUAL "GEN")
            set(PS2X_VU_PGO_FLAGS -fprofile-instr-generate)
            execute_process(COMMAND "${PS2X_VU_CLANG_CXX}" --print-file-name=libclang_rt.profile-x86_64.a
                OUTPUT_VARIABLE PS2X_CLANG_PROFILE_LIB OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT EXISTS "${PS2X_CLANG_PROFILE_LIB}")
                execute_process(COMMAND "${PS2X_VU_CLANG_CXX}" -print-runtime-dir
                    OUTPUT_VARIABLE PS2X_CLANG_RT_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
                file(GLOB PS2X_CLANG_PROFILE_LIB "${PS2X_CLANG_RT_DIR}/libclang_rt.profile*.a")
            endif()
            # clang's driver links with -u__llvm_profile_runtime; GCC's doesn't, so nothing would pull in the
            # part of the runtime that writes the .profraw at exit.
            target_link_libraries(ps2_runtime PUBLIC ${PS2X_CLANG_PROFILE_LIB})
            target_link_options(ps2_runtime PUBLIC -Wl,-u,__llvm_profile_runtime)
        elseif(PS2X_VU_PGO_PROFILE AND NOT PS2X_VU_PGO STREQUAL "OFF")
            set(PS2X_VU_PGO_FLAGS "-fprofile-instr-use=${PS2X_VU_PGO_PROFILE}" -Wno-profile-instr-unprofiled
                -Wno-profile-instr-out-of-date)
            set(PS2X_VU_PGO_DEPENDS "${PS2X_VU_PGO_PROFILE}")
            message(STATUS "VU1 sources: PGO profile ${PS2X_VU_PGO_PROFILE}")
        endif()
        set(PS2X_VU_CLANG_FLAGS
            -c -std=c++20 -fPIE -mavx2 -mfma -msse4.2 -mpopcnt -ffp-contract=off -w ${PS2X_VU_PGO_FLAGS}
            $<IF:$<CONFIG:Debug>,-O0,-O2> $<$<CONFIG:Debug>:-g> $<$<NOT:$<CONFIG:Debug>>:-DNDEBUG>
            "$<LIST:TRANSFORM,$<TARGET_PROPERTY:ps2_runtime,INCLUDE_DIRECTORIES>,PREPEND,-I>"
            "$<LIST:TRANSFORM,$<TARGET_PROPERTY:ps2_runtime,COMPILE_DEFINITIONS>,PREPEND,-D>")
        set(PS2X_VU_CLANG_OBJECTS)
        foreach(src IN LISTS PS2X_VU_CLANG_SOURCES)
            get_filename_component(name "${src}" NAME_WE)
            get_filename_component(abs "${src}" ABSOLUTE)
            set(obj "${CMAKE_CURRENT_BINARY_DIR}/vu_clang/${name}.o")
            add_custom_command(OUTPUT "${obj}"
                COMMAND "${PS2X_VU_CLANG_CXX}" ${PS2X_VU_CLANG_FLAGS} -o "${obj}" "${abs}"
                DEPENDS "${abs}" ${PS2X_VU_CLANG_DEPENDS} ${PS2X_VU_PGO_DEPENDS}
                COMMENT "clang++ ${name}.cpp"
                COMMAND_EXPAND_LISTS VERBATIM)
            list(APPEND PS2X_VU_CLANG_OBJECTS "${obj}")
        endforeach()
        set_source_files_properties(${PS2X_VU_CLANG_SOURCES} PROPERTIES HEADER_FILE_ONLY ON)
        set_source_files_properties(${PS2X_VU_CLANG_OBJECTS} PROPERTIES EXTERNAL_OBJECT ON GENERATED ON)
        target_sources(ps2_runtime PRIVATE ${PS2X_VU_CLANG_OBJECTS})
    else()
        message(STATUS "VU1 sources: GCC (no clang++ 15 or newer found)")
    endif()
endif()

file(GLOB RUNNER_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/*.cpp"
)

set(RUNNER_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/main.cpp")
set(ROOT_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

if(EXISTS "${RUNNER_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${RUNNER_MAIN_CPP}")
elseif(EXISTS "${ROOT_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${ROOT_MAIN_CPP}")
endif()

if(PS2X_IS_ANDROID)
    add_library(ps2EntryRunner SHARED
        ${RUNNER_SRC_FILES}
    )

    target_link_options(ps2EntryRunner PRIVATE "-Wl,--undefined=ANativeActivity_onCreate")
else()
    add_executable(ps2EntryRunner
        ${RUNNER_SRC_FILES}
    )
    if(WIN32)
        target_sources(ps2EntryRunner PRIVATE "${PS2X_APP_ICON_RC}")
    endif()
endif()

if(PS2X_ENABLE_RUNNER_UNITY_BUILD)
    set_target_properties(ps2EntryRunner PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE "${PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE}"
    )
endif()

# register_functions.cpp is one generated function of ~470k lines that runs once at start-up. GCC and
# Clang at -O3 spend close to an hour on it, so they build it alone at -O1 (MSVC copes as it is).
if(NOT MSVC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/register_functions.cpp")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/runner/register_functions.cpp" PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION ON
        COMPILE_OPTIONS "-O1")
endif()

if(PS2X_ENABLE_RUNNER_PCH)
    set(PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime_macros.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime.h"
    )

    foreach(PS2X_RUNNER_GENERATED_HEADER IN ITEMS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_recompiled_functions.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_recompiled_stubs.h")
        if(EXISTS "${PS2X_RUNNER_GENERATED_HEADER}")
            list(APPEND PS2X_RUNNER_PCH_HEADERS "${PS2X_RUNNER_GENERATED_HEADER}")
        endif()
    endforeach()

    list(APPEND PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_syscalls.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_stubs.h"
    )

    target_precompile_headers(ps2EntryRunner PRIVATE ${PS2X_RUNNER_PCH_HEADERS})
endif()

if(PS2X_DEFAULT_BOOT_ELF_DEFINE)
    target_compile_definitions(ps2EntryRunner PRIVATE
        PS2X_DEFAULT_BOOT_ELF="${PS2X_DEFAULT_BOOT_ELF_DEFINE}"
    )
endif()

if(MSVC)
    target_compile_options(ps2EntryRunner PRIVATE /FS)
endif()

# Texture packs (gs_texture_replace.cpp loads texpacks/kbm next to the executable).
add_custom_command(
    TARGET ps2EntryRunner POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/texpacks" "$<TARGET_FILE_DIR:ps2EntryRunner>/texpacks"
    VERBATIM
)

# Mods (ps2_mod_loader.cpp loads mods/ next to the executable): the readme, the mod SDK and the
# bundled mods (the examples, with their prebuilt mod.dll). Mod settings already
# there (mod.ini) and mods the user installs are left alone.
add_custom_command(
    TARGET ps2EntryRunner POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
    "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}/mods" "-DDST=$<TARGET_FILE_DIR:ps2EntryRunner>/mods"
    "-DAPI_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/include/runtime/s2r_mod_api.h"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_mods.cmake"
    VERBATIM
)

target_include_directories(ps2_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel
)

target_link_libraries(ps2_runtime PUBLIC ps2_host_backend ffmpeg ps2_iop)

# Online co-op (ps2_netplay.cpp, PS2X_NETPLAY=host|join): Epic Online Services P2P, from the EOS C SDK
# unpacked next to the project (PROJECT/EOS/SDK). Without it the netplay calls build as no-ops.
option(PS2X_ENABLE_EOS "Build EOS netplay when the EOS SDK is found" ON)
set(PS2X_EOS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../EOS/SDK" CACHE PATH "EOS C SDK root (Include, Lib, Bin)")
if(WIN32)
    set(_ps2x_eos_link "${PS2X_EOS_SDK_DIR}/Lib/EOSSDK-Win64-Shipping.lib")
    set(_ps2x_eos_bin "${PS2X_EOS_SDK_DIR}/Bin/EOSSDK-Win64-Shipping.dll")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    # The SDK ships no import library on Linux: the .so is linked directly (its soname has no path).
    set(_ps2x_eos_link "${PS2X_EOS_SDK_DIR}/Bin/libEOSSDK-Linux-Shipping.so")
    set(_ps2x_eos_bin "${_ps2x_eos_link}")
endif()
if(PS2X_ENABLE_EOS AND _ps2x_eos_link AND EXISTS "${PS2X_EOS_SDK_DIR}/Include/eos_sdk.h" AND EXISTS "${_ps2x_eos_link}")
    message(STATUS "EOS netplay: ${PS2X_EOS_SDK_DIR}")
    set(_ps2x_eos_found ON)
    set_source_files_properties(src/lib/ps2_netplay.cpp PROPERTIES
        COMPILE_DEFINITIONS "PS2X_EOS=1"
        INCLUDE_DIRECTORIES "${PS2X_EOS_SDK_DIR}/Include"
    )
    target_link_libraries(ps2_runtime PRIVATE "${_ps2x_eos_link}")
    add_custom_command(
        TARGET ps2EntryRunner POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_ps2x_eos_bin}" "$<TARGET_FILE_DIR:ps2EntryRunner>"
        VERBATIM
    )
    if(NOT WIN32)
        # Installed, the library is in lib/ next to the runner; in the build folder, next to it.
        set_target_properties(ps2EntryRunner PROPERTIES BUILD_RPATH "\$ORIGIN/lib;\$ORIGIN")
    endif()
endif()
target_link_libraries(ps2EntryRunner
    PRIVATE
    ps2_runtime
)

if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_VITA AND NOT PS2X_IS_ANDROID)
    target_sources(ps2EntryRunner PRIVATE
        src/lib/ps2_debug_panel.cpp
    )
    target_link_libraries(ps2EntryRunner PRIVATE rlImGui imgui)
    target_compile_definitions(ps2EntryRunner PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

ps2x_stage_ffmpeg_runtime_dlls(ps2EntryRunner)

# S2RecompLauncher.exe: settings, mods and Play, next to ps2EntryRunner.exe (launcher/launcher_main.cpp).
if((WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND TARGET rlImGui)
    # The UI fonts are compiled in (Roboto Medium and Droid Sans from Dear ImGui's misc/fonts, Apache 2.0), so the
    # launcher looks the same where Segoe UI is missing, such as under Proton on a Steam Deck.
    set(_launcher_fonts "// Generated by ps2xRuntime/CMakeLists.txt from Dear ImGui's misc/fonts.\n")
    foreach(_font Roboto-Medium DroidSans)
        file(READ "${PS2X_IMGUI_SOURCE_DIR}/misc/fonts/${_font}.ttf" _hex HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
        string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" _bytes "${_bytes}")
        string(MAKE_C_IDENTIFIER "${_font}" _sym)
        string(APPEND _launcher_fonts "extern unsigned char kFont_${_sym}[];\nextern const unsigned int kFont_${_sym}_size;\n"
                                      "unsigned char kFont_${_sym}[] = {\n${_bytes}};\nconst unsigned int kFont_${_sym}_size = sizeof(kFont_${_sym});\n")
    endforeach()
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/launcher_fonts.cpp" CONTENT "${_launcher_fonts}" @ONLY)

    add_executable(S2RecompLauncher WIN32
        launcher/launcher_main.cpp
        launcher/launcher_lobbies.cpp
        $<IF:$<BOOL:${WIN32}>,launcher/launcher_platform_win.cpp,launcher/launcher_platform_posix.cpp>
        "${CMAKE_CURRENT_BINARY_DIR}/launcher_fonts.cpp"
        $<IF:$<BOOL:${WIN32}>,${PS2X_APP_ICON_RC},${CMAKE_CURRENT_BINARY_DIR}/app_icon_png.cpp>
    )
    if(NOT WIN32)
        target_compile_definitions(S2RecompLauncher PRIVATE PS2X_APP_ICON_PNG=1)
    endif()
    target_link_libraries(S2RecompLauncher PRIVATE rlImGui imgui raylib)
    if(WIN32)
        target_link_libraries(S2RecompLauncher PRIVATE comdlg32 shell32 winhttp)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(S2RecompLauncher PRIVATE Threads::Threads)
    endif()
    if(MSVC)
        target_link_options(S2RecompLauncher PRIVATE "/ENTRY:mainCRTStartup")
    endif()
    if(_ps2x_eos_found)
        # The Lobbies tab logs in to EOS itself; the DLL is the one copied next to ps2EntryRunner above.
        set_source_files_properties(launcher/launcher_lobbies.cpp PROPERTIES
            COMPILE_DEFINITIONS "PS2X_EOS=1"
            INCLUDE_DIRECTORIES "${PS2X_EOS_SDK_DIR}/Include"
        )
        target_link_libraries(S2RecompLauncher PRIVATE "${_ps2x_eos_link}")
        if(NOT WIN32)
            set_target_properties(S2RecompLauncher PROPERTIES BUILD_RPATH "\$ORIGIN/lib;\$ORIGIN")
        endif()
    endif()
    add_dependencies(ps2EntryRunner S2RecompLauncher)
endif()

if(PS2X_IS_VITA)
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1") 
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

    if(PS2X_VITA_CREATE_PACKAGE)
        set(PS2X_VITA_VPK_FILES "")

        foreach(PS2X_VITA_MODULE IN ITEMS
            libgpu_es4_ext.suprx
            libIMGEGL.suprx
            libGLESv2.suprx
            libpvrPSP2_WSEGL.suprx
            libGL.suprx)
            set(PS2X_VITA_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vita/module/${PS2X_VITA_MODULE}")

            if(EXISTS "${PS2X_VITA_MODULE_PATH}")
                list(APPEND PS2X_VITA_VPK_FILES FILE "${PS2X_VITA_MODULE_PATH}" "module/${PS2X_VITA_MODULE}")
            else()
                message(WARNING
                    "Missing ${PS2X_VITA_MODULE_PATH}; the vpk will rely on ur0:data/external/ for it")
            endif()
        endforeach()

        vita_create_self(eboot.bin ps2EntryRunner UNSAFE)
        vita_create_vpk(ps2EntryRunner.vpk ${PS2X_VITA_TITLEID} eboot.bin
            VERSION ${PS2X_VITA_VERSION}
            NAME ${PS2X_VITA_APP_NAME}
            ${PS2X_VITA_VPK_FILES}
        )
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_runtime)
    EnableFastReleaseMode(ps2EntryRunner)

    if(WIN32 AND NOT PS2X_SHOW_WINDOWS_CONSOLE)
        if(MSVC)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/SUBSYSTEM:WINDOWS>
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/ENTRY:mainCRTStartup>
            )
        elseif(MINGW)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-mwindows>
            )
        endif()
    endif()
endif()

# Work around WinAPI vs raylib symbol clash for CloseWindow on x64
if(MSVC)
    target_link_options(ps2EntryRunner PRIVATE "/FORCE:MULTIPLE")
    # Compile the ~416 runner unity files 12 at a time (MSBuild --parallel only builds projects in
    # parallel; without /MP a full rebuild compiled one file at a time and took about an hour).
    target_compile_options(ps2EntryRunner PRIVATE /MP12)
    target_compile_options(ps2_runtime PRIVATE /MP12)
    # ps2EntryRunner.map: lets PS2X_PROF sample addresses be resolved to function names.
    target_link_options(ps2EntryRunner PRIVATE "/MAP")
endif()

# Vita: Work around for GCC 10's LTO plugin
if(PS2X_IS_VITA)
    target_link_options(ps2EntryRunner PRIVATE "-Wl,--allow-multiple-definition")
    target_sources(ps2EntryRunner PRIVATE src/lib/ps2_vita_runtime.cpp)
    set_source_files_properties(src/lib/ps2_vita_runtime.cpp PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION TRUE
        COMPILE_OPTIONS "-fno-lto"
    )
endif()

if(PS2X_IS_ANDROID)
    target_sources(ps2EntryRunner PRIVATE src/lib/ps2_android_runtime.cpp)
endif()

install(TARGETS ps2_runtime ps2EntryRunner
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if(WIN32 AND PS2X_ENABLE_FFMPEG)
    install(DIRECTORY "${FFMPEG_BIN_DIR}/"
        DESTINATION bin
        FILES_MATCHING
        PATTERN "*.dll"
    )
endif()

install(DIRECTORY include/
    DESTINATION include
)
