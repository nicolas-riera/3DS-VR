// Install and removal of the 3DS VR gamepad driver, a user-mode (UMDF 2)
// driver: Windows only asks that its package is signed by a trusted publisher,
// so it is signed here, on this PC. All in one elevated process:
//  1. copy the package (INF and DLL) to a temporary folder only admins can
//     write to, and check it is the package this app was built with,
//  2. make a code-signing certificate whose private key only lives in memory,
//  3. write the package's catalog (.cat) and sign it with that key,
//  4. trust the certificate (LocalMachine Root and TrustedPublisher). The key
//     is then dropped: nothing else can ever be signed with it,
//  5. create the root device node and install the driver on it.
// Removal undoes all of it: device, driver package, certificates.
#include "gamepad_setup.h"

#include <bcrypt.h>
#include <cfgmgr32.h>
#include <mscat.h>
#include <ncrypt.h>
#include <newdev.h>
#include <sddl.h>
#include <setupapi.h>

#include <cstdarg>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "vr3ds_pad.h"
#ifdef VR3DS_PAD_HASHES
#include "pad_hashes.h" // generated at build time
#endif

#define PAD_CERT_NAME L"3DS VR Gamepad Driver (made on this PC)"
#define PAD_DLL L"vr3ds_gamepad.dll"
#define PAD_CAT L"vr3ds_gamepad.cat"
#define PAD_CDF L"vr3ds_gamepad.cdf"

static const GUID kSystemClass = {0x4d36e97d, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
static const wchar_t* const kStores[] = {L"Root", L"TrustedPublisher"};

// not in the SDK headers (documented in mscat.h's reference, exported by wintrust.dll)
extern "C" LPWSTR WINAPI CryptCATCDFEnumMembersByCDFTagEx(CRYPTCATCDF* pCDF, LPWSTR pwszPrevCDFTag,
														  PFN_CDF_PARSE_ERROR_CALLBACK pfnParseError,
														  CRYPTCATMEMBER** ppMember, BOOL fContinueOnError,
														  LPVOID pvReserved);

// mssign32.dll, the signing engine of SignTool (documented, no SDK header)
struct SIGNER_FILE_INFO {
	DWORD cbSize;
	LPCWSTR pwszFileName;
	HANDLE hFile;
};
struct SIGNER_SUBJECT_INFO {
	DWORD cbSize;
	DWORD* pdwIndex;
	DWORD dwSubjectChoice; // 1 = file
	SIGNER_FILE_INFO* pSignerFileInfo;
};
struct SIGNER_CERT_STORE_INFO {
	DWORD cbSize;
	PCCERT_CONTEXT pSigningCert;
	DWORD dwCertPolicy; // 2 = add the chain
	HCERTSTORE hCertStore;
};
struct SIGNER_CERT {
	DWORD cbSize;
	DWORD dwCertChoice; // 2 = certificate context
	SIGNER_CERT_STORE_INFO* pCertStoreInfo;
	HWND hwnd;
};
struct SIGNER_ATTR_AUTHCODE {
	DWORD cbSize;
	BOOL fCommercial, fIndividual;
	LPCWSTR pwszName, pwszInfo;
};
struct SIGNER_SIGNATURE_INFO {
	DWORD cbSize;
	ALG_ID algidHash;
	DWORD dwAttrChoice; // 1 = Authenticode attributes
	SIGNER_ATTR_AUTHCODE* pAttrAuthcode;
	PCRYPT_ATTRIBUTES psAuthenticated, psUnauthenticated;
};
struct SIGNER_CONTEXT {
	DWORD cbSize, cbBlob;
	BYTE* pbBlob;
};
typedef HRESULT(WINAPI* SignerSignExFn)(DWORD, SIGNER_SUBJECT_INFO*, SIGNER_CERT*, SIGNER_SIGNATURE_INFO*, void*,
										LPCWSTR, PCRYPT_ATTRIBUTES, void*, SIGNER_CONTEXT**);
typedef HRESULT(WINAPI* SignerFreeSignerContextFn)(SIGNER_CONTEXT*);

// %APPDATA%\3DS VR\gamepad_setup.log, next to the SteamVR driver's log
static void SetupLog(const wchar_t* fmt, ...) {
	wchar_t msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vswprintf(msg, 1024, fmt, ap);
	va_end(ap);
	wchar_t* appdata = nullptr;
	size_t len = 0;
	if (_wdupenv_s(&appdata, &len, L"APPDATA") || !appdata) return;
	std::wstring dir = std::wstring(appdata) + L"\\3DS VR";
	free(appdata);
	CreateDirectoryW(dir.c_str(), nullptr);
	FILE* f = _wfopen((dir + L"\\gamepad_setup.log").c_str(), L"ab");
	if (!f) return;
	char utf8[3072];
	WideCharToMultiByte(CP_UTF8, 0, msg, -1, utf8, sizeof(utf8), nullptr, nullptr);
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, utf8);
	fclose(f);
}

// ------------------------------------------------------------------ checks

bool PadDriverRunning() {
	HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_VR3DS_PAD, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE) return false;
	SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
	bool ok = SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_VR3DS_PAD, 0, &ifd);
	SetupDiDestroyDeviceInfoList(set);
	return ok;
}

// Calls fn for each device node with our hardware ID.
template <typename F> static int ForEachPadDevice(bool presentOnly, F fn) {
	HDEVINFO set = SetupDiGetClassDevsW(&kSystemClass, nullptr, nullptr, presentOnly ? DIGCF_PRESENT : 0);
	if (set == INVALID_HANDLE_VALUE) return 0;
	int n = 0;
	SP_DEVINFO_DATA dev = {sizeof(dev)};
	for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
		wchar_t ids[512] = {};
		if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, (BYTE*)ids, sizeof(ids) - 4, nullptr))
			continue;
		for (const wchar_t* id = ids; *id; id += wcslen(id) + 1)
			if (!_wcsicmp(id, VR3DS_PAD_HARDWARE_ID)) {
				fn(set, &dev);
				n++;
				break;
			}
	}
	SetupDiDestroyDeviceInfoList(set);
	return n;
}

bool PadDriverInstalled() {
	return ForEachPadDevice(true, [](HDEVINFO, SP_DEVINFO_DATA*) {}) > 0;
}

// ----------------------------------------------------------------- signing

static NCRYPT_KEY_HANDLE NewKey() {
	NCRYPT_PROV_HANDLE prov = 0;
	NCRYPT_KEY_HANDLE key = 0;
	if (NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) return 0;
	// no name: an ephemeral key, never written to disk
	DWORD bits = 2048;
	if (NCryptCreatePersistedKey(prov, &key, BCRYPT_RSA_ALGORITHM, nullptr, 0, 0) != ERROR_SUCCESS ||
		NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, (PBYTE)&bits, sizeof(bits), 0) != ERROR_SUCCESS ||
		NCryptFinalizeKey(key, 0) != ERROR_SUCCESS) {
		if (key) NCryptFreeObject(key);
		key = 0;
	}
	NCryptFreeObject(prov);
	return key;
}

static PCCERT_CONTEXT NewCertificate(NCRYPT_KEY_HANDLE key) {
	BYTE name[256];
	DWORD nameLen = sizeof(name);
	if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=\"" PAD_CERT_NAME L"\"", CERT_X500_NAME_STR, nullptr, name, &nameLen,
						nullptr))
		return nullptr;
	CERT_NAME_BLOB subject = {nameLen, name};

	// code signing only
	LPSTR usage = (LPSTR)szOID_PKIX_KP_CODE_SIGNING;
	CERT_ENHKEY_USAGE eku = {1, &usage};
	BYTE ekuBuf[64], kuBuf[16];
	DWORD ekuLen = sizeof(ekuBuf), kuLen = sizeof(kuBuf);
	BYTE kuBits = CERT_DIGITAL_SIGNATURE_KEY_USAGE;
	CRYPT_BIT_BLOB ku = {1, &kuBits, 0};
	if (!CryptEncodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &eku, ekuBuf, &ekuLen) ||
		!CryptEncodeObject(X509_ASN_ENCODING, X509_KEY_USAGE, &ku, kuBuf, &kuLen))
		return nullptr;
	CERT_EXTENSION ext[2] = {{(LPSTR)szOID_ENHANCED_KEY_USAGE, FALSE, {ekuLen, ekuBuf}},
							 {(LPSTR)szOID_KEY_USAGE, TRUE, {kuLen, kuBuf}}};
	CERT_EXTENSIONS exts = {2, ext};
	CRYPT_ALGORITHM_IDENTIFIER alg = {(LPSTR)szOID_RSA_SHA256RSA};

	// from yesterday (clock differences) for 30 years: the driver must keep
	// installing on this PC after updates
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	ULARGE_INTEGER t = {{ft.dwLowDateTime, ft.dwHighDateTime}};
	t.QuadPart -= 24ull * 3600 * 10000000;
	ft = {t.LowPart, t.HighPart};
	SYSTEMTIME start, end;
	FileTimeToSystemTime(&ft, &start);
	end = start;
	end.wYear += 30;
	if (end.wMonth == 2 && end.wDay == 29) end.wDay = 28;
	return CertCreateSelfSignCertificate(key, &subject, CERT_CREATE_SELFSIGN_NO_KEY_INFO, nullptr, &alg, &start, &end,
										 &exts);
}

static bool g_cdfError;

static void WINAPI CdfError(DWORD area, DWORD error, WCHAR* line) {
	g_cdfError = true;
	SetupLog(L"Catalog definition error %lu/%lu: %s", area, error, line ? line : L"");
}

// What MakeCat does, with the attributes Inf2Cat writes for Windows 10 x64.
static bool WriteCatalog(const std::wstring& dir) {
	const char* cdf = "[CatalogHeader]\r\n"
					  "Name=vr3ds_gamepad.cat\r\n"
					  "PublicVersion=0x0000001\r\n"
					  "EncodingType=0x00010001\r\n"
					  "CatalogVersion=2\r\n"
					  "HashAlgorithms=SHA256\r\n"
					  "CATATTR1=0x10010001:OS:_v100_X64\r\n"
					  "CATATTR2=0x10010001:HWID1:root\\3dsvr_gamepad\r\n"
					  "[CatalogFiles]\r\n"
					  "<hash>vr3ds_gamepad.inf=vr3ds_gamepad.inf\r\n"
					  "<hash>vr3ds_gamepad.infATTR1=0x10010001:OSAttr:2:10.0\r\n"
					  "<hash>vr3ds_gamepad.infATTR2=0x10010001:File:vr3ds_gamepad.inf\r\n"
					  "<hash>vr3ds_gamepad.dll=vr3ds_gamepad.dll\r\n"
					  "<hash>vr3ds_gamepad.dllATTR1=0x10010001:OSAttr:2:10.0\r\n"
					  "<hash>vr3ds_gamepad.dllATTR2=0x10010001:File:vr3ds_gamepad.dll\r\n";
	FILE* f = _wfopen((dir + L"\\" PAD_CDF).c_str(), L"wb");
	if (!f) return false;
	fputs(cdf, f);
	fclose(f);

	// relative paths: the CDF parser does not need to handle the folder's name
	wchar_t cwd[MAX_PATH];
	GetCurrentDirectoryW(MAX_PATH, cwd);
	SetCurrentDirectoryW(dir.c_str());
	g_cdfError = false;
	bool ok = false;
	if (CRYPTCATCDF* c = CryptCATCDFOpen((LPWSTR)PAD_CDF, CdfError)) {
		CRYPTCATMEMBER* member = nullptr;
		LPWSTR tag = nullptr;
		while ((tag = CryptCATCDFEnumMembersByCDFTagEx(c, tag, CdfError, &member, TRUE, nullptr))) {}
		ok = CryptCATCDFClose(c) && !g_cdfError;
	}
	SetCurrentDirectoryW(cwd);
	DeleteFileW((dir + L"\\" PAD_CDF).c_str());
	return ok;
}

static HRESULT SignFile(const std::wstring& path, PCCERT_CONTEXT cert) {
	HMODULE dll = LoadLibraryExW(L"mssign32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!dll) return HRESULT_FROM_WIN32(GetLastError());
	auto signEx = (SignerSignExFn)GetProcAddress(dll, "SignerSignEx");
	auto freeContext = (SignerFreeSignerContextFn)GetProcAddress(dll, "SignerFreeSignerContext");
	HRESULT hr = E_NOTIMPL;
	if (signEx && freeContext) {
		SIGNER_FILE_INFO file = {sizeof(file), path.c_str(), nullptr};
		DWORD index = 0;
		SIGNER_SUBJECT_INFO subject = {sizeof(subject), &index, 1, &file};
		SIGNER_CERT_STORE_INFO store = {sizeof(store), cert, 2, nullptr};
		SIGNER_CERT signer = {sizeof(signer), 2, &store, nullptr};
		SIGNER_ATTR_AUTHCODE authcode = {sizeof(authcode), FALSE, TRUE, L"3DS VR gamepad driver", nullptr};
		SIGNER_SIGNATURE_INFO sig = {sizeof(sig), CALG_SHA_256, 1, &authcode, nullptr, nullptr};
		SIGNER_CONTEXT* ctx = nullptr;
		hr = signEx(0, &subject, &signer, &sig, nullptr, nullptr, nullptr, nullptr, &ctx);
		if (ctx) freeContext(ctx);
	}
	FreeLibrary(dll);
	return hr;
}

PCCERT_CONTEXT SignPadPackage(const std::wstring& dir) {
	NCRYPT_KEY_HANDLE key = NewKey();
	if (!key) {
		SetupLog(L"Could not create a signing key");
		return nullptr;
	}
	PCCERT_CONTEXT pub = nullptr;
	PCCERT_CONTEXT cert = NewCertificate(key);
	if (!cert) {
		SetupLog(L"Could not create the certificate (0x%08X)", GetLastError());
	} else {
		CERT_KEY_CONTEXT kc = {sizeof(kc)};
		kc.hNCryptKey = key;
		kc.dwKeySpec = CERT_NCRYPT_KEY_SPEC;
		HRESULT hr = E_FAIL;
		if (!CertSetCertificateContextProperty(cert, CERT_KEY_CONTEXT_PROP_ID, CERT_STORE_NO_CRYPT_RELEASE_FLAG, &kc))
			SetupLog(L"Could not attach the key (0x%08X)", GetLastError());
		else if (!WriteCatalog(dir))
			SetupLog(L"Could not write the catalog (0x%08X)", GetLastError());
		else if (FAILED(hr = SignFile(dir + L"\\" PAD_CAT, cert)))
			SetupLog(L"Could not sign the catalog (0x%08X)", hr);
		// the public part, without any link to the key
		if (SUCCEEDED(hr)) pub = CertCreateCertificateContext(X509_ASN_ENCODING, cert->pbCertEncoded, cert->cbCertEncoded);
		CertFreeCertificateContext(cert);
	}
	NCryptFreeObject(key); // gone for good
	return pub;
}

// ------------------------------------------------------------ certificates

static bool IsPadCert(PCCERT_CONTEXT c) {
	wchar_t name[128];
	CertGetNameStringW(c, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 128);
	return !wcscmp(name, PAD_CERT_NAME);
}

// Removes our certificates from both stores: only `only`, or all but `except`.
static int RemoveCertificates(PCCERT_CONTEXT only, PCCERT_CONTEXT except) {
	int n = 0;
	for (const wchar_t* s : kStores) {
		HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
										 CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG, s);
		if (!store) continue;
		PCCERT_CONTEXT c = nullptr;
		while ((c = CertEnumCertificatesInStore(store, c))) {
			if (!IsPadCert(c)) continue;
			if (only && !CertCompareCertificate(X509_ASN_ENCODING, c->pCertInfo, only->pCertInfo)) continue;
			if (except && CertCompareCertificate(X509_ASN_ENCODING, c->pCertInfo, except->pCertInfo)) continue;
			if (CertDeleteCertificateFromStore(CertDuplicateCertificateContext(c))) n++;
		}
		CertCloseStore(store, 0);
	}
	return n;
}

static DWORD TrustCertificate(PCCERT_CONTEXT cert) {
	for (const wchar_t* s : kStores) {
		HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, s);
		bool ok = store && CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, cert->pbCertEncoded,
															cert->cbCertEncoded, CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
		DWORD err = ok ? 0 : GetLastError();
		if (store) CertCloseStore(store, 0);
		if (!ok) {
			SetupLog(L"Could not add the certificate to %s (0x%08X)", s, err);
			return err ? err : ERROR_ACCESS_DENIED;
		}
	}
	return 0;
}

// ---------------------------------------------------------- device, package

// Driver store copies of our INF (oemNN.inf).
static std::vector<std::wstring> PadPackages() {
	std::vector<std::wstring> out;
	wchar_t win[MAX_PATH];
	GetWindowsDirectoryW(win, MAX_PATH);
	std::wstring dir = std::wstring(win) + L"\\INF\\";
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"oem*.inf").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return out;
	do {
		std::wstring path = dir + fd.cFileName;
		DWORD need = 0;
		SetupGetInfInformationW(path.c_str(), INFINFO_INF_NAME_IS_ABSOLUTE, nullptr, 0, &need);
		std::vector<BYTE> buf(need ? need : 1);
		auto* info = (SP_INF_INFORMATION*)buf.data();
		if (!need || !SetupGetInfInformationW(path.c_str(), INFINFO_INF_NAME_IS_ABSOLUTE, info, need, nullptr)) continue;
		SP_ORIGINAL_FILE_INFO_W orig = {sizeof(orig)};
		if (!SetupQueryInfOriginalFileInformationW(info, 0, nullptr, &orig)) continue;
		const wchar_t* base = wcsrchr(orig.OriginalInfName, L'\\');
		if (!_wcsicmp(base ? base + 1 : orig.OriginalInfName, VR3DS_PAD_INF_NAME)) out.push_back(fd.cFileName);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return out;
}

// Creates the root device node (once) and installs the driver on it, like
// "devcon install".
static DWORD InstallDevice(const std::wstring& inf, bool* reboot) {
	GUID cls;
	wchar_t clsName[MAX_CLASS_NAME_LEN];
	if (!SetupDiGetINFClassW(inf.c_str(), &cls, clsName, MAX_CLASS_NAME_LEN, nullptr)) return GetLastError();
	bool created = false;
	SP_DEVINFO_DATA dev = {sizeof(dev)};
	HDEVINFO set = SetupDiCreateDeviceInfoList(&cls, nullptr);
	if (set == INVALID_HANDLE_VALUE) return GetLastError();
	if (!ForEachPadDevice(false, [](HDEVINFO, SP_DEVINFO_DATA*) {})) {
		static const wchar_t hwid[] = VR3DS_PAD_HARDWARE_ID L"\0"; // REG_MULTI_SZ
		if (!SetupDiCreateDeviceInfoW(set, clsName, &cls, nullptr, nullptr, DICD_GENERATE_ID, &dev) ||
			!SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, (const BYTE*)hwid, sizeof(hwid)) ||
			!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev)) {
			DWORD err = GetLastError();
			SetupLog(L"Could not create the device (0x%08X)", err);
			SetupDiDestroyDeviceInfoList(set);
			return err;
		}
		created = true;
		SetupLog(L"Device created");
	}
	BOOL rb = FALSE;
	DWORD err = 0;
	if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, VR3DS_PAD_HARDWARE_ID, inf.c_str(), INSTALLFLAG_FORCE, &rb)) {
		err = GetLastError();
		SetupLog(L"Driver installation failed (0x%08X)", err);
		if (created) SetupDiCallClassInstaller(DIF_REMOVE, set, &dev);
	} else {
		*reboot = rb != FALSE;
		SetupLog(L"Driver installed%s", rb ? L" (restart needed)" : L"");
	}
	SetupDiDestroyDeviceInfoList(set);
	return err;
}

// Whatever is in the package gets signed and trusted: it must be the package
// this app was built with.
static bool CheckPackage(const std::wstring& dir) {
#ifdef VR3DS_PAD_HASHES
	struct {
		const wchar_t* name;
		const char* sha256;
	} files[] = {{VR3DS_PAD_INF_NAME, PAD_INF_SHA256}, {PAD_DLL, PAD_DLL_SHA256}};
	BCRYPT_ALG_HANDLE alg;
	if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return false;
	bool ok = true;
	for (const auto& f : files) {
		std::vector<BYTE> data;
		if (FILE* fp = _wfopen((dir + L"\\" + f.name).c_str(), L"rb")) {
			BYTE buf[65536];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) data.insert(data.end(), buf, buf + n);
			fclose(fp);
		}
		BYTE hash[32];
		char hex[65] = {};
		if (BCRYPT_SUCCESS(BCryptHash(alg, nullptr, 0, data.data(), (ULONG)data.size(), hash, sizeof(hash))))
			for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
		if (_stricmp(hex, f.sha256)) {
			SetupLog(L"%s is not the file this app was built with", f.name);
			ok = false;
		}
	}
	BCryptCloseAlgorithmProvider(alg, 0);
	return ok;
#else
	UNREFERENCED_PARAMETER(dir);
	SetupLog(L"This app was built without the gamepad driver");
	return false;
#endif
}

static void DeleteFolder(const std::wstring& dir) {
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

DWORD InstallPadDriver(const std::wstring& packageDir) {
	SetupLog(L"Installing the gamepad driver from %s", packageDir.c_str());
	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	std::wstring stage = std::wstring(tmp) + L"3DSVR-gamepad-" + std::to_wstring(GetTickCount64());
	// admins and the system only: nothing unelevated can swap the files
	// between the check and the signature
	SECURITY_ATTRIBUTES sa = {sizeof(sa)};
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1,
															  &sa.lpSecurityDescriptor, nullptr))
		return GetLastError();
	BOOL created = CreateDirectoryW(stage.c_str(), &sa);
	DWORD err = created ? 0 : GetLastError();
	LocalFree(sa.lpSecurityDescriptor);
	if (!created) return err;
	for (const wchar_t* f : {VR3DS_PAD_INF_NAME, PAD_DLL})
		if (!err && !CopyFileW((packageDir + L"\\" + f).c_str(), (stage + L"\\" + f).c_str(), FALSE)) {
			err = GetLastError();
			SetupLog(L"Missing %s (0x%08X)", f, err);
		}
	if (!err && !CheckPackage(stage)) err = ERROR_INVALID_IMAGE_HASH;
	PCCERT_CONTEXT cert = err ? nullptr : SignPadPackage(stage);
	if (!err && !cert) err = ERROR_INVALID_DATA;
	if (!err) err = TrustCertificate(cert);
	bool reboot = false;
	if (!err) err = InstallDevice(stage + L"\\" VR3DS_PAD_INF_NAME, &reboot);
	if (cert) {
		if (err) {
			RemoveCertificates(cert, nullptr);
		} else {
			// what an earlier install left: its certificate, and its package
			// (the one the device now uses is kept, it cannot be deleted)
			RemoveCertificates(nullptr, cert);
			for (const std::wstring& p : PadPackages())
				if (SetupUninstallOEMInfW(p.c_str(), 0, nullptr)) SetupLog(L"Removed the old package %s", p.c_str());
		}
		CertFreeCertificateContext(cert);
	}
	DeleteFolder(stage);
	SetupLog(err ? L"Install failed (0x%08X)" : L"Install done", err);
	return err ? err : reboot ? ERROR_SUCCESS_REBOOT_REQUIRED : ERROR_SUCCESS;
}

DWORD RemovePadDriver() {
	SetupLog(L"Removing the gamepad driver");
	DWORD err = 0;
	bool reboot = false;
	ForEachPadDevice(false, [&](HDEVINFO set, SP_DEVINFO_DATA* dev) {
		BOOL rb = FALSE;
		if (DiUninstallDevice(nullptr, set, dev, 0, &rb)) {
			reboot |= rb != FALSE;
			SetupLog(L"Device removed%s", rb ? L" (restart needed)" : L"");
		} else {
			err = GetLastError();
			SetupLog(L"Could not remove the device (0x%08X)", err);
		}
	});
	for (const std::wstring& p : PadPackages()) {
		if (SetupUninstallOEMInfW(p.c_str(), SUOI_FORCEDELETE, nullptr)) {
			SetupLog(L"Removed the package %s", p.c_str());
		} else {
			err = GetLastError();
			SetupLog(L"Could not remove the package %s (0x%08X)", p.c_str(), err);
		}
	}
	SetupLog(L"Removed %d certificate(s)", RemoveCertificates(nullptr, nullptr));
	return err ? err : reboot ? ERROR_SUCCESS_REBOOT_REQUIRED : ERROR_SUCCESS;
}
