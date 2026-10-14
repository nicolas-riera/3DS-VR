cmake_minimum_required(VERSION 3.20)
project(3dsvr_streamer VERSION 1.0.0 LANGUAGES C CXX)

# Builds:
#   driver_3dsvr.dll  SteamVR driver: virtual headset, encoder, network, audio, gamepad
#   3DS VR.exe        the app the user runs: setup, pairing, status
#   vr3ds_gamepad.dll the gamepad driver (virtual Xbox 360 controller), with its INF
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
	driver/src/controls.cpp
	driver/src/hands.cpp
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

# ---------------------------------------------------------- gamepad driver
# User-mode (UMDF 2) driver: no kernel code, so the app can sign it on the
# user's PC. The UMDF headers and stub library come from the WDK NuGet package.
option(VR3DS_GAMEPAD_DRIVER "Build the gamepad driver (downloads the WDK, about 110 MB, once)" ON)
if(VR3DS_GAMEPAD_DRIVER)
	set(WDK_VERSION "10.0.26100.6584")
	set(UMDF_MINOR 15) # UMDF 2.15: Windows 10 1507 and later
	set(WDK_DIR "${CMAKE_BINARY_DIR}/wdk/${WDK_VERSION}")
	if(NOT EXISTS "${WDK_DIR}/extracted")
		set(nupkg "${CMAKE_BINARY_DIR}/wdk/wdk-${WDK_VERSION}.nupkg")
		message(STATUS "Downloading the WDK ${WDK_VERSION} from NuGet (about 110 MB)")
		file(DOWNLOAD "https://api.nuget.org/v3-flatcontainer/microsoft.windows.wdk.x64/${WDK_VERSION}/microsoft.windows.wdk.x64.${WDK_VERSION}.nupkg"
			"${nupkg}" STATUS dl_status TLS_VERIFY ON
			EXPECTED_HASH SHA512=8e175d6819e1303aaddc656bdf64554ed691d0a1e66438d8d09093327d74390a64e3e01285708af50034f6f05ace9f278b5323bbce2a3394865df21f9f2389fa)
		list(GET dl_status 0 dl_code)
		if(NOT dl_code EQUAL 0)
			file(REMOVE "${nupkg}")
			message(FATAL_ERROR "Could not download the WDK: ${dl_status}")
		endif()
		file(ARCHIVE_EXTRACT INPUT "${nupkg}" DESTINATION "${WDK_DIR}"
			PATTERNS "c/Include/wdf/umdf/2.${UMDF_MINOR}/*" "c/Lib/wdf/umdf/x64/2.${UMDF_MINOR}/*" "LICENSE.txt")
		file(REMOVE "${nupkg}")
		file(TOUCH "${WDK_DIR}/extracted")
	endif()

	set(PAD_DIR "${DIST_DIR}/gamepad")
	add_library(gamepad_driver SHARED gamepad_driver/gamepad_driver.cpp)
	target_include_directories(gamepad_driver PRIVATE "${WDK_DIR}/c/Include/wdf/umdf/2.${UMDF_MINOR}" "${CMAKE_SOURCE_DIR}/common")
	target_compile_definitions(gamepad_driver PRIVATE UMDF_VERSION_MAJOR=2 UMDF_VERSION_MINOR=${UMDF_MINOR} UMDF_USING_NTSTATUS)
	target_link_libraries(gamepad_driver PRIVATE "${WDK_DIR}/c/Lib/wdf/umdf/x64/2.${UMDF_MINOR}/WdfDriverStubUm.lib" ntdll)
	target_compile_options(gamepad_driver PRIVATE /W3 /permissive- /utf-8)
	set_target_properties(gamepad_driver PROPERTIES
		OUTPUT_NAME "vr3ds_gamepad"
		RUNTIME_OUTPUT_DIRECTORY "${PAD_DIR}"
		RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PAD_DIR}"
		RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PAD_DIR}"
		RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${PAD_DIR}")
	add_custom_command(TARGET gamepad_driver POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/gamepad_driver/vr3ds_gamepad.inf" "${PAD_DIR}/vr3ds_gamepad.inf")
endif()

# console tool that prints what each XInput DLL sees (testing, not shipped)
add_executable(padtest gamepad_driver/padtest.cpp)
target_compile_options(padtest PRIVATE /W3 /permissive- /utf-8)

# --------------------------------------------------------------------- app
add_executable(app WIN32
	app/main.cpp
	app/gamepad_setup.cpp
	app/app.rc
)
target_include_directories(app PRIVATE "${CMAKE_SOURCE_DIR}/common" "${CMAKE_SOURCE_DIR}/../protocol")
target_link_libraries(app PRIVATE ViGEmClient setupapi newdev crypt32 ncrypt wintrust bcrypt
	ws2_32 shell32 advapi32 dwmapi gdi32 gdiplus)
target_link_options(app PRIVATE /MANIFEST:NO)
target_compile_options(app PRIVATE /W3 /permissive-)
set_target_properties(app PROPERTIES
	OUTPUT_NAME "3DS VR"
	RUNTIME_OUTPUT_DIRECTORY "${DIST_DIR}"
	RUNTIME_OUTPUT_DIRECTORY_RELEASE "${DIST_DIR}"
	RUNTIME_OUTPUT_DIRECTORY_DEBUG "${DIST_DIR}"
	RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${DIST_DIR}")

if(VR3DS_GAMEPAD_DRIVER)
	# the app checks these hashes before it signs the gamepad package
	set(PAD_HASHES "${CMAKE_BINARY_DIR}/generated/pad_hashes.h")
	add_custom_command(OUTPUT "${PAD_HASHES}"
		COMMAND ${CMAKE_COMMAND} "-DDLL=$<TARGET_FILE:gamepad_driver>"
			"-DINF=${CMAKE_SOURCE_DIR}/gamepad_driver/vr3ds_gamepad.inf" "-DOUT=${PAD_HASHES}"
			-P "${CMAKE_SOURCE_DIR}/gamepad_driver/hashes.cmake"
		DEPENDS gamepad_driver gamepad_driver/vr3ds_gamepad.inf gamepad_driver/hashes.cmake
		VERBATIM)
	target_sources(app PRIVATE "${PAD_HASHES}")
	target_include_directories(app PRIVATE "${CMAKE_BINARY_DIR}/generated")
	target_compile_definitions(app PRIVATE VR3DS_PAD_HASHES)
endif()
