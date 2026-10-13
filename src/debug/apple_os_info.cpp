#include <debug/apple_os_info.h>
#include <logging/logging.h>

extern "C" {
#include <efi.h>
}

namespace NDebug {

static const CHAR16* EfiText(const wchar_t* text) {
	return reinterpret_cast<const CHAR16*>(text);
}

static const wchar_t* GetCallbackAvailability(bool protocolReady, bool revisionSupported, bool callbackPresent) {
	if (!protocolReady) {
		return L"unavailable";
	}
	if (!revisionSupported) {
		return L"unsupported_revision";
	}
	if (!callbackPresent) {
		return L"null_callback";
	}
	return L"available";
}

void LogProtocolLookup(
	EFI_SYSTEM_TABLE* systemTable,
	EFI_STATUS lookupStatus,
	EFI_STATUS resultStatus,
	bool pointerPresent,
	uint64_t revision,
	bool hasVersionCallback,
	bool hasVendorCallback)
{
	const bool protocolReady = resultStatus == EFI_SUCCESS;
	const wchar_t* versionAvailability = GetCallbackAvailability(
		protocolReady,
		/* revisionSupported */ revision > 0,
		hasVersionCallback);
	const wchar_t* vendorAvailability = GetCallbackAvailability(
		protocolReady,
		/* revisionSupported */ revision >= 2,
		hasVendorCallback);
	const wchar_t* protocolPresence = pointerPresent ? L"present" : L"null";

	NLogging::Log(
		systemTable,
		L"[protocol] lookup=%r result=%r pointer=%s revision=%lu "
			L"vendor=%s version=%s\n",
		/* lookupStatus */ lookupStatus,
		/* resultStatus */ resultStatus,
		/* protocolPresence */ EfiText(protocolPresence),
		/* revision */ static_cast<UINT64>(revision),
		/* vendorAvailability */ EfiText(vendorAvailability),
		/* versionAvailability */ EfiText(versionAvailability));
}

} // namespace NDebug
