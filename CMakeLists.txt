cmake_minimum_required(VERSION 3.16)
project(CellScope C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(OpenGL REQUIRED)
find_package(PkgConfig)

find_package(glfw3 CONFIG QUIET)
if(NOT glfw3_FOUND)
    if(NOT PkgConfig_FOUND)
        find_package(PkgConfig REQUIRED)
    endif()
    pkg_check_modules(GLFW REQUIRED glfw3)
endif()

# RTL-SDR
if(PkgConfig_FOUND)
    pkg_check_modules(RTLSDR QUIET librtlsdr)
endif()
if(NOT RTLSDR_FOUND)
    find_library(RTLSDR_LIBRARIES NAMES rtlsdr librtlsdr REQUIRED)
    find_path(RTLSDR_INCLUDE_DIRS NAMES rtl-sdr.h REQUIRED)
endif()

# HackRF (native source). The MSYS2 libhackrf.pc has a hardcoded build-machine
# libusb include path, so locate the header/library directly instead of via
# pkg-config. hackrf.h itself doesn't pull in libusb, so no extra include is
# needed for our wrapper.
find_path(HACKRF_INCLUDE_DIRS NAMES libhackrf/hackrf.h hackrf.h
          PATH_SUFFIXES libhackrf REQUIRED)
find_library(HACKRF_LIBRARIES NAMES hackrf libhackrf REQUIRED)

# Airspy (native source, optional).
# Vendored headers always available; library detection is optional.
set(AIRSPY_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/airspy)
find_library(AIRSPY_LIBRARIES NAMES airspy libairspy)
set(AIRSPY_FOUND FALSE)
if(AIRSPY_LIBRARIES)
    set(AIRSPY_FOUND TRUE)
    message(STATUS "Airspy link library found: ${AIRSPY_LIBRARIES}")
endif()

# LibreSDR (USRP B210 clone) via UHD. Linux-only: UHD loads our custom FPGA
# bitstream (libresdr_b210.bin) onto the device by passing fpga=<path> in the
# device args at make() time. Prefer pkg-config (uses the /usr/local UHD install
# and avoids the imported-target quirk in some UHDConfig.cmake versions), then
# fall back to the CMake config package.
set(LIBRESDR_FOUND FALSE)
if(NOT WIN32)
    set(UHD_FOUND FALSE)
    if(PkgConfig_FOUND)
        pkg_check_modules(UHD QUIET uhd)
    endif()
    if(NOT UHD_FOUND)
        find_package(UHD CONFIG QUIET)
    endif()
    if(UHD_FOUND)
        message(STATUS "UHD found: ${UHD_VERSION}")
        set(LIBRESDR_FOUND TRUE)
    endif()
endif()

set(IMGUI_DIR   ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMPLOT_DIR  ${CMAKE_SOURCE_DIR}/third_party/implot)

# ---------------------------------------------------------------------------
# LTE stack (srsRAN2 + FALCON + LTESniffer), vendored under third_party/lte.
# Enabled by default; builds the decode libraries + the CellScope LTE engine
# bridge (exposed via the cellscope_lte aggregate target).
# ---------------------------------------------------------------------------
option(HAS_LTE "Build the vendored LTE (srsRAN/FALCON/LTESniffer) stack" ON)
if(HAS_LTE)
    add_subdirectory(third_party/lte)
endif()

# ---------------------------------------------------------------------------
# Dear ImGui + ImPlot (vendored) compiled as a static lib
# ---------------------------------------------------------------------------
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMPLOT_DIR}
)
# implot_items.cpp is extremely template-heavy: ~5 min to compile at -O2.
# It is not performance-critical for us, so build it at -O1 (~1 min). The last
# -O flag wins in GCC, so this overrides the RelWithDebInfo -O2.
set_source_files_properties(${IMPLOT_DIR}/implot_items.cpp PROPERTIES COMPILE_OPTIONS "-O1")
target_link_libraries(imgui PUBLIC OpenGL::GL)
if(glfw3_FOUND)
    target_link_libraries(imgui PUBLIC glfw)
else()
    target_include_directories(imgui PUBLIC ${GLFW_INCLUDE_DIRS})
    target_link_libraries(imgui PUBLIC ${GLFW_LIBRARIES})
    target_link_directories(imgui PUBLIC ${GLFW_LIBRARY_DIRS})
endif()

# ---------------------------------------------------------------------------
# CellScope application
# ---------------------------------------------------------------------------
add_executable(CellScope
    src/main.cpp
    src/signal/processing.cpp
    src/voice/voice_ops.cpp
    src/gui/gui_panels.cpp
    src/session/session.cpp
    src/state/config.cpp
    src/sdr/rtl_sdr_source.cpp
    src/sdr/hackrf_source.cpp
    
    src/sdr/wav_file_source.cpp
    src/sdr/sdrpp_server_source.cpp
    src/sdr/iq_recorder.cpp
    src/dsp/jfft.cpp
    src/gui/waterfall.cpp
    src/decode/decoder.cpp
    src/decode/decoder_manager.cpp
    src/decode/band_plan.cpp
    src/decode/json_mini.cpp
    src/util/log.cpp
    src/decode/egc/egc_decoder.cpp
    src/i18n/i18n.cpp
    src/update/version_check.cpp
)
target_include_directories(CellScope PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${RTLSDR_INCLUDE_DIRS}
    ${HACKRF_INCLUDE_DIRS}
)
target_compile_definitions(CellScope PRIVATE _USE_MATH_DEFINES)
if(AIRSPY_FOUND)
    target_sources(CellScope PRIVATE src/sdr/airspy_source.cpp)
    target_compile_definitions(CellScope PRIVATE HAS_AIRSPY=1)
    target_include_directories(CellScope PRIVATE ${AIRSPY_INCLUDE_DIRS})
endif()
if(LIBRESDR_FOUND)
    target_sources(CellScope PRIVATE src/sdr/libresdr_source.cpp)
    target_compile_definitions(CellScope PRIVATE HAS_LIBRESDR=1)
    target_include_directories(CellScope PRIVATE ${UHD_INCLUDE_DIRS})
    target_link_directories(CellScope PRIVATE ${UHD_LIBRARY_DIRS})
endif()
find_library(ZSTD_LIBRARIES NAMES zstd libzstd REQUIRED)
find_library(ZLIB_LIB NAMES z libz zlib REQUIRED)

target_link_libraries(CellScope PRIVATE imgui ${ZLIB_LIB} ${RTLSDR_LIBRARIES} ${HACKRF_LIBRARIES} ${ZSTD_LIBRARIES})
if(AIRSPY_FOUND)
    target_link_libraries(CellScope PRIVATE ${AIRSPY_LIBRARIES})
endif()
if(LIBRESDR_FOUND)
    target_link_libraries(CellScope PRIVATE ${UHD_LIBRARIES})
endif()
if(HAS_LTE)
    target_link_libraries(CellScope PRIVATE cellscope_lte)
    target_compile_definitions(CellScope PRIVATE HAS_LTE=1)
    target_sources(CellScope PRIVATE src/lte/lte_diag.cpp)
    if(WIN32)
        target_link_libraries(CellScope PRIVATE dbghelp psapi)
    endif()
endif()
target_link_directories(CellScope PRIVATE ${RTLSDR_LIBRARY_DIRS})

if(WIN32)
    target_link_libraries(CellScope PRIVATE opengl32 gdi32 comdlg32 ole32 winmm ws2_32 winhttp shell32)

    # Build as a GUI-subsystem app so no console window opens alongside the GUI.
    # MinGW provides a WinMain shim, so the existing int main() still works.
    set_target_properties(CellScope PROPERTIES WIN32_EXECUTABLE TRUE)

    # Embed the application icon (icon.ico in the project root). Windows and
    # GLFW both pick up the first icon resource for the exe and window.
    if(EXISTS "${CMAKE_SOURCE_DIR}/icon.ico")
        enable_language(RC)
        set(FRAMESYNC_ICON "${CMAKE_SOURCE_DIR}/icon.ico")
        configure_file("${CMAKE_SOURCE_DIR}/app.rc.in" "${CMAKE_BINARY_DIR}/app.rc" @ONLY)
        target_sources(CellScope PRIVATE "${CMAKE_BINARY_DIR}/app.rc")
    endif()

    # Copy MinGW runtime DLLs next to the executable so it runs standalone
    # (PyCharm, double-click) without the MSYS2 bin dir on PATH.
    get_filename_component(MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(FRAMESYNC_RUNTIME_DLLS
        libgcc_s_seh-1.dll
        libwinpthread-1.dll
        libstdc++-6.dll
        glfw3.dll
        librtlsdr.dll
        libhackrf.dll
        libusb-1.0.dll
        libzstd.dll
        zlib1.dll
    )
    if(AIRSPY_FOUND)
        list(APPEND FRAMESYNC_RUNTIME_DLLS libairspy.dll)
    endif()
    if(HAS_LTE)
        list(APPEND FRAMESYNC_RUNTIME_DLLS libfftw3f-3.dll)
    endif()
    foreach(dll ${FRAMESYNC_RUNTIME_DLLS})
        if(EXISTS "${MINGW_BIN}/${dll}")
            add_custom_command(TARGET CellScope POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${MINGW_BIN}/${dll}" "$<TARGET_FILE_DIR:CellScope>"
                COMMENT "Copying ${dll}")
        else()
            message(WARNING "Runtime DLL not found: ${MINGW_BIN}/${dll}")
        endif()
    endforeach()
endif()
