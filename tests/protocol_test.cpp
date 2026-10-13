#include <apple_set_os.h>
#include <efi/apple_os_info.h>

extern "C" {
#include <efi.h>
}
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <type_traits>

static_assert(!std::is_default_constructible_v<NEfi::TAppleOsInfo>);
static_assert(!std::is_convertible_v<EFI_SYSTEM_TABLE*, NEfi::TAppleOsInfo>);

struct TMockAppleOsInfoProtocol {
	UINTN Revision;
	void (EFIAPI* SetOsVersion)(char* version);
	void (EFIAPI* SetOsVendor)(char* vendor);
};

static constexpr EFI_GUID EXPECTED_APPLE_SET_OS_GUID = {
	.Data1 = 0xc5c5da95,
	.Data2 = 0x7d5c,
	.Data3 = 0x45e6,
	.Data4 = { 0xb2, 0xf1, 0x3f, 0xd5, 0x2b, 0xb1, 0x00, 0x77 }
};

enum class ECall {
	Locate,
	Vendor,
	Version
};

static TMockAppleOsInfoProtocol* LOCATED_PROTOCOL;
static EFI_STATUS LOCATE_STATUS;
static bool LOCATE_ARGUMENTS_VALID;
static bool VENDOR_ARGUMENT_VALID;
static bool VERSION_ARGUMENT_VALID;
static bool OUTPUT_ARGUMENTS_VALID;
static SIMPLE_TEXT_OUTPUT_INTERFACE* EXPECTED_CON_OUT;
static ECall CALLS[8];
static size_t CALL_COUNT;
static size_t OUTPUT_COUNT;
static size_t DIAGNOSTIC_CALL_COUNT;
static CHAR16 OUTPUT_LOG[8192];
static size_t OUTPUT_LENGTH;
static EFI_STATUS OUTPUT_STATUS;

static bool Check(bool condition, const char* name, const char* message) {
	if (!condition) {
		fprintf(stderr, "%s: %s\n", name, message);
	}
	return condition;
}

static size_t CountOccurrences(const wchar_t* needle) {
	size_t length = 0;
	size_t matches = 0;
	while (needle[length] != 0) {
		++length;
	}
	for (size_t index = 0; index + length <= OUTPUT_LENGTH; ++index) {
		if (memcmp(OUTPUT_LOG + index, needle, length * sizeof(CHAR16)) == 0) {
			++matches;
		}
	}
	return matches;
}

static EFI_STATUS EFIAPI MockHandleProtocol(EFI_HANDLE, EFI_GUID*, void** protocol) {
	++DIAGNOSTIC_CALL_COUNT;
	*protocol = nullptr;
	return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI MockLocateBuffer(
	EFI_LOCATE_SEARCH_TYPE,
	EFI_GUID*,
	void*,
	UINTN* count,
	EFI_HANDLE** handles)
{
	++DIAGNOSTIC_CALL_COUNT;
	*count = 0;
	*handles = nullptr;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockFreePool(void* buffer) {
	++DIAGNOSTIC_CALL_COUNT;
	free(buffer);
	return EFI_SUCCESS;
}

static void RecordCall(ECall call) {
	if (CALL_COUNT < sizeof(CALLS) / sizeof(CALLS[0])) {
		CALLS[CALL_COUNT] = call;
	}
	++CALL_COUNT;
}

static bool SameAscii(const char* left, const char* right) {
	while (*left != 0 && *left == *right) {
		++left;
		++right;
	}
	return *left == *right;
}

static EFI_STATUS EFIAPI MockLocateProtocol(EFI_GUID* guid, void* registration, void** protocol) {
	LOCATE_ARGUMENTS_VALID = guid != nullptr &&
		memcmp(guid, &EXPECTED_APPLE_SET_OS_GUID, sizeof(*guid)) == 0 &&
		registration == nullptr && protocol != nullptr;
	RecordCall(ECall::Locate);
	if (protocol != nullptr) {
		*protocol = LOCATED_PROTOCOL;
	}
	return LOCATE_STATUS;
}

static void EFIAPI MockVendor(char* vendor) {
	VENDOR_ARGUMENT_VALID = vendor != nullptr && SameAscii(vendor, "Apple Inc.");
	RecordCall(ECall::Vendor);
}

static void EFIAPI MockVersion(char* version) {
	VERSION_ARGUMENT_VALID = version != nullptr && SameAscii(version, "Mac OS X 10.9");
	RecordCall(ECall::Version);
}

static EFI_STATUS EFIAPI MockOutputString(SIMPLE_TEXT_OUTPUT_INTERFACE* conOut, CHAR16* text) {
	OUTPUT_ARGUMENTS_VALID = OUTPUT_ARGUMENTS_VALID &&
		conOut == EXPECTED_CON_OUT && text != nullptr;
	++OUTPUT_COUNT;
	if (text != nullptr) {
		for (size_t index = 0; text[index] != 0; ++index) {
			if (OUTPUT_LENGTH >= sizeof(OUTPUT_LOG) / sizeof(OUTPUT_LOG[0])) {
				OUTPUT_ARGUMENTS_VALID = false;
				return EFI_VOLUME_FULL;
			}
			OUTPUT_LOG[OUTPUT_LENGTH++] = text[index];
		}
	}
	return OUTPUT_STATUS;
}

static bool RunCase(
	const char* name,
	TMockAppleOsInfoProtocol* protocol,
	EFI_STATUS locateResult,
	EFI_STATUS expectedStatus,
	const ECall* expectedCalls,
	size_t expectedCount,
	bool useConsole = false,
	EFI_STATUS outputStatus = EFI_SUCCESS)
{
	// GNU-EFI caches these pointers; firmware tables outlive every image call.
	static EFI_BOOT_SERVICES bootServices;
	static EFI_SYSTEM_TABLE systemTable;
	static SIMPLE_TEXT_OUTPUT_INTERFACE conOut;
	static SIMPLE_TEXT_OUTPUT_MODE conOutMode;
	bootServices = {};
	systemTable = {};
	conOut = {};
	conOutMode = {};
	conOutMode.Attribute = EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK);
	bootServices.LocateProtocol = MockLocateProtocol;
	bootServices.HandleProtocol = MockHandleProtocol;
	bootServices.LocateHandleBuffer = MockLocateBuffer;
	bootServices.FreePool = MockFreePool;
	systemTable.BootServices = &bootServices;
	conOut.OutputString = MockOutputString;
	conOut.Mode = &conOutMode;
	if (useConsole) {
		systemTable.ConOut = &conOut;
	}
	LOCATED_PROTOCOL = protocol;
	LOCATE_STATUS = locateResult;
	LOCATE_ARGUMENTS_VALID = false;
	VENDOR_ARGUMENT_VALID = true;
	VERSION_ARGUMENT_VALID = true;
	OUTPUT_ARGUMENTS_VALID = true;
	EXPECTED_CON_OUT = &conOut;
	CALL_COUNT = 0;
	OUTPUT_COUNT = 0;
	DIAGNOSTIC_CALL_COUNT = 0;
	OUTPUT_LENGTH = 0;
	OUTPUT_STATUS = outputStatus;

	const EFI_STATUS status = RunAppleSetOs(/* image */ nullptr, /* systemTable */ &systemTable);
	if (!Check(status == expectedStatus, name, "entry status mismatch")) {
		return false;
	}
	if (!Check(CALL_COUNT == expectedCount, name, "callback count mismatch")) {
		return false;
	}
	if (!Check(LOCATE_ARGUMENTS_VALID, name, "invalid LocateProtocol arguments")) {
		return false;
	}
	if (!Check(VENDOR_ARGUMENT_VALID, name, "invalid vendor argument")) {
		return false;
	}
	if (!Check(VERSION_ARGUMENT_VALID, name, "invalid version argument")) {
		return false;
	}
	if (!Check(OUTPUT_ARGUMENTS_VALID, name, "invalid console arguments")) {
		return false;
	}
	if (!Check(useConsole || OUTPUT_COUNT == 0, name, "unexpected console output")) {
		return false;
	}
	if (!Check(useConsole || DIAGNOSTIC_CALL_COUNT == 0, name, "diagnostics accessed firmware without a console")) {
		return false;
	}
	for (size_t index = 0; index < expectedCount; ++index) {
		if (CALLS[index] != expectedCalls[index]) {
			fprintf(stderr, "%s: callback order mismatch\n", name);
			return false;
		}
	}
	if (useConsole) {
		size_t vendorCount = 0;
		size_t versionCount = 0;
		for (size_t index = 0; index < expectedCount; ++index) {
			vendorCount += expectedCalls[index] == ECall::Vendor;
			versionCount += expectedCalls[index] == ECall::Version;
		}
		const bool vendorLogMatches =
			CountOccurrences(L"[vendor] call argument=\"Apple Inc.\"") == vendorCount &&
			CountOccurrences(L"[vendor] success") == vendorCount;
		const bool versionLogMatches =
			CountOccurrences(L"[version] call argument=\"Mac OS X 10.9\"") == versionCount &&
			CountOccurrences(L"[version] success") == versionCount;
		const bool gpuLogMatches = CountOccurrences(L"[gpu.before]") == 1 &&
			CountOccurrences(L"[gpu.after]") == 1;
		const bool legacyOutputAbsent = CountOccurrences(L"apple_set_os started") == 0 &&
			CountOccurrences(L"Set os vendor to") == 0;
		if (!Check(CountOccurrences(L"[protocol]") == 1, name, "protocol log count mismatch")) {
			return false;
		}
		if (!Check(vendorLogMatches, name, "vendor call/success log count mismatch")) {
			return false;
		}
		if (!Check(versionLogMatches, name, "version call/success log count mismatch")) {
			return false;
		}
		if (!Check(gpuLogMatches, name, "GPU before/after log count mismatch")) {
			return false;
		}
		const size_t expectedErrorCount = expectedStatus != EFI_SUCCESS;
		if (!Check(
			CountOccurrences(L"[entry] error=") == expectedErrorCount,
			name, "entry error log count mismatch"))
		{
			return false;
		}
		if (!Check(CountOccurrences(L"\r\r\n") == 0, name, "duplicated carriage return")) {
			return false;
		}
		if (!Check(CountOccurrences(L"[guard]") == 0, name, "unexpected guard log")) {
			return false;
		}
		if (!Check(legacyOutputAbsent, name, "unexpected legacy console output")) {
			return false;
		}
	}
	return true;
}

static bool CheckInactive(const char* name, const NEfi::TAppleOsInfo& appleOsInfo) {
	const size_t previousCallCount = CALL_COUNT;
	if (!Check(!appleOsInfo.SetOsVendor("Apple Inc."), name, "inactive vendor call accepted")) {
		return false;
	}
	if (!Check(!appleOsInfo.SetOsVersion("Mac OS X 10.9"), name, "inactive version call accepted")) {
		return false;
	}
	if (!Check(CALL_COUNT == previousCallCount, name, "inactive wrapper invoked a callback")) {
		return false;
	}
	return true;
}

static bool RunWrapperCases() {
	TMockAppleOsInfoProtocol protocol = { 2, MockVersion, MockVendor };
	EFI_BOOT_SERVICES bootServices = {};
	EFI_SYSTEM_TABLE systemTable = {};
	EFI_SYSTEM_TABLE invalidSystemTable = {};
	bootServices.LocateProtocol = MockLocateProtocol;
	systemTable.BootServices = &bootServices;
	CALL_COUNT = 0;
	LOCATED_PROTOCOL = &protocol;
	LOCATE_STATUS = EFI_SUCCESS;
	LOCATE_ARGUMENTS_VALID = false;
	VENDOR_ARGUMENT_VALID = false;
	VERSION_ARGUMENT_VALID = false;
	NEfi::TAppleOsInfo appleOsInfo(&systemTable);
	const char* name = "initialized wrapper";
	if (!Check(appleOsInfo.GetStatus() == EFI_SUCCESS, name, "constructor failed")) {
		return false;
	}
	if (!Check(appleOsInfo.SetOsVendor("Apple Inc."), name, "vendor callback not invoked")) {
		return false;
	}
	if (!Check(appleOsInfo.SetOsVersion("Mac OS X 10.9"), name, "version callback not invoked")) {
		return false;
	}
	const bool argumentsValid = LOCATE_ARGUMENTS_VALID &&
		VENDOR_ARGUMENT_VALID && VERSION_ARGUMENT_VALID;
	if (!Check(argumentsValid, name, "callback argument mismatch")) {
		return false;
	}
	const bool callOrderMatches = CALL_COUNT == 3 && CALLS[0] == ECall::Locate &&
		CALLS[1] == ECall::Vendor && CALLS[2] == ECall::Version;
	if (!Check(callOrderMatches, name, "expected LocateProtocol, vendor, version order")) {
		return false;
	}

	struct TConstructorCase {
		const char* Name;
		EFI_SYSTEM_TABLE* SystemTable;
		TMockAppleOsInfoProtocol* Protocol;
		EFI_STATUS LocateStatus;
		EFI_STATUS ExpectedStatus;
		size_t ExpectedCallCount;
	};
	const TConstructorCase cases[] = {
		{ "failed constructor with spurious protocol", &systemTable, &protocol,
			EFI_DEVICE_ERROR, EFI_DEVICE_ERROR, 1 },
		{ "constructor success with null protocol", &systemTable, nullptr,
			EFI_SUCCESS, EFI_NOT_FOUND, 1 },
		{ "constructor with null system table", nullptr, &protocol,
			EFI_SUCCESS, EFI_INVALID_PARAMETER, 0 },
		{ "constructor without boot services", &invalidSystemTable, &protocol,
			EFI_SUCCESS, EFI_INVALID_PARAMETER, 0 }
	};
	for (const TConstructorCase& testCase : cases) {
		LOCATED_PROTOCOL = testCase.Protocol;
		LOCATE_STATUS = testCase.LocateStatus;
		LOCATE_ARGUMENTS_VALID = false;
		CALL_COUNT = 0;
		NEfi::TAppleOsInfo failedAppleOsInfo(testCase.SystemTable);
		if (!Check(
			failedAppleOsInfo.GetStatus() == testCase.ExpectedStatus,
			testCase.Name, "constructor status mismatch"))
		{
			return false;
		}
		if (!Check(
			CALL_COUNT == testCase.ExpectedCallCount,
			testCase.Name, "LocateProtocol call count mismatch"))
		{
			return false;
		}
		const bool locateArgumentsValid = testCase.ExpectedCallCount == 0 ||
			LOCATE_ARGUMENTS_VALID;
		if (!Check(locateArgumentsValid, testCase.Name, "invalid LocateProtocol arguments")) {
			return false;
		}
		if (!CheckInactive(testCase.Name, failedAppleOsInfo)) {
			return false;
		}
	}
	return true;
}

int main() {
	static constexpr ECall LOCATE_ONLY[] = { ECall::Locate };
	static constexpr ECall VERSION_ONLY[] = { ECall::Locate, ECall::Version };
	static constexpr ECall VENDOR_ONLY[] = { ECall::Locate, ECall::Vendor };
	static constexpr ECall VENDOR_THEN_VERSION[] = {
		ECall::Locate, ECall::Vendor, ECall::Version
	};
	TMockAppleOsInfoProtocol protocol = { 0, MockVersion, MockVendor };

	const bool initialCallWithoutConsolePassed = RunCase(
		/* name */ "initial call without console",
		/* protocol */ &protocol,
		/* locateResult */ EFI_SUCCESS,
		/* expectedStatus */ EFI_SUCCESS,
		/* expectedCalls */ LOCATE_ONLY,
		/* expectedCount */ 1);
	if (!initialCallWithoutConsolePassed) {
		return 1;
	}
	const bool revisionZeroPassed = RunCase(
		/* name */ "revision 0",
		/* protocol */ &protocol,
		/* locateResult */ EFI_SUCCESS,
		/* expectedStatus */ EFI_SUCCESS,
		/* expectedCalls */ LOCATE_ONLY,
		/* expectedCount */ 1,
		/* useConsole */ true);
	if (!revisionZeroPassed) {
		return 1;
	}
	protocol.Revision = 1;
	const bool revisionOnePassed = RunCase(
		/* name */ "revision 1",
		/* protocol */ &protocol,
		/* locateResult */ EFI_SUCCESS,
		/* expectedStatus */ EFI_SUCCESS,
		/* expectedCalls */ VERSION_ONLY,
		/* expectedCount */ 2,
		/* useConsole */ true);
	if (!revisionOnePassed) {
		return 1;
	}
	protocol.Revision = 2;
	const bool revisionTwoPassed = RunCase(
		/* name */ "revision 2",
		/* protocol */ &protocol,
		/* locateResult */ EFI_SUCCESS,
		/* expectedStatus */ EFI_SUCCESS,
		/* expectedCalls */ VENDOR_THEN_VERSION,
		/* expectedCount */ 3,
		/* useConsole */ true);
	if (!revisionTwoPassed) {
		return 1;
	}
	protocol.Revision = 3;
	const bool revisionThreePassed = RunCase(
		/* name */ "revision 3",
		/* protocol */ &protocol,
		/* locateResult */ EFI_SUCCESS,
		/* expectedStatus */ EFI_SUCCESS,
		/* expectedCalls */ VENDOR_THEN_VERSION,
		/* expectedCount */ 3);
	if (!revisionThreePassed) {
		return 1;
	}

	protocol.SetOsVendor = nullptr;
	const bool missingVendorCallbackPassed = RunCase(
		/* name */ "missing vendor callback",
		/* protocol */ &protocol,
		/* locateResult */ EFI_SUCCESS,
		/* expectedStatus */ EFI_SUCCESS,
		/* expectedCalls */ VERSION_ONLY,
		/* expectedCount */ 2);
	if (!missingVendorCallbackPassed) {
		return 1;
	}
	protocol.SetOsVendor = MockVendor;
	protocol.SetOsVersion = nullptr;
	const bool missingVersionCallbackPassed = RunCase(
		/* name */ "missing version callback",
		/* protocol */ &protocol,
		/* locateResult */ EFI_SUCCESS,
		/* expectedStatus */ EFI_SUCCESS,
		/* expectedCalls */ VENDOR_ONLY,
		/* expectedCount */ 2);
	if (!missingVersionCallbackPassed) {
		return 1;
	}
	protocol.SetOsVendor = nullptr;
	const bool missingCallbacksPassed = RunCase(
		/* name */ "missing both callbacks",
		/* protocol */ &protocol,
		/* locateResult */ EFI_SUCCESS,
		/* expectedStatus */ EFI_SUCCESS,
		/* expectedCalls */ LOCATE_ONLY,
		/* expectedCount */ 1);
	if (!missingCallbacksPassed) {
		return 1;
	}

	protocol.SetOsVendor = MockVendor;
	protocol.SetOsVersion = MockVersion;
	const bool nullProtocolPassed = RunCase(
		/* name */ "success with null protocol",
		/* protocol */ nullptr,
		/* locateResult */ EFI_SUCCESS,
		/* expectedStatus */ EFI_NOT_FOUND,
		/* expectedCalls */ LOCATE_ONLY,
		/* expectedCount */ 1,
		/* useConsole */ true);
	if (!nullProtocolPassed) {
		return 1;
	}
	const bool missingProtocolPassed = RunCase(
		/* name */ "protocol not found",
		/* protocol */ nullptr,
		/* locateResult */ EFI_NOT_FOUND,
		/* expectedStatus */ EFI_NOT_FOUND,
		/* expectedCalls */ LOCATE_ONLY,
		/* expectedCount */ 1);
	if (!missingProtocolPassed) {
		return 1;
	}
	const bool spuriousProtocolPassed = RunCase(
		/* name */ "failed locate with spurious protocol",
		/* protocol */ &protocol,
		/* locateResult */ EFI_DEVICE_ERROR,
		/* expectedStatus */ EFI_DEVICE_ERROR,
		/* expectedCalls */ LOCATE_ONLY,
		/* expectedCount */ 1,
		/* useConsole */ true);
	if (!spuriousProtocolPassed) {
		return 1;
	}
	const bool locateFailurePassed = RunCase(
		/* name */ "preserve locate failure",
		/* protocol */ nullptr,
		/* locateResult */ EFI_ACCESS_DENIED,
		/* expectedStatus */ EFI_ACCESS_DENIED,
		/* expectedCalls */ LOCATE_ONLY,
		/* expectedCount */ 1);
	if (!locateFailurePassed) {
		return 1;
	}
	const bool consoleOutputPassed = RunCase(
		/* name */ "console output",
		/* protocol */ &protocol,
		/* locateResult */ EFI_SUCCESS,
		/* expectedStatus */ EFI_SUCCESS,
		/* expectedCalls */ VENDOR_THEN_VERSION,
		/* expectedCount */ 3,
		/* useConsole */ true);
	if (!consoleOutputPassed) {
		return 1;
	}
	const bool consoleFailurePassed = RunCase(
		/* name */ "console failure does not alter protocol calls",
		/* protocol */ &protocol,
		/* locateResult */ EFI_SUCCESS,
		/* expectedStatus */ EFI_SUCCESS,
		/* expectedCalls */ VENDOR_THEN_VERSION,
		/* expectedCount */ 3,
		/* useConsole */ true,
		/* outputStatus */ EFI_DEVICE_ERROR);
	if (!consoleFailurePassed) {
		return 1;
	}

	CALL_COUNT = 0;
	EFI_SYSTEM_TABLE systemTable = {};
	const EFI_STATUS nullSystemTableStatus =
		RunAppleSetOs(/* image */ nullptr, /* systemTable */ nullptr);
	if (!Check(
		nullSystemTableStatus == EFI_INVALID_PARAMETER,
		"null system table", "expected EFI_INVALID_PARAMETER"))
	{
		return 1;
	}
	const EFI_STATUS missingBootServicesStatus =
		RunAppleSetOs(/* image */ nullptr, /* systemTable */ &systemTable);
	if (!Check(
		missingBootServicesStatus == EFI_INVALID_PARAMETER,
		"no boot services", "expected EFI_INVALID_PARAMETER"))
	{
		return 1;
	}
	if (!Check(CALL_COUNT == 0, "invalid system tables", "unexpected boot services call")) {
		return 1;
	}

	if (!RunWrapperCases()) {
		return 1;
	}

	puts("protocol tests passed");
	return 0;
}
