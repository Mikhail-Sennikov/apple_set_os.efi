#include <logging/file_log.h>
#include <logging/logging.h>

extern "C" {
#include <efi.h>
#include <efilib.h>
}

namespace NLogging::NPrivate {

static EFI_GUID LOG_PATH_VARIABLE_GUID = {
	.Data1 = 0x80f05b32,
	.Data2 = 0x6ba8,
	.Data3 = 0x4c7e,
	.Data4 = { 0xba, 0x1b, 0x40, 0x23, 0x57, 0xff, 0x91, 0xd8 }
};
static constexpr wchar_t LOG_PATH_VARIABLE_NAME[] = L"AppleSetOsLogPath";
static constexpr CHAR16 UTF16_BOM = 0xfeff;
// UEFI reserves this position value for seeking to the end of a file.
static constexpr uint64_t END_OF_FILE_POSITION = ~uint64_t{0};

static EFI_STATUS ReadLogPath(EFI_SYSTEM_TABLE* systemTable, CHAR16** path) {
	*path = nullptr;
	const auto* runtime = systemTable->RuntimeServices;
	const auto* services = systemTable->BootServices;
	if (runtime == nullptr || runtime->GetVariable == nullptr ||
		services->AllocatePool == nullptr || services->FreePool == nullptr)
	{
		return EFI_NOT_FOUND;
	}
	auto* variableName = const_cast<CHAR16*>(reinterpret_cast<const CHAR16*>(LOG_PATH_VARIABLE_NAME));
	UINTN size = 0;
	EFI_STATUS status = runtime->GetVariable(
		variableName, &LOG_PATH_VARIABLE_GUID, nullptr, &size, nullptr);
	if (status != EFI_BUFFER_TOO_SMALL) {
		return status == EFI_SUCCESS ? EFI_INVALID_PARAMETER : status;
	}
	if (size < 2 * sizeof(CHAR16) || size % sizeof(CHAR16) != 0) {
		return EFI_INVALID_PARAMETER;
	}
	const UINTN capacity = size;
	void* buffer = nullptr;
	status = services->AllocatePool(EfiLoaderData, size, &buffer);
	if (status != EFI_SUCCESS || buffer == nullptr) {
		return EFI_OUT_OF_RESOURCES;
	}
	status = runtime->GetVariable(
		variableName, &LOG_PATH_VARIABLE_GUID, nullptr, &size, buffer);
	if (status == EFI_SUCCESS) {
		auto* text = static_cast<CHAR16*>(buffer);
		const UINTN length = size / sizeof(CHAR16);
		if (size > capacity || size < 2 * sizeof(CHAR16) || size % sizeof(CHAR16) != 0 ||
			text[0] != L'\\' || text[length - 1] != 0)
		{
			status = EFI_INVALID_PARAMETER;
		} else {
			for (UINTN index = 0; index + 1 < length; ++index) {
				if (text[index] == 0) {
					status = EFI_INVALID_PARAMETER;
					break;
				}
			}
		}
	}
	if (status != EFI_SUCCESS) {
		services->FreePool(buffer);
		return status;
	}
	*path = static_cast<CHAR16*>(buffer);
	return EFI_SUCCESS;
}

static EFI_STATUS OpenLogFile(
	EFI_HANDLE image,
	EFI_SYSTEM_TABLE* systemTable,
	CHAR16* path,
	EFI_FILE_HANDLE* file)
{
	*file = nullptr;
	const auto* services = systemTable->BootServices;
	if (image == nullptr || services->HandleProtocol == nullptr) {
		return EFI_NOT_FOUND;
	}
	void* result = nullptr;
	EFI_STATUS status = services->HandleProtocol(image, &LoadedImageProtocol, &result);
	if (status != EFI_SUCCESS || result == nullptr) {
		return status == EFI_SUCCESS ? EFI_NOT_FOUND : status;
	}
	const auto* loadedImage = static_cast<EFI_LOADED_IMAGE*>(result);
	if (loadedImage->DeviceHandle == nullptr) {
		return EFI_NOT_FOUND;
	}
	result = nullptr;
	status = services->HandleProtocol(loadedImage->DeviceHandle, &FileSystemProtocol, &result);
	if (status != EFI_SUCCESS || result == nullptr) {
		return status == EFI_SUCCESS ? EFI_NOT_FOUND : status;
	}
	auto* filesystem = static_cast<EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*>(result);
	if (filesystem->OpenVolume == nullptr) {
		return EFI_UNSUPPORTED;
	}
	EFI_FILE_HANDLE root = nullptr;
	status = filesystem->OpenVolume(filesystem, &root);
	if (status == EFI_SUCCESS && root != nullptr && root->Open != nullptr && root->Close != nullptr) {
		status = root->Open(root, file, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
	} else if (status == EFI_SUCCESS) {
		status = EFI_UNSUPPORTED;
	}
	if (root != nullptr && root->Close != nullptr) {
		root->Close(root);
	}
	return status;
}

void TFileLog::Open(EFI_HANDLE image, EFI_SYSTEM_TABLE* systemTable) {
	if (File_ != nullptr || systemTable == nullptr || systemTable->BootServices == nullptr) {
		return;
	}
	SystemTable_ = systemTable;
	CHAR16* path = nullptr;
	EFI_STATUS status = ReadLogPath(systemTable, &path);
	if (status != EFI_SUCCESS) {
		if (status != EFI_NOT_FOUND) {
			Log(systemTable, L"[log] variable=AppleSetOsLogPath ignored=%r\n", status);
		}
		return;
	}
	status = OpenLogFile(image, systemTable, path, &File_);
	systemTable->BootServices->FreePool(path);
	if (status != EFI_SUCCESS || File_ == nullptr || File_->Close == nullptr ||
		File_->Read == nullptr || File_->Write == nullptr || File_->SetPosition == nullptr ||
		File_->GetPosition == nullptr || File_->Flush == nullptr)
	{
		Disable();
		Log(systemTable, L"[log] file=disabled open=%r\n", status == EFI_SUCCESS ? EFI_UNSUPPORTED : status);
		return;
	}
	status = File_->SetPosition(File_, 0);
	CHAR16 bom = 0;
	UINTN size = sizeof(bom);
	if (status == EFI_SUCCESS) {
		status = File_->Read(File_, &size, &bom);
	}
	if (status != EFI_SUCCESS || (size != 0 && (size != sizeof(bom) || bom != UTF16_BOM))) {
		Disable();
		Log(systemTable, L"[log] file=disabled reason=not-utf16-log\n");
		return;
	}
	const bool emptyFile = size == 0;
	status = File_->SetPosition(File_, END_OF_FILE_POSITION);
	uint64_t position = 0;
	if (status == EFI_SUCCESS) {
		status = File_->GetPosition(File_, &position);
	}
	if (status != EFI_SUCCESS || position % sizeof(CHAR16) != 0 || (emptyFile && position != 0)) {
		Disable();
		Log(systemTable, L"[log] file=disabled seek=%r\n", status == EFI_SUCCESS ? EFI_INVALID_PARAMETER : status);
		return;
	}
	if (emptyFile) {
		bom = UTF16_BOM;
		if (!WriteAll(&bom, sizeof(bom)) || File_->Flush(File_) != EFI_SUCCESS) {
			Disable();
			return;
		}
	}
}

void TFileLog::Close() {
	if (File_ != nullptr && File_->Flush != nullptr) {
		File_->Flush(File_);
	}
	Disable();
}

bool TFileLog::WriteAll(void* data, UINTN size) {
	auto* bytes = static_cast<uint8_t*>(data);
	while (size != 0) {
		UINTN written = size;
		const EFI_STATUS status = File_->Write(File_, &written, bytes);
		if (status != EFI_SUCCESS || written == 0 || written > size) {
			return false;
		}
		bytes += written;
		size -= written;
	}
	return true;
}

void TFileLog::Disable() {
	if (File_ != nullptr && File_->Close != nullptr) {
		File_->Close(File_);
	}
	File_ = nullptr;
}

void TFileLog::Write(
	EFI_SYSTEM_TABLE* systemTable,
	const wchar_t* format,
	va_list args,
	UINTN formattedLength)
{
	if (File_ == nullptr || SystemTable_ != systemTable || formattedLength == 0) {
		return;
	}
	if (formattedLength > (~UINTN{0} / sizeof(CHAR16)) - 1) {
		Disable();
		return;
	}
	// VPrint already counted the entire message, including its inserted CRs.
	// Allocate exactly that text plus its NUL, without a fixed message limit.
	const UINTN size = (formattedLength + 1) * sizeof(CHAR16);
	const auto* services = systemTable->BootServices;
	void* buffer = nullptr;
	if (services->AllocatePool(EfiLoaderData, size, &buffer) != EFI_SUCCESS || buffer == nullptr) {
		Disable();
		return;
	}
	const UINTN length = UnicodeVSPrint(
		static_cast<CHAR16*>(buffer), size, reinterpret_cast<const CHAR16*>(format), args);
	const bool complete = length == formattedLength && WriteAll(buffer, length * sizeof(CHAR16));
	services->FreePool(buffer);
	if (!complete || File_->Flush(File_) != EFI_SUCCESS) {
		Disable();
	}
}

} // namespace NLogging::NPrivate
