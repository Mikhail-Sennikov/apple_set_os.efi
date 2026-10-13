#include <apple_set_os.h>
#include <debug/boot_environment.h>
#include <debug/gpu.h>
#include <efi/apple_os_info.h>

extern "C" {
#include <efi.h>
}

#define APPLE_SET_OS_VENDOR  "Apple Inc."
#define APPLE_SET_OS_VERSION "Mac OS X 10.9"

EFI_STATUS RunAppleSetOs(EFI_HANDLE image, EFI_SYSTEM_TABLE* systemTable) {
	if (systemTable == nullptr || systemTable->BootServices == nullptr) {
		return EFI_INVALID_PARAMETER;
	}

	NDebug::LogBootEnvironment(image, systemTable);
	NDebug::LogGpuList(/* systemTable */ systemTable, /* phase */ L"before");

	NEfi::TAppleOsInfo appleOsInfo(systemTable);
	const EFI_STATUS status = appleOsInfo.GetStatus();
	if (status == EFI_SUCCESS) {
		appleOsInfo.SetOsVendor(APPLE_SET_OS_VENDOR);
		appleOsInfo.SetOsVersion(APPLE_SET_OS_VERSION);
	}

	NDebug::LogGpuList(/* systemTable */ systemTable, /* phase */ L"after");
	NDebug::LogResult(systemTable, status);
	return status;
}
