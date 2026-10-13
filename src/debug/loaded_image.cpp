#include <debug/loaded_image.h>
#include <debug/protocol_lookup.h>
#include <logging/logging.h>

extern "C" {
#include <efi.h>
#include <efilib.h>
}

namespace NDebug {

static EFI_GUID DEVICE_PATH_GUID = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID LOADED_IMAGE_GUID = EFI_LOADED_IMAGE_PROTOCOL_GUID;

// PE/COFF layout: DOS e_lfanew at 0x3c, then PE signature and a 20-byte COFF header.
static constexpr uint32_t DOS_SIGNATURE = 0x5a4d;
static constexpr uint32_t PE_SIGNATURE = 0x00004550;
static constexpr unsigned DOS_PE_OFFSET = 0x3c;
static constexpr unsigned PE_SIGNATURE_SIZE = 4;
static constexpr unsigned COFF_MACHINE_OFFSET = 0;
static constexpr unsigned COFF_OPTIONAL_SIZE_OFFSET = 16;
static constexpr unsigned COFF_HEADER_SIZE = 20;
static constexpr uint32_t PE32_MAGIC = 0x10b;
static constexpr uint32_t PE32_PLUS_MAGIC = 0x20b;
// The subsystem is a 16-bit field at offset 68 in both PE optional-header formats.
static constexpr unsigned PE_SUBSYSTEM_OFFSET = 68;
static constexpr unsigned PE_SUBSYSTEM_END = PE_SUBSYSTEM_OFFSET + sizeof(uint16_t);
static constexpr uint32_t PE_MACHINE_IA32 = 0x014c;
static constexpr uint32_t PE_MACHINE_X64 = 0x8664;
static constexpr uint32_t PE_MACHINE_ARM64 = 0xaa64;
static constexpr uint32_t PE_EFI_APPLICATION = 10;
static constexpr uint32_t PE_EFI_BOOT_SERVICES_DRIVER = 11;
static constexpr uint32_t PE_EFI_RUNTIME_DRIVER = 12;

// Packed UEFI hard-drive node offsets; sizeof the native struct includes padding.
static constexpr unsigned HARD_DRIVE_NODE_SIZE = 42;
static constexpr unsigned HARD_DRIVE_SIGNATURE_OFFSET = 24;
static constexpr unsigned HARD_DRIVE_MBR_TYPE_OFFSET = 40;
static constexpr unsigned HARD_DRIVE_SIGNATURE_TYPE_OFFSET = 41;

// Bound traversal of firmware-owned paths, whose allocation size is not exposed.
static constexpr unsigned DEVICE_PATH_MAX_NODES = 256;
static constexpr unsigned DEVICE_PATH_MAX_BYTES = 64 * 1024;

// Unaligned UTF-16 cannot be passed to the formatter directly. Limit its scratch
// space to 256 bytes; longer spans are streamed in chunks, never truncated.
static constexpr unsigned UNALIGNED_PATH_CHUNK_BYTES = 256;
static constexpr unsigned UNALIGNED_PATH_CHUNK_CHARACTERS =
	UNALIGNED_PATH_CHUNK_BYTES / sizeof(CHAR16);

struct TPeImageInfo {
	uint32_t Machine;
	uint32_t Subsystem;
};

static const CHAR16* AsEfiText(const wchar_t* text) {
	return reinterpret_cast<const CHAR16*>(text);
}

static bool ReadImageValue(
	const EFI_LOADED_IMAGE* image,
	uint64_t offset,
	unsigned size,
	uint32_t& value)
{
	if (size > sizeof(value) || offset > image->ImageSize || size > image->ImageSize - offset) {
		return false;
	}
	const uint8_t* bytes = static_cast<const uint8_t*>(image->ImageBase) + offset;
	value = 0;
	for (unsigned index = 0; index < size; ++index) {
		value |= uint32_t{bytes[index]} << (index * 8);
	}
	return true;
}

static bool ReadPeHeaders(const EFI_LOADED_IMAGE* image, TPeImageInfo& info) {
	uint32_t imageSignature = 0;
	const bool imageSignatureRead = ReadImageValue(
		/* image */ image,
		/* offset */ 0,
		/* size */ sizeof(uint32_t),
		/* value */ imageSignature);
	if (!imageSignatureRead) {
		return false;
	}

	uint32_t peOffset = 0;
	if (static_cast<uint16_t>(imageSignature) == DOS_SIGNATURE) {
		const bool peOffsetRead = ReadImageValue(
			/* image */ image,
			/* offset */ DOS_PE_OFFSET,
			/* size */ sizeof(uint32_t),
			/* value */ peOffset);
		if (!peOffsetRead) {
			return false;
		}
		uint32_t peSignature = 0;
		const bool peSignatureRead = ReadImageValue(
			/* image */ image,
			/* offset */ peOffset,
			/* size */ sizeof(uint32_t),
			/* value */ peSignature);
		if (!peSignatureRead || peSignature != PE_SIGNATURE) {
			return false;
		}
	} else if (imageSignature != PE_SIGNATURE) {
		return false;
	}

	const uint64_t coffOffset = uint64_t{peOffset} + PE_SIGNATURE_SIZE;
	const bool machineRead = ReadImageValue(
		/* image */ image,
		/* offset */ coffOffset + COFF_MACHINE_OFFSET,
		/* size */ sizeof(uint16_t),
		/* value */ info.Machine);
	if (!machineRead) {
		return false;
	}
	uint32_t optionalSize = 0;
	const bool optionalSizeRead = ReadImageValue(
		/* image */ image,
		/* offset */ coffOffset + COFF_OPTIONAL_SIZE_OFFSET,
		/* size */ sizeof(uint16_t),
		/* value */ optionalSize);
	if (!optionalSizeRead) {
		return false;
	}

	const uint64_t optionalOffset = coffOffset + COFF_HEADER_SIZE;
	if (optionalSize < PE_SUBSYSTEM_END || optionalOffset + optionalSize > image->ImageSize) {
		return false;
	}
	uint32_t optionalMagic = 0;
	const bool optionalMagicRead = ReadImageValue(
		/* image */ image,
		/* offset */ optionalOffset,
		/* size */ sizeof(uint16_t),
		/* value */ optionalMagic);
	if (!optionalMagicRead) {
		return false;
	}
	if (optionalMagic != PE32_MAGIC && optionalMagic != PE32_PLUS_MAGIC) {
		return false;
	}
	const bool subsystemRead = ReadImageValue(
		/* image */ image,
		/* offset */ optionalOffset + PE_SUBSYSTEM_OFFSET,
		/* size */ sizeof(uint16_t),
		/* value */ info.Subsystem);
	return subsystemRead;
}

static const wchar_t* GetImageArchitectureName(uint32_t machine) {
	switch (machine) {
	case PE_MACHINE_X64: return L"x64";
	case PE_MACHINE_IA32: return L"ia32";
	case PE_MACHINE_ARM64: return L"arm64";
	default: return L"unknown";
	}
}

static const wchar_t* GetImageTypeName(uint32_t subsystem) {
	switch (subsystem) {
	case PE_EFI_APPLICATION: return L"application";
	case PE_EFI_BOOT_SERVICES_DRIVER: return L"boot-services-driver";
	case PE_EFI_RUNTIME_DRIVER: return L"runtime-driver";
	default: return L"unknown";
	}
}

static void LogImageType(EFI_SYSTEM_TABLE* systemTable, const EFI_LOADED_IMAGE* image) {
	if (image->ImageBase == nullptr || image->ImageSize == 0) {
		NLogging::Log(systemTable, L"[image] arch=unknown type=unknown reason=missing-image\n");
		return;
	}
	TPeImageInfo info = {};
	const bool headersRead = ReadPeHeaders(image, info);
	if (!headersRead) {
		NLogging::Log(systemTable, L"[image] arch=unknown type=unknown reason=invalid-pe\n");
		return;
	}
	const wchar_t* arch = GetImageArchitectureName(info.Machine);
	const wchar_t* type = GetImageTypeName(info.Subsystem);
	NLogging::Log(
		systemTable,
		L"[image] arch=%s type=%s machine=%04x subsystem=%d\n",
		/* arch */ AsEfiText(arch),
		/* type */ AsEfiText(type),
		/* machine */ info.Machine,
		/* subsystem */ info.Subsystem);
}

static unsigned GetPathNodeSize(const uint8_t* node) {
	// Device-path header: Type, SubType, then the 16-bit little-endian Length.
	return unsigned{node[2]} | (unsigned{node[3]} << 8);
}

static uint8_t GetPathNodeType(const uint8_t* node) {
	return node[0] & EFI_DP_TYPE_MASK;
}

// These checks limit traversal; they cannot validate arbitrary firmware pointers.
static const wchar_t* GetDevicePathError(const EFI_DEVICE_PATH* path) {
	const uint8_t* node = reinterpret_cast<const uint8_t*>(path);
	unsigned totalSize = 0;
	for (unsigned index = 0; node != nullptr && index < DEVICE_PATH_MAX_NODES; ++index) {
		if (totalSize > DEVICE_PATH_MAX_BYTES - sizeof(EFI_DEVICE_PATH)) {
			return L"invalid-device-path";
		}
		const unsigned size = GetPathNodeSize(node);
		if (size < sizeof(EFI_DEVICE_PATH) || size > DEVICE_PATH_MAX_BYTES - totalSize) {
			return L"invalid-device-path";
		}
		if (GetPathNodeType(node) == END_DEVICE_PATH_TYPE) {
			if (size != sizeof(EFI_DEVICE_PATH)) {
				return L"invalid-device-path";
			}
			if (node[1] == END_ENTIRE_DEVICE_PATH_SUBTYPE) {
				return nullptr;
			}
			if (node[1] == END_INSTANCE_DEVICE_PATH_SUBTYPE) {
				return L"multiple-device-path-instances";
			}
			return L"invalid-device-path";
		}
		totalSize += size;
		node += size;
	}
	return L"invalid-device-path";
}

static bool IsFilePathNode(const uint8_t* node) {
	return GetPathNodeType(node) == MEDIA_DEVICE_PATH && node[1] == MEDIA_FILEPATH_DP;
}

static bool IsValidFilePathNode(const uint8_t* node) {
	const unsigned size = GetPathNodeSize(node);
	const unsigned pathBytes = size - sizeof(EFI_DEVICE_PATH);
	return pathBytes >= sizeof(CHAR16) && pathBytes % sizeof(CHAR16) == 0 &&
		node[size - 2] == 0 && node[size - 1] == 0;
}

static CHAR16 ReadPathCharacter(const uint8_t* node, unsigned index) {
	// Device-path payloads can start at any byte boundary.
	const unsigned offset = sizeof(EFI_DEVICE_PATH) + index * sizeof(CHAR16);
	return uint16_t{node[offset]} | (uint16_t{node[offset + 1]} << 8);
}

static void LogPathSpan(
	EFI_SYSTEM_TABLE* systemTable,
	const uint8_t* node,
	unsigned start,
	unsigned length)
{
	const uint8_t* text = node + sizeof(EFI_DEVICE_PATH) + start * sizeof(CHAR16);
	const bool aligned = reinterpret_cast<uintptr_t>(text) % alignof(CHAR16) == 0;
	if (aligned) {
		// GNU-EFI's field width bounds both reads and output to this exact span.
		NLogging::Log(
			systemTable,
			L"%.*s",
			/* length */ static_cast<UINTN>(length),
			/* text */ reinterpret_cast<const CHAR16*>(text));
		return;
	}

	CHAR16 chunk[UNALIGNED_PATH_CHUNK_CHARACTERS];
	unsigned offset = 0;
	while (offset < length) {
		unsigned chunkLength = length - offset;
		if (chunkLength > UNALIGNED_PATH_CHUNK_CHARACTERS) {
			chunkLength = UNALIGNED_PATH_CHUNK_CHARACTERS;
		}
		for (unsigned index = 0; index < chunkLength; ++index) {
			chunk[index] = ReadPathCharacter(node, start + offset + index);
		}
		NLogging::Log(
			systemTable,
			L"%.*s",
			/* length */ static_cast<UINTN>(chunkLength),
			/* text */ chunk);
		offset += chunkLength;
	}
}

static void LogImagePath(EFI_SYSTEM_TABLE* systemTable, const EFI_DEVICE_PATH* path) {
	if (path == nullptr) {
		NLogging::Log(systemTable, L"[image] path=unknown reason=missing-device-path\n");
		return;
	}
	const wchar_t* pathError = GetDevicePathError(path);
	if (pathError != nullptr) {
		NLogging::Log(systemTable, L"[image] path=unknown reason=%s\n", AsEfiText(pathError));
		return;
	}
	const uint8_t* node = reinterpret_cast<const uint8_t*>(path);
	bool hasFilePath = false;
	// Validate every fragment before emitting any part of the path record.
	while (GetPathNodeType(node) != END_DEVICE_PATH_TYPE) {
		if (IsFilePathNode(node)) {
			hasFilePath = true;
			const bool fragmentValid = IsValidFilePathNode(node);
			if (!fragmentValid) {
				NLogging::Log(systemTable, L"[image] path=unknown reason=invalid-filepath-node\n");
				return;
			}
		}
		node += GetPathNodeSize(node);
	}
	if (!hasFilePath) {
		NLogging::Log(systemTable, L"[image] path=unknown reason=no-filepath-node\n");
		return;
	}

	NLogging::Log(systemTable, L"[image] path=");
	node = reinterpret_cast<const uint8_t*>(path);
	bool hasText = false;
	CHAR16 lastCharacter = 0;
	while (GetPathNodeType(node) != END_DEVICE_PATH_TYPE) {
		if (IsFilePathNode(node)) {
			unsigned index = 0;
			CHAR16 character = ReadPathCharacter(node, index);
			const bool needsSeparator = hasText && character != 0 &&
				lastCharacter != '\\' && character != '\\';
			if (needsSeparator) {
				NLogging::Log(systemTable, L"\\");
			}
			while (character != 0) {
				const bool repeatedSeparator = hasText && lastCharacter == '\\' && character == '\\';
				if (repeatedSeparator) {
					character = ReadPathCharacter(node, ++index);
					continue;
				}
				const unsigned start = index;
				do {
					lastCharacter = character;
					hasText = true;
					character = ReadPathCharacter(node, ++index);
				} while (character != 0 && (lastCharacter != '\\' || character != '\\'));
				LogPathSpan(/* systemTable */ systemTable, /* node */ node,
					/* start */ start, /* length */ index - start);
			}
		}
		node += GetPathNodeSize(node);
	}
	NLogging::Log(systemTable, L"\n");
}

static bool IsGptPartitionNode(const uint8_t* node, unsigned size) {
	if (GetPathNodeType(node) != MEDIA_DEVICE_PATH || node[1] != MEDIA_HARDDRIVE_DP) {
		return false;
	}
	if (size < HARD_DRIVE_NODE_SIZE) {
		return false;
	}
	return node[HARD_DRIVE_MBR_TYPE_OFFSET] == MBR_TYPE_EFI_PARTITION_TABLE_HEADER &&
		node[HARD_DRIVE_SIGNATURE_TYPE_OFFSET] == SIGNATURE_TYPE_GUID;
}

static void LogSourcePartition(EFI_SYSTEM_TABLE* systemTable, EFI_HANDLE device) {
	EFI_DEVICE_PATH* path = nullptr;
	const EFI_STATUS status = NPrivate::GetProtocol(
		/* systemTable */ systemTable,
		/* handle */ device,
		/* guid */ &DEVICE_PATH_GUID,
		/* protocol */ &path);
	if (status != EFI_SUCCESS) {
		NLogging::Log(systemTable, L"[image] partition=unknown device_path_status=%r\n", status);
		return;
	}
	const wchar_t* pathError = GetDevicePathError(path);
	if (pathError != nullptr) {
		NLogging::Log(systemTable, L"[image] partition=unknown reason=%s\n", AsEfiText(pathError));
		return;
	}
	const uint8_t* node = reinterpret_cast<const uint8_t*>(path);
	while (GetPathNodeType(node) != END_DEVICE_PATH_TYPE) {
		const unsigned size = GetPathNodeSize(node);
		if (IsGptPartitionNode(node, size)) {
			EFI_GUID partitionGuid;
			CopyMem(
				/* destination */ &partitionGuid,
				/* source */ const_cast<uint8_t*>(node + HARD_DRIVE_SIGNATURE_OFFSET),
				/* length */ sizeof(partitionGuid));
			NLogging::Log(systemTable, L"[image] partition=%g source=gpt-device-path\n", &partitionGuid);
			return;
		}
		node += size;
	}
	NLogging::Log(systemTable, L"[image] partition=unknown reason=no-gpt-partition\n");
}

void LogLoadedImage(EFI_HANDLE image, EFI_SYSTEM_TABLE* systemTable) {
	if (!NLogging::IsAvailable(systemTable)) {
		return;
	}
	EFI_LOADED_IMAGE* loadedImage = nullptr;
	const EFI_STATUS lookupStatus = NPrivate::GetProtocol(
		/* systemTable */ systemTable,
		/* handle */ image,
		/* guid */ &LOADED_IMAGE_GUID,
		/* protocol */ &loadedImage);
	if (lookupStatus != EFI_SUCCESS) {
		NLogging::Log(systemTable,
			L"[image] lookup=%r arch=unknown type=unknown partition=unknown path=unknown "
			L"reason=no-loaded-image\n", lookupStatus);
		return;
	}
	LogImageType(systemTable, loadedImage);
	LogSourcePartition(systemTable, loadedImage->DeviceHandle);
	LogImagePath(systemTable, loadedImage->FilePath);
}

} // namespace NDebug
