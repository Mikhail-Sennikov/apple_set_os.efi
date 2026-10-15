#pragma once

#include <logging/file_log.h>

#include <stdarg.h>

namespace NLogging::NPrivate {

class TLogger {
public:
	constexpr TLogger() = default;
	TLogger(const TLogger&) = delete;
	TLogger& operator=(const TLogger&) = delete;

	void Close() const {
		File_.Close();
		SystemTable_ = nullptr;
	}
	bool IsAvailable(EFI_SYSTEM_TABLE* systemTable) const;
	void Log(EFI_SYSTEM_TABLE* systemTable, const wchar_t* format, va_list args) const;

private:
	mutable const EFI_SYSTEM_TABLE* SystemTable_ = nullptr;
	mutable TFileLog File_;
};

const TLogger& GetLogger();

} // namespace NLogging::NPrivate
