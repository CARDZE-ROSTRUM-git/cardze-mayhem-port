cmake_minimum_required(VERSION 3.16)
project(mayhem-desktop LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(MAYHEM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mayhem-firmware" CACHE PATH "Mayhem firmware checkout")
set(FW "${MAYHEM_DIR}/firmware")

if(NOT EXISTS "${FW}/application/ui_navigation.cpp")
    message(FATAL_ERROR "Mayhem firmware not found at ${MAYHEM_DIR}")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

include(cmake/UpstreamSources.cmake)
# Every upstream external app (Games/RX/TX/Utilities/Settings/Debug tabs),
# compiled straight into the host binary. Generated from the upstream
# external.cmake by tools/gen_external.py.
include(cmake/ExternalApps.cmake)
# The firmware's real M4 receive processors (proc_*.cpp). Compiled so the
# packet-decoding apps actually decode; host_m4.cpp instantiates them.
include(cmake/M4Decoders.cmake)

set(PLATFORM_SOURCES
    platform/src/chibios.cpp
    platform/src/host_regs.cpp
    platform/src/portapack.cpp
    platform/src/lcd_sdl.cpp
    platform/src/input_controls.cpp
    platform/src/frame_sync.cpp
    platform/src/host_lcd.cpp
    platform/src/hardware_stubs.cpp
    platform/src/ff_stub.cpp
    platform/src/host_battery.cpp
    platform/src/host_file_reader.cpp
    platform/src/host_audio.cpp
    platform/src/host_external_items_menu_loader.cpp
    platform/src/host_launcher.cpp
    platform/src/host_settings.cpp
    platform/src/host_baseband.cpp
    platform/src/host_m4.cpp
    platform/src/host_buffer_exchange.cpp
    platform/src/sdr_device.cpp
    platform/src/main.cpp
)

# SDR backends are optional: each is compiled only when its library is present.
# The matching HOST_HAVE_* macro makes the device facade reference it.
pkg_check_modules(RTLSDR librtlsdr)
pkg_check_modules(IIO libiio)

if(RTLSDR_FOUND)
    list(APPEND PLATFORM_SOURCES platform/src/sdr_rtlsdr.cpp)
endif()
if(IIO_FOUND)
    list(APPEND PLATFORM_SOURCES platform/src/sdr_pluto.cpp)
endif()

add_executable(mayhem-desktop ${MAYHEM_UPSTREAM_SOURCES} ${MAYHEM_EXTERNAL_SOURCES} ${MAYHEM_M4_SOURCES} ${PLATFORM_SOURCES})

# The whole binary is built as the M0 application (see target_compile_definitions
# below), so the baseband processors are built the same way: no LPC43XX_M4
# branching, one consistent buffer_t layout. The only M4-specific need was
# Timestamp::now(), which the host RTC shim provides (see host_forced.hpp).
#
# Each processor ships an unused int main() that the image factory never calls;
# -ffunction-sections/-fdata-sections plus --gc-sections drop it and everything
# only it reached, so the dead M4 entry points cost nothing.
target_compile_options(mayhem-desktop PRIVATE -ffunction-sections -fdata-sections)
target_link_options(mayhem-desktop PRIVATE -Wl,--gc-sections)

# proc_flex.cpp carries its own copy of pocsag::EccContainer (see the comment in
# that file: "Implement EccContainer here to avoid linking pocsag.cpp which pulls
# in app headers"). On the device that is correct -- the flex image is a separate
# ELF that never links firmware/common/pocsag.cpp. In this single desktop binary
# we must link pocsag.cpp anyway (pocsag_app / battleship / pocsag_rx need it),
# so the two definitions collide. They are verified identical (differ only in
# comments), so allowing the overlap lets the linker keep either one. This is
# scoped to that one upstream duplication; the other known M4/M0 name clash
# (EventDispatcher) is fixed properly via platform/include/event_m4.hpp, not by
# this flag.
target_link_options(mayhem-desktop PRIVATE -Wl,--allow-multiple-definition)

# The shared baseband support files (spectrum_collector.cpp and friends) include
# the firmware's own baseband/event_m4.hpp by quoted include, which resolves to
# the file next to them. Pre-including the host shadow (platform/include is the
# first -I) defines its guard first, so those includes become no-ops and the
# processors see the host EventDispatcher instead of the M4 one. Without this
# the M4 dispatcher would clash with the M0 application's identically named
# class (firmware/application/event_m0.cpp).
#
# C++ only: MAYHEM_M4_SOURCES also carries the FT8 decoder library (plain C,
# ft8_lib/*.c + ft8_portapack.c), which must not see the C++ shadow header.
set_source_files_properties(${MAYHEM_M4_SOURCES} PROPERTIES
    COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:CXX>:-include;event_m4.hpp>")

# vdl2_decoder.cpp declares its own `constexpr float PI`; the force-included
# complex.hpp -> mathdef.hpp chain defines a `PI` macro that would collide. On
# the device that file never saw the macro. Ask host_forced.hpp to #undef it for
# this one translation unit only (see HOST_UNDEF_PI there); every other file
# keeps the macro, so adsb/sonde keep compiling.
set_source_files_properties(${FW}/baseband/vdl2_decoder.cpp PROPERTIES
    COMPILE_DEFINITIONS HOST_UNDEF_PI)

# These two host sources use the real C++17 <filesystem>; the firmware's own
# filesystem shim (force-included for every other TU) must be skipped for them
# so that only one std::filesystem definition exists per translation unit.
set_source_files_properties(
    platform/src/ff_stub.cpp
    platform/src/host_regs.cpp
    platform/src/host_launcher.cpp
    PROPERTIES COMPILE_DEFINITIONS HOST_STD_FILESYSTEM)

target_include_directories(mayhem-desktop PRIVATE
    platform/include
    ${FW}/common
    ${FW}/application
    ${FW}/application/ui
    ${FW}/application/hw
    ${FW}/application/apps
    ${FW}/application/protocols
    ${FW}/application/bmps
    ${FW}/application/external
    # Baseband dir last: it also holds a debug.hpp, and the M0 application's
    # debug.hpp must win for the application translation units.
    ${FW}/baseband
    ${FW}/chibios-portapack/ext/fatfs/src
    ${FW}/chibios-portapack/os/hal/platforms/LPC43xx
    ${SDL2_INCLUDE_DIRS}
    ${RTLSDR_INCLUDE_DIRS}
    ${IIO_INCLUDE_DIRS}
)

target_compile_definitions(mayhem-desktop PRIVATE
    LPC43XX
    LPC43XX_M0
    __NEWLIB__
    HACKRF_ONE
    TOOLCHAIN_GCC
    TOOLCHAIN_GCC_ARM
    FLASH_TIER=0
    _RANDOM_TCC=0
    FLASH_SIZE_MB=8
    FLASH_SIZE_LIMIT_MB=8
    VERSION_STRING="desktop"
    VERSION_MD5=0
    GCC_VERSION_MISMATCH=0
    # Host replacement for the MCU-only lpc43xx_cpp.hpp.
    __LPC43XX_CPP_H__
)

if(RTLSDR_FOUND)
    target_compile_definitions(mayhem-desktop PRIVATE HOST_HAVE_RTLSDR)
endif()
if(IIO_FOUND)
    target_compile_definitions(mayhem-desktop PRIVATE HOST_HAVE_PLUTO)
endif()

target_compile_options(mayhem-desktop PRIVATE
    # host_forced.hpp is C++ (namespaces, templates): only force-include it into
    # C++ translation units. The FT8 library (ft8_lib/*.c) is plain C and is
    # compiled as such, so it must not see the C++ shim.
    $<$<COMPILE_LANGUAGE:CXX>:-include ${CMAKE_CURRENT_SOURCE_DIR}/platform/include/host_forced.hpp>
    -Wall -Wextra
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-missing-field-initializers -Wno-sign-compare -Wno-narrowing
    -Wno-deprecated-declarations -Wno-psabi -Wno-stringop-overflow
    -Wno-array-bounds -Wno-class-memaccess -Wno-cpp -fpermissive
)

target_link_libraries(mayhem-desktop PRIVATE ${SDL2_LIBRARIES} pthread)

if(RTLSDR_FOUND)
    target_include_directories(mayhem-desktop PRIVATE ${RTLSDR_INCLUDE_DIRS})
    target_link_libraries(mayhem-desktop PRIVATE ${RTLSDR_LIBRARIES})
endif()
if(IIO_FOUND)
    target_include_directories(mayhem-desktop PRIVATE ${IIO_INCLUDE_DIRS})
    target_link_libraries(mayhem-desktop PRIVATE ${IIO_LIBRARIES})
endif()
