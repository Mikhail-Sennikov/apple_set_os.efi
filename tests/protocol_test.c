#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/apple_set_os.h"

#define TYPE_MATCHES(expression, type) \
	_Generic((expression), type: 1, default: 0)

typedef VOID (__attribute__((ms_abi)) *EXPECTED_VOID_CALLBACK)(CHAR8 *value);

_Static_assert(TYPE_MATCHES(
	((APPLE_SET_OS_INTERFACE *)0)->set_os_version,
	EXPECTED_VOID_CALLBACK), "OS version callback must use the void ABI");
_Static_assert(TYPE_MATCHES(
	((APPLE_SET_OS_INTERFACE *)0)->set_os_vendor,
	EXPECTED_VOID_CALLBACK), "OS vendor callback must use the void ABI");

_Static_assert(sizeof(((APPLE_SET_OS_INTERFACE *)0)->revision) == sizeof(UINTN),
	"Protocol revision must have native pointer width");
_Static_assert(offsetof(APPLE_SET_OS_INTERFACE, set_os_version) == sizeof(UINTN),
	"OS version callback must follow the revision");
_Static_assert(offsetof(APPLE_SET_OS_INTERFACE, set_os_vendor) == 2 * sizeof(UINTN),
	"OS vendor callback must follow the OS version callback");

static int calls[8];
static int call_count;
static int vendor_argument_valid;
static int version_argument_valid;
static APPLE_SET_OS_STAGE observer_fail_stage;

static int
same_ascii(const CHAR8 *left, const CHAR8 *right)
{
	while (*left != 0 && *left == *right) {
		++left;
		++right;
	}
	return *left == *right;
}

static VOID EFIAPI
mock_vendor(CHAR8 *vendor)
{
	vendor_argument_valid = same_ascii(
		vendor, (CHAR8 *)APPLE_SET_OS_VENDOR_TEXT);
	calls[call_count++] = 10;
}

static VOID EFIAPI
mock_version(CHAR8 *version)
{
	version_argument_valid = same_ascii(
		version, (CHAR8 *)APPLE_SET_OS_VERSION_TEXT);
	calls[call_count++] = 20;
}

static EFI_STATUS
mock_observer(VOID *context, APPLE_SET_OS_STAGE stage)
{
	(void)context;
	calls[call_count++] = 100 + (int)stage;
	return stage == observer_fail_stage ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

static int
run_case(UINTN revision, APPLE_SET_OS_STAGE fail_stage,
	const int *expected, int expected_count, EFI_STATUS expected_status)
{
	APPLE_SET_OS_INTERFACE protocol;
	APPLE_SET_OS_RESULT result;
	EFI_STATUS status;
	int index;

	protocol.revision = revision;
	protocol.set_os_version = mock_version;
	protocol.set_os_vendor = mock_vendor;
	observer_fail_stage = fail_stage;
	vendor_argument_valid = 0;
	version_argument_valid = 0;
	call_count = 0;

	status = apple_set_os_apply(&protocol, &result, mock_observer, NULL);
	if (status != expected_status || call_count != expected_count)
		return 1;
	for (index = 0; index < expected_count; ++index) {
		if (calls[index] != expected[index])
			return 2;
	}
	if (result.vendor_attempted != vendor_argument_valid ||
		result.version_attempted != version_argument_valid)
		return 3;
	return 0;
}

int
main(void)
{
	static const int revision_0[] = { 0 };
	static const int revision_1[] = { 102, 20, 103 };
	static const int revision_2[] = { 100, 10, 101, 102, 20, 103 };
	static const int observer_failure[] = { 100 };
	APPLE_SET_OS_INTERFACE protocol;
	APPLE_SET_OS_RESULT result;
	EFI_STATUS status;

	(void)APPLE_SET_OS_GUID;
	(void)revision_0;

	if (run_case(0, (APPLE_SET_OS_STAGE)-1,
		revision_0, 0, EFI_SUCCESS) != 0)
		return 10;
	if (run_case(1, (APPLE_SET_OS_STAGE)-1,
		revision_1, 3, EFI_SUCCESS) != 0)
		return 11;
	if (run_case(2, (APPLE_SET_OS_STAGE)-1,
		revision_2, 6, EFI_SUCCESS) != 0)
		return 12;
	if (run_case(3, (APPLE_SET_OS_STAGE)-1,
		revision_2, 6, EFI_SUCCESS) != 0)
		return 13;
	if (run_case(2, APPLE_SET_OS_BEFORE_VENDOR,
		observer_failure, 1, EFI_DEVICE_ERROR) != 0)
		return 14;

	protocol.revision = 2;
	protocol.set_os_vendor = NULL;
	protocol.set_os_version = mock_version;
	call_count = 0;
	version_argument_valid = 0;
	observer_fail_stage = (APPLE_SET_OS_STAGE)-1;
	if (apple_set_os_apply(&protocol, &result, mock_observer, NULL) !=
		EFI_SUCCESS)
		return 15;
	if (result.vendor_attempted || !result.version_attempted ||
		!version_argument_valid)
		return 16;

	if (apple_set_os_apply(NULL, &result, NULL, NULL) !=
		EFI_INVALID_PARAMETER)
		return 17;
	if (apple_set_os_apply(&protocol, NULL, NULL, NULL) !=
		EFI_INVALID_PARAMETER)
		return 18;

	protocol.revision = 0;
	protocol.set_os_vendor = mock_vendor;
	protocol.set_os_version = mock_version;
	call_count = 0;
	status = apple_set_os_apply(&protocol, &result, NULL, NULL);
	if (status != EFI_SUCCESS || call_count != 0 || result.revision != 0 ||
		result.vendor_attempted || result.version_attempted)
		return 19;

	protocol.revision = 2;
	protocol.set_os_vendor = mock_vendor;
	protocol.set_os_version = mock_version;
	call_count = 0;
	vendor_argument_valid = 0;
	version_argument_valid = 0;
	status = apple_set_os_apply(&protocol, &result, NULL, NULL);
	if (status != EFI_SUCCESS || call_count != 2 ||
		calls[0] != 10 || calls[1] != 20)
		return 20;
	if (result.revision != 2 || !result.vendor_attempted ||
		!result.version_attempted || !vendor_argument_valid ||
		!version_argument_valid)
		return 21;

	protocol.set_os_version = NULL;
	call_count = 0;
	status = apple_set_os_apply(&protocol, &result, NULL, NULL);
	if (status != EFI_SUCCESS || call_count != 1 || calls[0] != 10 ||
		!result.vendor_attempted || result.version_attempted)
		return 22;

	observer_fail_stage = APPLE_SET_OS_AFTER_VENDOR;
	protocol.set_os_version = mock_version;
	call_count = 0;
	status = apple_set_os_apply(&protocol, &result, mock_observer, NULL);
	if (status != EFI_DEVICE_ERROR || call_count != 3 ||
		calls[0] != 100 || calls[1] != 10 || calls[2] != 101)
		return 23;

	observer_fail_stage = APPLE_SET_OS_BEFORE_VERSION;
	call_count = 0;
	status = apple_set_os_apply(&protocol, &result, mock_observer, NULL);
	if (status != EFI_DEVICE_ERROR || call_count != 4 ||
		calls[3] != 102 || result.version_attempted)
		return 24;

	observer_fail_stage = APPLE_SET_OS_AFTER_VERSION;
	call_count = 0;
	status = apple_set_os_apply(&protocol, &result, mock_observer, NULL);
	if (status != EFI_DEVICE_ERROR || call_count != 6 ||
		calls[4] != 20 || calls[5] != 103 ||
		!result.version_attempted)
		return 25;

	puts("protocol tests passed");
	return 0;
}
