// Copyright (c) 2015 Bruno Bierbaumer

#include <apple_set_os.h>

extern "C" {
#include <efi.h>
}

#define APPLE_SET_OS_VENDOR  "Apple Inc."
#define APPLE_SET_OS_VERSION "Mac OS X 10.9"

static EFI_GUID APPLE_SET_OS_GUID = {
	.Data1 = 0xc5c5da95,
	.Data2 = 0x7d5c,
	.Data3 = 0x45e6,
	.Data4 = { 0xb2, 0xf1, 0x3f, 0xd5, 0x2b, 0xb1, 0x00, 0x77 }
};

struct TAppleSetOsProtocol {
	UINT64 version;
	EFI_STATUS (EFIAPI *setOsVersion)(CHAR8 *version);
	EFI_STATUS (EFIAPI *setOsVendor)(CHAR8 *vendor);
};

EFI_STATUS apple_set_os_run(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable) {
	SIMPLE_TEXT_OUTPUT_INTERFACE *conOut = systemTable->ConOut;
	conOut->OutputString(conOut, (CHAR16 *)L"apple_set_os started\r\n");

	void *protocol = nullptr;

	EFI_STATUS status = systemTable->BootServices->LocateProtocol(
		&APPLE_SET_OS_GUID, nullptr, &protocol);
	TAppleSetOsProtocol *setOs = static_cast<TAppleSetOsProtocol *>(protocol);
	if (static_cast<INTN>(status) < 0 || setOs == nullptr) {
		conOut->OutputString(conOut, (CHAR16 *)L"Could not locate the apple set os protocol.\r\n");
		return status;
	}

	if (setOs->version != 0) {
		status = setOs->setOsVersion((CHAR8 *)APPLE_SET_OS_VERSION);
		if (static_cast<INTN>(status) < 0) {
			conOut->OutputString(conOut, (CHAR16 *)L"Could not set version.\r\n");
			return status;
		}
		conOut->OutputString(conOut,
			(CHAR16 *)L"Set os version to " APPLE_SET_OS_VERSION ".\r\n");
	}

	status = setOs->setOsVendor((CHAR8 *)APPLE_SET_OS_VENDOR);
	if (static_cast<INTN>(status) < 0) {
		conOut->OutputString(conOut, (CHAR16 *)L"Could not set vendor.\r\n");
		return status;
	}
	conOut->OutputString(conOut,
		(CHAR16 *)L"Set os vendor to " APPLE_SET_OS_VENDOR ".\r\n");

	return EFI_SUCCESS;
}
