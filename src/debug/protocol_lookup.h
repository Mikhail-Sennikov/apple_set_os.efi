#pragma once

extern "C" {
#include <efi.h>
}

namespace NDebug::NPrivate {

template<typename T>
static EFI_STATUS GetProtocol(
	EFI_SYSTEM_TABLE* systemTable,
	EFI_HANDLE handle,
	EFI_GUID* guid,
	T** protocol)
{
	*protocol = nullptr;
	if (systemTable == nullptr || systemTable->BootServices == nullptr ||
		systemTable->BootServices->HandleProtocol == nullptr)
	{
		return EFI_UNSUPPORTED;
	}
	if (handle == nullptr) {
		return EFI_NOT_FOUND;
	}
	void* result = nullptr;
	const EFI_STATUS status = systemTable->BootServices->HandleProtocol(
		/* handle */ handle,
		/* protocol */ guid,
		/* interface */ &result);
	if (status != EFI_SUCCESS) {
		return status;
	}
	if (result == nullptr) {
		return EFI_NOT_FOUND;
	}
	*protocol = static_cast<T*>(result);
	return EFI_SUCCESS;
}

} // namespace NDebug::NPrivate
