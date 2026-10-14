// The 3DS VR gamepad driver (gamepad\ next to 3DS VR.exe): status checks, and
// the install and removal that run elevated ("3DS VR.exe --install-gamepad"
// and "--remove-gamepad", started by the app with one admin prompt).
#pragma once
#include <windows.h>
#include <wincrypt.h>

#include <string>

#define PAD_ARG_INSTALL L"--install-gamepad"
#define PAD_ARG_REMOVE  L"--remove-gamepad"

bool PadDriverRunning();   // started: the SteamVR driver can plug the controller in
bool PadDriverInstalled(); // its device exists, even if it does not start

// Process exit codes: ERROR_SUCCESS, ERROR_SUCCESS_REBOOT_REQUIRED or an error.
DWORD InstallPadDriver(const std::wstring& packageDir);
DWORD RemovePadDriver();

// Writes the catalog of the package in `dir` and signs it with a new key that
// only lives in memory. Returns the certificate (public part only).
PCCERT_CONTEXT SignPadPackage(const std::wstring& dir);
