#include <efi/apple_os_info.h>

extern "C" {
#include <efi.h>
}
#include <stddef.h>
#include <type_traits>

#if !defined(__x86_64__) || !defined(HAVE_USE_MS_ABI)
#error Apple Set OS requires GNU-EFI Microsoft x64 ABI support
#endif

namespace NEfi {

static EFI_GUID APPLE_SET_OS_GUID = {
	.Data1 = 0xc5c5da95,
	.Data2 = 0x7d5c,
	.Data3 = 0x45e6,
	.Data4 = { 0xb2, 0xf1, 0x3f, 0xd5, 0x2b, 0xb1, 0x00, 0x77 }
};

// Apple OS Info uses EDK2's CHAR8 (char), not GNU-EFI's unsigned char alias.
struct TAppleOsInfo::TProtocol {
	const uint64_t Revision;
	void (EFIAPI* SetOsVersion)(char* version);
	void (EFIAPI* SetOsVendor)(char* vendor);
};

TAppleOsInfo::TAppleOsInfo(EFI_SYSTEM_TABLE* systemTable) : Status_(EFI_INVALID_PARAMETER) {
	using TExpectedCallback = void (__attribute__((ms_abi))*)(char* value);
	static_assert(std::is_same_v<decltype(TProtocol::SetOsVersion), TExpectedCallback>,
		"Apple callbacks must return void and use the Microsoft x64 ABI");
	static_assert(std::is_same_v<decltype(TProtocol::SetOsVendor), TExpectedCallback>);
	static_assert(std::is_same_v<std::remove_cv_t<decltype(TProtocol::Revision)>, UINTN>);
	static_assert(sizeof(UINTN) == sizeof(void*));
	static_assert(offsetof(TProtocol, Revision) == 0);
	static_assert(offsetof(TProtocol, SetOsVersion) == sizeof(UINTN));
	static_assert(offsetof(TProtocol, SetOsVendor) == 2 * sizeof(UINTN));
	static_assert(sizeof(TProtocol) == 3 * sizeof(UINTN));

	if (systemTable == nullptr || systemTable->BootServices == nullptr) {
		return;
	}

	void* locatedProtocol = nullptr;
	Status_ = systemTable->BootServices->LocateProtocol(
		&APPLE_SET_OS_GUID, nullptr, &locatedProtocol);
	if (Status_ != EFI_SUCCESS) {
		return;
	}
	if (locatedProtocol == nullptr) {
		Status_ = EFI_NOT_FOUND;
		return;
	}

	Protocol_ = static_cast<TProtocol*>(locatedProtocol);
}

EFI_STATUS TAppleOsInfo::GetStatus() const {
	return Status_;
}

bool TAppleOsInfo::SetOsVendor(const char* vendor) const {
	if (Protocol_ == nullptr ||
		Protocol_->Revision < 2 ||
		Protocol_->SetOsVendor == nullptr)
	{
		return false;
	}
	Protocol_->SetOsVendor(const_cast<char*>(vendor));
	return true;
}

bool TAppleOsInfo::SetOsVersion(const char* version) const {
	if (Protocol_ == nullptr ||
		Protocol_->Revision == 0 ||
		Protocol_->SetOsVersion == nullptr)
	{
		return false;
	}
	Protocol_->SetOsVersion(const_cast<char*>(version));
	return true;
}

} // namespace NEfi
