#include <apple_set_os.h>
#include <efi/apple_os_info.h>

extern "C" {
#include <efi.h>
}
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <type_traits>

static_assert(!std::is_default_constructible_v<TAppleOsInfo>);
static_assert(!std::is_convertible_v<EFI_SYSTEM_TABLE*, TAppleOsInfo>);

struct TMockAppleOsInfoProtocol {
	UINTN revision;
	void (EFIAPI* setOsVersion)(char* version);
	void (EFIAPI* setOsVendor)(char* vendor);
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

static EFI_STATUS EFIAPI MockLocateProtocol(EFI_GUID* guid,
	void* registration, void** protocol) {
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

static EFI_STATUS EFIAPI MockOutputString(SIMPLE_TEXT_OUTPUT_INTERFACE* conOut,
	CHAR16* text) {
	OUTPUT_ARGUMENTS_VALID = OUTPUT_ARGUMENTS_VALID &&
		conOut == EXPECTED_CON_OUT && text != nullptr;
	++OUTPUT_COUNT;
	return EFI_SUCCESS;
}

static bool RunCase(const char* name, TMockAppleOsInfoProtocol* protocol,
	EFI_STATUS locateResult, EFI_STATUS expectedStatus,
	const ECall* expectedCalls, size_t expectedCount, bool useConsole = false) {
	EFI_BOOT_SERVICES bootServices = {};
	EFI_SYSTEM_TABLE systemTable = {};
	SIMPLE_TEXT_OUTPUT_INTERFACE conOut = {};
	bootServices.LocateProtocol = MockLocateProtocol;
	systemTable.BootServices = &bootServices;
	conOut.OutputString = MockOutputString;
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

	EFI_STATUS status = apple_set_os_run(nullptr, &systemTable);
	if (status != expectedStatus || CALL_COUNT != expectedCount ||
		!LOCATE_ARGUMENTS_VALID || !VENDOR_ARGUMENT_VALID || !VERSION_ARGUMENT_VALID ||
		!OUTPUT_ARGUMENTS_VALID || (useConsole && OUTPUT_COUNT != 3))
	{
		fprintf(stderr, "%s: status, arguments, or call count mismatch\n", name);
		return false;
	}
	for (size_t index = 0; index < expectedCount; ++index) {
		if (CALLS[index] != expectedCalls[index]) {
			fprintf(stderr, "%s: callback order mismatch\n", name);
			return false;
		}
	}
	return true;
}

static bool CheckInactive(const char* name, const TAppleOsInfo& appleOsInfo) {
	size_t previousCallCount = CALL_COUNT;
	if (appleOsInfo.SetOsVendor("Apple Inc.") ||
		appleOsInfo.SetOsVersion("Mac OS X 10.9") ||
		CALL_COUNT != previousCallCount)
	{
		fprintf(stderr, "%s: inactive wrapper must not invoke callbacks\n", name);
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
	TAppleOsInfo appleOsInfo(&systemTable);
	if (appleOsInfo.GetStatus() != EFI_SUCCESS ||
		!appleOsInfo.SetOsVendor("Apple Inc.") ||
		!appleOsInfo.SetOsVersion("Mac OS X 10.9") ||
		!LOCATE_ARGUMENTS_VALID || !VENDOR_ARGUMENT_VALID || !VERSION_ARGUMENT_VALID ||
		CALL_COUNT != 3 || CALLS[0] != ECall::Locate ||
		CALLS[1] != ECall::Vendor || CALLS[2] != ECall::Version)
	{
		fputs("initialized wrapper must invoke vendor and version callbacks\n", stderr);
		return false;
	}

	struct TConstructorCase {
		const char* name;
		EFI_SYSTEM_TABLE* systemTable;
		TMockAppleOsInfoProtocol* protocol;
		EFI_STATUS locateStatus;
		EFI_STATUS expectedStatus;
		size_t expectedCallCount;
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
		LOCATED_PROTOCOL = testCase.protocol;
		LOCATE_STATUS = testCase.locateStatus;
		LOCATE_ARGUMENTS_VALID = false;
		CALL_COUNT = 0;
		TAppleOsInfo failedAppleOsInfo(testCase.systemTable);
		if (failedAppleOsInfo.GetStatus() != testCase.expectedStatus ||
			CALL_COUNT != testCase.expectedCallCount ||
			(testCase.expectedCallCount != 0 && !LOCATE_ARGUMENTS_VALID))
		{
			fprintf(stderr, "%s: status or locate call mismatch\n", testCase.name);
			return false;
		}
		if (!CheckInactive(testCase.name, failedAppleOsInfo)) {
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

	if (!RunCase("revision 0", &protocol, EFI_SUCCESS, EFI_SUCCESS,
		LOCATE_ONLY, 1))
	{
		return 1;
	}
	protocol.revision = 1;
	if (!RunCase("revision 1", &protocol, EFI_SUCCESS, EFI_SUCCESS,
		VERSION_ONLY, 2))
	{
		return 1;
	}
	protocol.revision = 2;
	if (!RunCase("revision 2", &protocol, EFI_SUCCESS, EFI_SUCCESS,
		VENDOR_THEN_VERSION, 3))
	{
		return 1;
	}
	protocol.revision = 3;
	if (!RunCase("revision 3", &protocol, EFI_SUCCESS, EFI_SUCCESS,
		VENDOR_THEN_VERSION, 3))
	{
		return 1;
	}

	protocol.setOsVendor = nullptr;
	if (!RunCase("missing vendor callback", &protocol, EFI_SUCCESS, EFI_SUCCESS,
		VERSION_ONLY, 2))
	{
		return 1;
	}
	protocol.setOsVendor = MockVendor;
	protocol.setOsVersion = nullptr;
	if (!RunCase("missing version callback", &protocol, EFI_SUCCESS, EFI_SUCCESS,
		VENDOR_ONLY, 2))
	{
		return 1;
	}
	protocol.setOsVendor = nullptr;
	if (!RunCase("missing both callbacks", &protocol, EFI_SUCCESS, EFI_SUCCESS,
		LOCATE_ONLY, 1))
	{
		return 1;
	}

	protocol.setOsVendor = MockVendor;
	protocol.setOsVersion = MockVersion;
	if (!RunCase("success with null protocol", nullptr, EFI_SUCCESS,
		EFI_NOT_FOUND, LOCATE_ONLY, 1))
	{
		return 1;
	}
	if (!RunCase("protocol not found", nullptr, EFI_NOT_FOUND,
		EFI_NOT_FOUND, LOCATE_ONLY, 1))
	{
		return 1;
	}
	if (!RunCase("failed locate with spurious protocol", &protocol,
		EFI_DEVICE_ERROR, EFI_DEVICE_ERROR, LOCATE_ONLY, 1))
	{
		return 1;
	}
	if (!RunCase("preserve locate failure", nullptr, EFI_ACCESS_DENIED,
		EFI_ACCESS_DENIED, LOCATE_ONLY, 1))
	{
		return 1;
	}
	if (!RunCase("console output", &protocol, EFI_SUCCESS, EFI_SUCCESS,
		VENDOR_THEN_VERSION, 3, true))
	{
		return 1;
	}

	CALL_COUNT = 0;
	EFI_SYSTEM_TABLE systemTable = {};
	if (apple_set_os_run(nullptr, nullptr) != EFI_INVALID_PARAMETER ||
		apple_set_os_run(nullptr, &systemTable) != EFI_INVALID_PARAMETER ||
		CALL_COUNT != 0)
	{
		fputs("invalid system table must not access boot services\n", stderr);
		return 1;
	}

	if (!RunWrapperCases()) {
		return 1;
	}

	puts("protocol tests passed");
	return 0;
}
