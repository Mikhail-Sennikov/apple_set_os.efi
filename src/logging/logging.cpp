#include <logging/logging.h>

extern "C" {
#include <efi.h>
#include <efilib.h>
}

namespace NLogging {

static_assert(sizeof(wchar_t) == sizeof(CHAR16), "EFI text requires 16-bit wchar_t");
static_assert(alignof(wchar_t) == alignof(CHAR16));

void Log(EFI_SYSTEM_TABLE* systemTable, const wchar_t* format, ...) {
	if (systemTable == nullptr || systemTable->BootServices == nullptr || format == nullptr) {
		return;
	}
	const auto* consoleOut = systemTable->ConOut;
	if (consoleOut == nullptr ||
		consoleOut->OutputString == nullptr ||
		consoleOut->Mode == nullptr)
	{
		return;
	}

	static const EFI_SYSTEM_TABLE* const LOG_SYSTEM_TABLE = [systemTable] {
		// nullptr skips GNU-EFI's optional image and language discovery.
		InitializeLib(nullptr, systemTable);
		return ST;
	}();
	if (LOG_SYSTEM_TABLE != systemTable || ST != systemTable) {
		return;
	}

	va_list args;
	va_start(args, format);
	// GNU-EFI streams formatted text; no whole-message buffer is needed.
	VPrint(reinterpret_cast<const CHAR16*>(format), args);
	va_end(args);
}

} // namespace NLogging
