// Copyright (c) 2015 Bruno Bierbaumer

#include <apple_set_os.h>
#include <efi/apple_os_info.h>
#include <util/console_out.h>

extern "C" {
#include <efi.h>
}

#define APPLE_SET_OS_VENDOR  "Apple Inc."
#define APPLE_SET_OS_VERSION "Mac OS X 10.9"

EFI_STATUS apple_set_os_run(EFI_HANDLE image, EFI_SYSTEM_TABLE* systemTable) {
	if (systemTable == nullptr || systemTable->BootServices == nullptr) {
		return EFI_INVALID_PARAMETER;
	}

	TConsoleOut conOut(systemTable);
	conOut.Write(L"apple_set_os started\r\n");

	TAppleOsInfo appleOsInfo(systemTable);
	EFI_STATUS status = appleOsInfo.GetStatus();
	if (status != EFI_SUCCESS) {
		conOut.Write(L"Could not locate the apple set os protocol.\r\n");
		return status;
	}

	if (appleOsInfo.SetOsVendor(APPLE_SET_OS_VENDOR)) {
		conOut.Write(L"Set os vendor to " APPLE_SET_OS_VENDOR ".\r\n");
	}

	if (appleOsInfo.SetOsVersion(APPLE_SET_OS_VERSION)) {
		conOut.Write(L"Set os version to " APPLE_SET_OS_VERSION ".\r\n");
	}

	return EFI_SUCCESS;
}
