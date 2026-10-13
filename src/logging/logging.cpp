#include <logging/logging.h>

extern "C" {
#include <efi.h>
#include <efilib.h>
}

namespace NLogging {

static_assert(sizeof(wchar_t) == sizeof(CHAR16), "EFI text requires 16-bit wchar_t");
static_assert(alignof(wchar_t) == alignof(CHAR16));

static const EFI_SYSTEM_TABLE* LOG_SYSTEM_TABLE = nullptr;

bool IsAvailable(EFI_SYSTEM_TABLE* systemTable) {
	if (systemTable == nullptr || systemTable->BootServices == nullptr) {
		return false;
	}
	const auto* consoleOut = systemTable->ConOut;
	if (consoleOut == nullptr ||
		consoleOut->OutputString == nullptr ||
		consoleOut->Mode == nullptr)
	{
		return false;
	}
	return LOG_SYSTEM_TABLE == nullptr || (LOG_SYSTEM_TABLE == systemTable && ST == systemTable);
}

void Log(EFI_SYSTEM_TABLE* systemTable, const wchar_t* format, ...) {
	if (format == nullptr || !IsAvailable(systemTable)) {
		return;
	}

	static const EFI_SYSTEM_TABLE* const INITIALIZED_SYSTEM_TABLE = [systemTable] {
		// nullptr skips GNU-EFI's optional image and language discovery.
		InitializeLib(/* image */ nullptr, /* systemTable */ systemTable);
		LOG_SYSTEM_TABLE = ST;
		return LOG_SYSTEM_TABLE;
	}();
	if (INITIALIZED_SYSTEM_TABLE != systemTable || ST != systemTable) {
		return;
	}

	va_list args;
	va_start(/* args */ args, /* lastNamedParameter */ format);
	// GNU-EFI streams formatted text; no whole-message buffer is needed.
	VPrint(reinterpret_cast<const CHAR16*>(format), args);
	va_end(args);
}

} // namespace NLogging
