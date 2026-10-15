#pragma once

struct _EFI_SYSTEM_TABLE;
using EFI_SYSTEM_TABLE = _EFI_SYSTEM_TABLE;

namespace NLogging {

// Checks the console and any existing logging session without initializing GNU-EFI.
bool IsAvailable(EFI_SYSTEM_TABLE* systemTable);

// The first nonempty message initializes console and optional file logging.
void Log(EFI_SYSTEM_TABLE* systemTable, const wchar_t* format, ...);

} // namespace NLogging
