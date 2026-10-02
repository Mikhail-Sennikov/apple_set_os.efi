#include "apple_set_os.h"

EFI_GUID APPLE_SET_OS_GUID = {
	0xc5c5da95, 0x7d5c, 0x45e6,
	{ 0xb2, 0xf1, 0x3f, 0xd5, 0x2b, 0xb1, 0x00, 0x77 }
};

static CHAR8 apple_set_os_vendor[] = APPLE_SET_OS_VENDOR_TEXT;
static CHAR8 apple_set_os_version[] = APPLE_SET_OS_VERSION_TEXT;

EFI_STATUS
apple_set_os_locate(
	IN EFI_BOOT_SERVICES *boot_services,
	OUT APPLE_SET_OS_INTERFACE **protocol
)
{
	EFI_STATUS status;

	if (boot_services == NULL || protocol == NULL)
		return EFI_INVALID_PARAMETER;

	*protocol = NULL;
	status = boot_services->LocateProtocol(
		&APPLE_SET_OS_GUID, NULL, (VOID **)protocol);
	if (status != EFI_SUCCESS)
		return status;
	return *protocol != NULL ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_STATUS
apple_set_os_apply(
	IN APPLE_SET_OS_INTERFACE *protocol,
	OUT APPLE_SET_OS_RESULT *result,
	IN APPLE_SET_OS_OBSERVER observer,
	IN VOID *observer_context
)
{
	EFI_STATUS status;

	if (result == NULL)
		return EFI_INVALID_PARAMETER;

	result->revision = protocol != NULL ? protocol->revision : 0;
	result->vendor_attempted = 0;
	result->version_attempted = 0;

	if (protocol == NULL)
		return EFI_INVALID_PARAMETER;

	if (protocol->revision >= 2 && protocol->set_os_vendor != NULL) {
		if (observer != NULL) {
			status = observer(observer_context, APPLE_SET_OS_BEFORE_VENDOR);
			if (status != EFI_SUCCESS)
				return status;
		}

		result->vendor_attempted = 1;
		protocol->set_os_vendor(apple_set_os_vendor);

		if (observer != NULL) {
			status = observer(observer_context, APPLE_SET_OS_AFTER_VENDOR);
			if (status != EFI_SUCCESS)
				return status;
		}
	}

	if (protocol->revision > 0 && protocol->set_os_version != NULL) {
		if (observer != NULL) {
			status = observer(observer_context, APPLE_SET_OS_BEFORE_VERSION);
			if (status != EFI_SUCCESS)
				return status;
		}

		result->version_attempted = 1;
		protocol->set_os_version(apple_set_os_version);

		if (observer != NULL) {
			status = observer(observer_context, APPLE_SET_OS_AFTER_VERSION);
			if (status != EFI_SUCCESS)
				return status;
		}
	}

	return EFI_SUCCESS;
}
