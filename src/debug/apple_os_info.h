#pragma once

extern "C" {
#include <efibind.h>
#include <efidef.h>
}

struct _EFI_SYSTEM_TABLE;
using EFI_SYSTEM_TABLE = _EFI_SYSTEM_TABLE;

namespace NDebug {

void LogProtocolLookup(
	EFI_SYSTEM_TABLE* systemTable,
	EFI_STATUS lookupStatus,
	EFI_STATUS resultStatus,
	bool pointerPresent,
	uint64_t revision,
	bool hasVersionCallback,
	bool hasVendorCallback);

} // namespace NDebug
