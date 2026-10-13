#include <debug/boot_environment.h>
#include <debug/loaded_image.h>
#include <logging/logging.h>

extern "C" {
#include <efi.h>
}

#ifndef APPLE_SET_OS_BUILD_ID
#define APPLE_SET_OS_BUILD_ID "unknown"
#endif

namespace NDebug {

static const CHAR16* AsEfiText(const wchar_t* text) {
	return reinterpret_cast<const CHAR16*>(text);
}

static void LogRunIdentity(EFI_SYSTEM_TABLE* systemTable) {
	const auto* runtimeServices = systemTable->RuntimeServices;
	const auto* bootServices = systemTable->BootServices;
	EFI_TIME time = {};
	uint64_t counter = 0;
	EFI_STATUS timeStatus = EFI_UNSUPPORTED;
	if (runtimeServices != nullptr && runtimeServices->GetTime != nullptr) {
		timeStatus = runtimeServices->GetTime(&time, /* capabilities */ nullptr);
	}
	EFI_STATUS counterStatus = EFI_UNSUPPORTED;
	if (bootServices != nullptr && bootServices->GetNextMonotonicCount != nullptr) {
		counterStatus = bootServices->GetNextMonotonicCount(&counter);
	}
	if (timeStatus != EFI_SUCCESS && counterStatus != EFI_SUCCESS) {
		NLogging::Log(
			systemTable,
			L"[run] id=unknown build=%a time_status=%r counter_status=%r\n",
			/* buildId */ APPLE_SET_OS_BUILD_ID,
			/* timeStatus */ timeStatus,
			/* counterStatus */ counterStatus);
		return;
	}
	if (timeStatus != EFI_SUCCESS) {
		NLogging::Log(
			systemTable,
			L"[run] id=counter-%016lx build=%a time_status=%r\n",
			/* counter */ counter,
			/* buildId */ APPLE_SET_OS_BUILD_ID,
			/* timeStatus */ timeStatus);
		return;
	}
	if (counterStatus != EFI_SUCCESS) {
		NLogging::Log(
			systemTable,
			L"[run] id=time-%04d%02d%02d-%02d%02d%02d build=%a counter_status=%r\n",
			/* year */ static_cast<UINT32>(time.Year),
			/* month */ static_cast<UINT32>(time.Month),
			/* day */ static_cast<UINT32>(time.Day),
			/* hour */ static_cast<UINT32>(time.Hour),
			/* minute */ static_cast<UINT32>(time.Minute),
			/* second */ static_cast<UINT32>(time.Second),
			/* buildId */ APPLE_SET_OS_BUILD_ID,
			/* counterStatus */ counterStatus);
		return;
	}
	NLogging::Log(
		systemTable,
		L"[run] id=%04d%02d%02d-%02d%02d%02d-%016lx build=%a\n",
		/* year */ static_cast<UINT32>(time.Year),
		/* month */ static_cast<UINT32>(time.Month),
		/* day */ static_cast<UINT32>(time.Day),
		/* hour */ static_cast<UINT32>(time.Hour),
		/* minute */ static_cast<UINT32>(time.Minute),
		/* second */ static_cast<UINT32>(time.Second),
		/* counter */ counter,
		/* buildId */ APPLE_SET_OS_BUILD_ID);
}

static void LogFirmware(EFI_SYSTEM_TABLE* systemTable) {
	const CHAR16* vendor = systemTable->FirmwareVendor;
	if (vendor == nullptr) {
		vendor = AsEfiText(L"unknown");
	}
	NLogging::Log(
		systemTable,
		L"[firmware] vendor=%s revision=0x%08x uefi_revision=0x%08x\n",
		/* vendor */ vendor,
		/* firmwareRevision */ systemTable->FirmwareRevision,
		/* uefiRevision */ systemTable->Hdr.Revision);
}

void LogBootEnvironment(EFI_HANDLE image, EFI_SYSTEM_TABLE* systemTable) {
	if (!NLogging::IsAvailable(systemTable)) {
		return;
	}
	LogRunIdentity(systemTable);
	LogFirmware(systemTable);
	LogLoadedImage(image, systemTable);
}

void LogResult(EFI_SYSTEM_TABLE* systemTable, EFI_STATUS status) {
	if (status != EFI_SUCCESS) {
		NLogging::Log(systemTable, L"[entry] error=%r\n", status);
	}
}

} // namespace NDebug
