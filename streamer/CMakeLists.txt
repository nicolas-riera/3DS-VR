cmake_minimum_required(VERSION 3.20)
project(3dsvr_streamer VERSION 1.0.0 LANGUAGES C CXX)

# Builds:
#   driver_3dsvr.dll  SteamVR driver: virtual headset, encoder, network, audio, gamepad
#   3DS VR.exe        the app the user runs: setup, pairing, status
# into build/dist/3DS VR/ (the folder that ships).

if(NOT WIN32)
	message(FATAL_ERROR "The streamer is Windows-only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static CRT: nothing to install next to SteamVR
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

include(FetchContent)

# OpenVR driver API (header only)
set(OPENVR_TAG "v2.5.1")
set(OPENVR_HEADER "${CMAKE_BINARY_DIR}/openvr/openvr_driver.h")
if(NOT EXISTS "${OPENVR_HEADER}")
	file(DOWNLOAD "https://raw.githubusercontent.com/ValveSoftware/openvr/${OPENVR_TAG}/headers/openvr_driver.h"
		"${OPENVR_HEADER}" STATUS dl_status TLS_VERIFY ON)
	list(GET dl_status 0 dl_code)
	if(NOT dl_code EQUAL 0)
		file(REMOVE "${OPENVR_HEADER}")
		message(FATAL_ERROR "Could not download openvr_driver.h: ${dl_status}")
	endif()
endif()

# ViGEm client (virtual Xbox 360 controller)
FetchContent_Declare(vigemclient
	GIT_REPOSITORY https://github.com/nefarius/ViGEmClient.git
	GIT_TAG v1.21.222.0
	GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(vigemclient)

set(DIST_DIR "${CMAKE_BINARY_DIR}/dist/3DS VR")
set(DRIVER_DIR "${DIST_DIR}/driver/3dsvr")

# ------------------------------------------------------------------ driver
add_library(driver_3dsvr SHARED
	driver/src/driver_main.cpp
	driver/src/hmd.cpp
	driver/src/compositor.cpp
	driver/src/streamer.cpp
	driver/src/encoder_h264.cpp
	driver/src/encoder_jpeg.cpp
	driver/src/audio_capture.cpp
	driver/src/gamepad.cpp
	driver/src/pairing.cpp
	driver/src/log.cpp
)
target_include_directories(driver_3dsvr PRIVATE
	"${CMAKE_BINARY_DIR}/openvr"
	"${CMAKE_SOURCE_DIR}/../protocol"
	"${CMAKE_SOURCE_DIR}/common")
target_link_libraries(driver_3dsvr PRIVATE ViGEmClient setupapi
	d3d11 dxgi d3dcompiler mfplat mfuuid mf windowscodecs ole32 ws2_32 winmm avrt)
target_compile_options(driver_3dsvr PRIVATE /W3 /permissive-)
set_target_properties(driver_3dsvr PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${DRIVER_DIR}/bin/win64"
	RUNTIME_OUTPUT_DIRECTORY_RELEASE "${DRIVER_DIR}/bin/win64"
	RUNTIME_OUTPUT_DIRECTORY_DEBUG "${DRIVER_DIR}/bin/win64"
	RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${DRIVER_DIR}/bin/win64")

# driver manifest and resources
add_custom_command(TARGET driver_3dsvr POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/driver/driver.vrdrivermanifest" "${DRIVER_DIR}/driver.vrdrivermanifest"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/driver/resources" "${DRIVER_DIR}/resources")

# --------------------------------------------------------------------- app
add_executable(app WIN32
	app/main.cpp
	app/app.rc
)
target_include_directories(app PRIVATE "${CMAKE_SOURCE_DIR}/common" "${CMAKE_SOURCE_DIR}/../protocol")
target_link_libraries(app PRIVATE ViGEmClient setupapi ws2_32 shell32 advapi32 urlmon dwmapi gdi32 gdiplus)
target_link_options(app PRIVATE /MANIFEST:NO)
target_compile_options(app PRIVATE /W3 /permissive-)
set_target_properties(app PROPERTIES
	OUTPUT_NAME "3DS VR"
	RUNTIME_OUTPUT_DIRECTORY "${DIST_DIR}"
	RUNTIME_OUTPUT_DIRECTORY_RELEASE "${DIST_DIR}"
	RUNTIME_OUTPUT_DIRECTORY_DEBUG "${DIST_DIR}"
	RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${DIST_DIR}")
