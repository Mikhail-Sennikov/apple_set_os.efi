#include <logging/logger.h>
#include <logging/logging.h>

extern "C" {
#include <efi.h>
#include <efilib.h>

// GNU-EFI's _entry saves our image handle before calling efi_main.
extern EFI_HANDLE LibImageHandle;
}

namespace NLogging::NPrivate {

static_assert(sizeof(wchar_t) == sizeof(CHAR16), "EFI text requires 16-bit wchar_t");
static_assert(alignof(wchar_t) == alignof(CHAR16));

// Namespace scope lets GCC put cleanup directly into GNU-EFI's fini array.
static const TLogger LOGGER;

bool TLogger::IsAvailable(EFI_SYSTEM_TABLE* systemTable) const {
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
	return SystemTable_ == nullptr || (SystemTable_ == systemTable && ST == systemTable);
}

void TLogger::Log(EFI_SYSTEM_TABLE* systemTable, const wchar_t* format, va_list args) const {
	if (format == nullptr || *format == 0 || !IsAvailable(systemTable)) {
		return;
	}

	if (SystemTable_ == nullptr) {
		// The EFI entry already initialized GNU-EFI with our image handle.
		// nullptr also allows console-only use outside that entry, e.g. in tests.
		InitializeLib(/* image */ nullptr, /* systemTable */ systemTable);
		if (ST != systemTable) {
			return;
		}
		// Bind first: opening failures may themselves write a console message.
		SystemTable_ = systemTable;
		File_.Open(LibImageHandle, systemTable);
	}

	// GNU-EFI's console and buffer formatters each copy args internally.
	const UINTN length = VPrint(reinterpret_cast<const CHAR16*>(format), args);
	File_.Write(systemTable, format, args, length);
}

const TLogger& GetLogger() {
	return LOGGER;
}

} // namespace NLogging::NPrivate

namespace NLogging {

bool IsAvailable(EFI_SYSTEM_TABLE* systemTable) {
	return NPrivate::GetLogger().IsAvailable(systemTable);
}

void Log(EFI_SYSTEM_TABLE* systemTable, const wchar_t* format, ...) {
	va_list args;
	va_start(/* args */ args, /* lastNamedParameter */ format);
	NPrivate::GetLogger().Log(systemTable, format, args);
	va_end(args);
}

} // namespace NLogging
