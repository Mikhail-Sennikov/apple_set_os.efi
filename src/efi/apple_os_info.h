#pragma once

extern "C" {
#include <efibind.h>
#include <efidef.h>
}

struct _EFI_SYSTEM_TABLE;
using EFI_SYSTEM_TABLE = _EFI_SYSTEM_TABLE;

namespace NEfi {

class TAppleOsInfo {
public:
	explicit TAppleOsInfo(EFI_SYSTEM_TABLE* systemTable);
	EFI_STATUS GetStatus() const;

	// Returns true if the firmware callback was invoked, not a firmware status.
	bool SetOsVendor(const char* vendor) const;
	bool SetOsVersion(const char* version) const;

private:
	struct TProtocol;
	TProtocol* Protocol_ = nullptr;
	EFI_SYSTEM_TABLE* SystemTable_;
	EFI_STATUS Status_;
};

} // namespace NEfi
