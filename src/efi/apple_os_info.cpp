#include <efi/apple_os_info.h>

extern "C" {
#include <efi.h>
}
#include <stddef.h>
#include <type_traits>

#if !defined(__x86_64__) || !defined(HAVE_USE_MS_ABI)
#error Apple Set OS requires GNU-EFI Microsoft x64 ABI support
#endif

static EFI_GUID APPLE_SET_OS_GUID = {
	.Data1 = 0xc5c5da95,
	.Data2 = 0x7d5c,
	.Data3 = 0x45e6,
	.Data4 = { 0xb2, 0xf1, 0x3f, 0xd5, 0x2b, 0xb1, 0x00, 0x77 }
};

// Apple OS Info uses EDK2's CHAR8 (char), not GNU-EFI's unsigned char alias.
struct TAppleOsInfo::TProtocol {
	UINTN revision;
	void (EFIAPI* setOsVersion)(char* version);
	void (EFIAPI* setOsVendor)(char* vendor);
};

TAppleOsInfo::TAppleOsInfo(EFI_SYSTEM_TABLE* systemTable) : status(EFI_INVALID_PARAMETER) {
	using TExpectedCallback = void (__attribute__((ms_abi))*)(char* value);
	static_assert(std::is_same_v<decltype(TProtocol::setOsVersion), TExpectedCallback>,
		"Apple callbacks must return void and use the Microsoft x64 ABI");
	static_assert(std::is_same_v<decltype(TProtocol::setOsVendor), TExpectedCallback>);
	static_assert(std::is_same_v<decltype(TProtocol::revision), UINTN>);
	static_assert(sizeof(UINTN) == sizeof(void*));
	static_assert(offsetof(TProtocol, revision) == 0);
	static_assert(offsetof(TProtocol, setOsVersion) == sizeof(UINTN));
	static_assert(offsetof(TProtocol, setOsVendor) == 2 * sizeof(UINTN));
	static_assert(sizeof(TProtocol) == 3 * sizeof(UINTN));

	if (systemTable == nullptr || systemTable->BootServices == nullptr) {
		return;
	}

	void* locatedProtocol = nullptr;
	status = systemTable->BootServices->LocateProtocol(
		&APPLE_SET_OS_GUID, nullptr, &locatedProtocol);
	if (status != EFI_SUCCESS) {
		return;
	}
	if (locatedProtocol == nullptr) {
		status = EFI_NOT_FOUND;
		return;
	}

	protocol = static_cast<TProtocol*>(locatedProtocol);
}

EFI_STATUS TAppleOsInfo::GetStatus() const {
	return status;
}

bool TAppleOsInfo::SetOsVendor(const char* vendor) const {
	if (protocol == nullptr ||
		protocol->revision < 2 ||
		protocol->setOsVendor == nullptr)
	{
		return false;
	}
	protocol->setOsVendor(const_cast<char*>(vendor));
	return true;
}

bool TAppleOsInfo::SetOsVersion(const char* version) const {
	if (protocol == nullptr ||
		protocol->revision == 0 ||
		protocol->setOsVersion == nullptr)
	{
		return false;
	}
	protocol->setOsVersion(const_cast<char*>(version));
	return true;
}
