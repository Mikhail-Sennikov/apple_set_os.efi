// Exercise the real core and console diagnostics against mocked EFI services.
#include <apple_set_os.h>
#include <debug/loaded_image.h>

extern "C" {
#include <efi.h>
#include <efilib.h>
#include <efipciio.h>
}
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Wire offsets come from PE/COFF and UEFI Device Path, not padded C structs.
static constexpr UINTN TEST_DOS_PE_POINTER_OFFSET = 0x3c;
static constexpr UINTN TEST_PE_HEADER_OFFSET = 0x80; // Chosen placement in this fixture.
static constexpr UINTN TEST_PE_MACHINE_OFFSET = TEST_PE_HEADER_OFFSET + 4;
static constexpr UINTN TEST_PE_OPTIONAL_SIZE_OFFSET = TEST_PE_HEADER_OFFSET + 20;
static constexpr UINTN TEST_PE_OPTIONAL_HEADER_OFFSET = TEST_PE_HEADER_OFFSET + 24;
static constexpr UINTN TEST_PE_SUBSYSTEM_OFFSET = TEST_PE_OPTIONAL_HEADER_OFFSET + 68;
static constexpr UINT16 TEST_SUBSYSTEM_APPLICATION = 10;
static constexpr UINT16 TEST_SUBSYSTEM_DRIVER = 11;
static constexpr UINTN TEST_GPT_NODE_SIZE = 42;
static constexpr UINTN TEST_GPT_SIGNATURE_OFFSET = 24;
static constexpr UINTN TEST_GPT_MBR_TYPE_OFFSET = 40;
static constexpr UINTN TEST_GPT_SIGNATURE_TYPE_OFFSET = 41;
static constexpr UINT32 TEST_PCI_ID_OFFSET = 0;
static constexpr UINT32 TEST_PCI_CLASS_REVISION_OFFSET = 8;
// Root-bridge configuration address of bus 0, device 2, function 0.
static constexpr UINT64 TEST_IGD_CONFIG_ADDRESS = UINT64{2} << 16;
static constexpr UINT32 TEST_INTEL_ID = 0x3e9b8086;
static constexpr UINT32 TEST_INTEL_CLASS_REVISION = 0x03000002;

struct TMockAppleOsInfoProtocol {
	UINTN Revision;
	void (EFIAPI* SetOsVersion)(char* version);
	void (EFIAPI* SetOsVendor)(char* vendor);
};

struct TMockGpu {
	EFI_PCI_IO_PROTOCOL Pci;
	UINT32 Id;
	UINT32 ClassRevision;
	UINTN Segment;
	UINTN Bus;
	UINTN Device;
	UINTN Function;
	EFI_STATUS ReadError;
	EFI_STATUS LocationError;
};

struct TMockImagePath {
	EFI_DEVICE_PATH File;
	CHAR16 Path[sizeof(L"\\EFI\\custom\\apple_set_os.efi") / sizeof(CHAR16)];
	EFI_DEVICE_PATH End;
};

static EFI_BOOT_SERVICES SERVICES;
static EFI_RUNTIME_SERVICES RUNTIME;
static EFI_SYSTEM_TABLE SYSTEM_TABLE;
static SIMPLE_TEXT_OUTPUT_INTERFACE CONSOLE;
static SIMPLE_TEXT_OUTPUT_MODE CONSOLE_MODE;
static EFI_LOADED_IMAGE DRIVER_IMAGE;
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL ROOT_BRIDGE;
static TMockAppleOsInfoProtocol PROTOCOL;
static TMockGpu GPUS[3];
static TMockImagePath IMAGE_PATH;
static CHAR16 FIRMWARE_VENDOR[] = { 'A', 'p', 'p', 'l', 'e', 0 };
static UINT8 DEVICE;
static UINT8 PARTITION_PATH[TEST_GPT_NODE_SIZE + sizeof(EFI_DEVICE_PATH)];
static UINT8 PE_IMAGE[512];
static void* LOCATED_PROTOCOL;
static CHAR16 CONTENTS[32768];
static UINTN CONTENT_LENGTH;
static UINTN GPU_COUNT;
static int PROTOCOL_LOCATE_COUNT;
static int VENDOR_CALLS;
static int VERSION_CALLS;
enum class ECallback {
	Vendor,
	Version
};
static ECallback CALLBACK_ORDER[2];
static UINTN CALLBACK_COUNT;
static int LOCATE_RETURNS_NULL;
static int LOCATE_FAIL_RETURNS_POINTER;
static int HANDLE_RETURNS_NULL;
static int FORBIDDEN_OPERATIONS;
static int PROBE_READS;
static int PCI_READS;
static int HANDLE_PROTOCOL_CALLS;
static int LOCATE_BUFFER_CALLS;
static int TIME_READS;
static int MONOTONIC_READS;
static int OUTPUT_CALLS;
static int CONSOLE_OVERFLOW;
static int POOL_OPERATIONS;
static EFI_STATUS HANDLE_ERROR;
static EFI_STATUS LOCATE_STATUS;
static EFI_STATUS OUTPUT_ERROR;
static EFI_STATUS GPU_LOCATE_ERROR;
static EFI_STATUS PROBE_ERROR;

static bool SameGuid(const EFI_GUID* left, const EFI_GUID* right) {
	return memcmp(left, right, sizeof(*left)) == 0;
}

static EFI_STATUS EFIAPI MockOutputString(SIMPLE_TEXT_OUTPUT_INTERFACE* self, CHAR16* text) {
	++OUTPUT_CALLS;
	if (self != &CONSOLE || text == nullptr) {
		CONSOLE_OVERFLOW = 1;
		return EFI_INVALID_PARAMETER;
	}
	if (OUTPUT_ERROR != EFI_SUCCESS) {
		return OUTPUT_ERROR;
	}
	for (UINTN index = 0; text[index] != 0; ++index) {
		if (CONTENT_LENGTH >= sizeof(CONTENTS) / sizeof(CONTENTS[0])) {
			CONSOLE_OVERFLOW = 1;
			return EFI_VOLUME_FULL;
		}
		CONTENTS[CONTENT_LENGTH++] = text[index];
	}
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockAllocatePool(
	EFI_MEMORY_TYPE,
	UINTN size,
	void** buffer)
{
	++POOL_OPERATIONS;
	*buffer = malloc(size);
	if (*buffer == nullptr) {
		return EFI_OUT_OF_RESOURCES;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockFreePool(void* buffer) {
	++POOL_OPERATIONS;
	free(buffer);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockLocateHandle(
	EFI_LOCATE_SEARCH_TYPE,
	EFI_GUID*,
	void*,
	UINTN* size,
	EFI_HANDLE*)
{
	*size = 0;
	return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI MockHandleProtocol(
	EFI_HANDLE handle,
	EFI_GUID* guid,
	void** interface)
{
	++HANDLE_PROTOCOL_CALLS;
	*interface = nullptr;
	if (SameGuid(guid, &FileSystemProtocol)) {
		++FORBIDDEN_OPERATIONS;
		return EFI_ACCESS_DENIED;
	}
	if (handle == &DRIVER_IMAGE && SameGuid(guid, &LoadedImageProtocol)) {
		if (HANDLE_ERROR != EFI_SUCCESS) {
			return HANDLE_ERROR;
		}
		if (HANDLE_RETURNS_NULL) {
			return EFI_SUCCESS;
		}
		*interface = &DRIVER_IMAGE;
	} else if (handle == &DEVICE && SameGuid(guid, &DevicePathProtocol)) {
		*interface = PARTITION_PATH;
	} else if (handle == &ROOT_BRIDGE) {
		*interface = &ROOT_BRIDGE;
	} else {
		for (UINTN index = 0; index < GPU_COUNT; ++index) {
			if (handle == &GPUS[index]) {
				*interface = &GPUS[index].Pci;
			}
		}
	}
	if (*interface == nullptr) {
		return EFI_NOT_FOUND;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockLocateProtocol(
	EFI_GUID*,
	void*,
	void** interface)
{
	++PROTOCOL_LOCATE_COUNT;
	*interface = nullptr;
	if (LOCATE_FAIL_RETURNS_POINTER) {
		// Deliberately violate LocateProtocol's usual error result for this fixture.
		*interface = LOCATED_PROTOCOL;
	} else if (LOCATE_STATUS == EFI_SUCCESS && !LOCATE_RETURNS_NULL) {
		*interface = LOCATED_PROTOCOL;
	}
	return LOCATE_STATUS;
}

static EFI_STATUS EFIAPI MockLocateBuffer(
	EFI_LOCATE_SEARCH_TYPE,
	EFI_GUID* guid,
	void*,
	UINTN* count,
	EFI_HANDLE** handles)
{
	++LOCATE_BUFFER_CALLS;
	EFI_GUID pciGuid = EFI_PCI_IO_PROTOCOL_GUID;
	EFI_GUID rootGuid = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
	*count = 0;
	*handles = nullptr;
	if (SameGuid(guid, &pciGuid)) {
		if (GPU_LOCATE_ERROR != EFI_SUCCESS) {
			return GPU_LOCATE_ERROR;
		}
		*count = GPU_COUNT;
	} else if (SameGuid(guid, &rootGuid)) {
		*count = 1;
	} else {
		return EFI_NOT_FOUND;
	}
	*handles = static_cast<EFI_HANDLE*>(malloc(*count * sizeof(EFI_HANDLE)));
	if (*handles == nullptr && *count != 0) {
		return EFI_OUT_OF_RESOURCES;
	}
	for (UINTN index = 0; index < *count; ++index) {
		if (SameGuid(guid, &pciGuid)) {
			(*handles)[index] = &GPUS[index];
		} else {
			(*handles)[index] = &ROOT_BRIDGE;
		}
	}
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockPciRead(
	EFI_PCI_IO_PROTOCOL* self,
	EFI_PCI_IO_PROTOCOL_WIDTH width,
	UINT32 offset,
	UINTN count,
	void* buffer)
{
	++PCI_READS;
	const bool knownOffset = offset == TEST_PCI_ID_OFFSET || offset == TEST_PCI_CLASS_REVISION_OFFSET;
	if (width != EfiPciIoWidthUint32 || count != 1 || !knownOffset) {
		return EFI_INVALID_PARAMETER;
	}
	for (TMockGpu& gpu : GPUS) {
		if (self == &gpu.Pci) {
			if (gpu.ReadError != EFI_SUCCESS) {
				return gpu.ReadError;
			}
			if (offset == TEST_PCI_ID_OFFSET) {
				*static_cast<UINT32*>(buffer) = gpu.Id;
			} else {
				*static_cast<UINT32*>(buffer) = gpu.ClassRevision;
			}
			return EFI_SUCCESS;
		}
	}
	return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI MockPciWrite(
	EFI_PCI_IO_PROTOCOL*,
	EFI_PCI_IO_PROTOCOL_WIDTH,
	UINT32,
	UINTN,
	void*)
{
	++FORBIDDEN_OPERATIONS;
	return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI MockLocation(
	EFI_PCI_IO_PROTOCOL* self,
	UINTN* segment,
	UINTN* bus,
	UINTN* device,
	UINTN* function)
{
	for (TMockGpu& gpu : GPUS) {
		if (self == &gpu.Pci) {
			if (gpu.LocationError != EFI_SUCCESS) {
				return gpu.LocationError;
			}
			*segment = gpu.Segment;
			*bus = gpu.Bus;
			*device = gpu.Device;
			*function = gpu.Function;
			return EFI_SUCCESS;
		}
	}
	return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI MockProbeRead(
	EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL*,
	EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width,
	UINT64 address,
	UINTN count,
	void* buffer)
{
	++PROBE_READS;
	if (PROBE_ERROR != EFI_SUCCESS) {
		return PROBE_ERROR;
	}
	const UINT64 classAddress = TEST_IGD_CONFIG_ADDRESS + TEST_PCI_CLASS_REVISION_OFFSET;
	const bool knownAddress = address == TEST_IGD_CONFIG_ADDRESS || address == classAddress;
	if (width != EfiPciIoWidthUint32 || count != 1 || !knownAddress) {
		return EFI_INVALID_PARAMETER;
	}
	if (address == TEST_IGD_CONFIG_ADDRESS) {
		*static_cast<UINT32*>(buffer) = TEST_INTEL_ID;
	} else {
		*static_cast<UINT32*>(buffer) = TEST_INTEL_CLASS_REVISION;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockProbeWrite(
	EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL*,
	EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH,
	UINT64,
	UINTN,
	void*)
{
	++FORBIDDEN_OPERATIONS;
	return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI MockGetTime(
	EFI_TIME* time,
	EFI_TIME_CAPABILITIES*)
{
	++TIME_READS;
	*time = { .Year = 2026, .Month = 10, .Day = 10, .Hour = 1, .Minute = 2, .Second = 3 };
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockMonotonic(UINT64* count) {
	++MONOTONIC_READS;
	*count = 42;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI MockGetVariable(
	CHAR16*,
	EFI_GUID*,
	UINT32*,
	UINTN*,
	void*)
{
	return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI MockSetVariable(
	CHAR16*,
	EFI_GUID*,
	UINT32,
	UINTN,
	void*)
{
	++FORBIDDEN_OPERATIONS;
	return EFI_ACCESS_DENIED;
}

static EFI_STATUS EFIAPI MockCreateEvent(
	UINT32,
	EFI_TPL,
	EFI_EVENT_NOTIFY,
	void*,
	EFI_EVENT*)
{
	++FORBIDDEN_OPERATIONS;
	return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI MockRegisterNotify(
	EFI_GUID*,
	EFI_EVENT,
	void**)
{
	++FORBIDDEN_OPERATIONS;
	return EFI_UNSUPPORTED;
}

static void RecordCallback(ECallback callback) {
	if (CALLBACK_COUNT < sizeof(CALLBACK_ORDER) / sizeof(CALLBACK_ORDER[0])) {
		CALLBACK_ORDER[CALLBACK_COUNT] = callback;
	}
	++CALLBACK_COUNT;
}

static bool CallbacksInVendorVersionOrder() {
	return CALLBACK_COUNT == 2 &&
		CALLBACK_ORDER[0] == ECallback::Vendor &&
		CALLBACK_ORDER[1] == ECallback::Version;
}

static void EFIAPI MockVendor(char* value) {
	if (strcmp(value, "Apple Inc.") == 0) {
		++VENDOR_CALLS;
	}
	RecordCallback(ECallback::Vendor);
}

static void EFIAPI MockVersion(char* value) {
	if (strcmp(value, "Mac OS X 10.9") == 0) {
		++VERSION_CALLS;
	}
	RecordCallback(ECallback::Version);
}

template<typename T>
static void StoreValue(UINT8* bytes, UINTN offset, T value) {
	memcpy(bytes + offset, &value, sizeof(value));
}

static void ResetCase() {
	SERVICES = {};
	RUNTIME = {};
	SYSTEM_TABLE = {};
	CONSOLE = {};
	CONSOLE_MODE = {};
	CONSOLE_MODE.Attribute = EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK);
	CONSOLE.OutputString = MockOutputString;
	CONSOLE.Mode = &CONSOLE_MODE;
	memset(CONTENTS, 0, sizeof(CONTENTS));
	CONTENT_LENGTH = 0;
	PROTOCOL_LOCATE_COUNT = 0;
	VENDOR_CALLS = 0;
	VERSION_CALLS = 0;
	CALLBACK_COUNT = 0;
	LOCATE_RETURNS_NULL = 0;
	LOCATE_FAIL_RETURNS_POINTER = 0;
	HANDLE_RETURNS_NULL = 0;
	FORBIDDEN_OPERATIONS = 0;
	PROBE_READS = 0;
	PCI_READS = 0;
	HANDLE_PROTOCOL_CALLS = 0;
	LOCATE_BUFFER_CALLS = 0;
	TIME_READS = 0;
	MONOTONIC_READS = 0;
	OUTPUT_CALLS = 0;
	CONSOLE_OVERFLOW = 0;
	POOL_OPERATIONS = 0;
	HANDLE_ERROR = EFI_SUCCESS;
	LOCATE_STATUS = EFI_SUCCESS;
	OUTPUT_ERROR = EFI_SUCCESS;
	GPU_LOCATE_ERROR = EFI_SUCCESS;
	PROBE_ERROR = EFI_SUCCESS;
	SERVICES.LocateHandle = MockLocateHandle;
	SERVICES.AllocatePool = MockAllocatePool;
	SERVICES.FreePool = MockFreePool;
	SERVICES.LocateHandleBuffer = MockLocateBuffer;
	SERVICES.HandleProtocol = MockHandleProtocol;
	SERVICES.LocateProtocol = MockLocateProtocol;
	SERVICES.GetNextMonotonicCount = MockMonotonic;
	SERVICES.CreateEvent = MockCreateEvent;
	SERVICES.RegisterProtocolNotify = MockRegisterNotify;
	RUNTIME.GetTime = MockGetTime;
	RUNTIME.GetVariable = MockGetVariable;
	RUNTIME.SetVariable = MockSetVariable;
	SYSTEM_TABLE.BootServices = &SERVICES;
	SYSTEM_TABLE.RuntimeServices = &RUNTIME;
	SYSTEM_TABLE.ConOut = &CONSOLE;
	SYSTEM_TABLE.FirmwareVendor = FIRMWARE_VENDOR;
	SYSTEM_TABLE.FirmwareRevision = 0x1234;
	SYSTEM_TABLE.Hdr.Revision = 0x000200a0;
	DRIVER_IMAGE = {};
	DRIVER_IMAGE.DeviceHandle = &DEVICE;
	DRIVER_IMAGE.ImageDataType = EfiBootServicesData;
	DRIVER_IMAGE.ImageBase = PE_IMAGE;
	DRIVER_IMAGE.ImageSize = sizeof(PE_IMAGE);
	memset(PE_IMAGE, 0, sizeof(PE_IMAGE));
	StoreValue<UINT16>(PE_IMAGE, 0, 0x5a4d);
	StoreValue<UINT32>(PE_IMAGE, TEST_DOS_PE_POINTER_OFFSET, TEST_PE_HEADER_OFFSET);
	StoreValue<UINT32>(PE_IMAGE, TEST_PE_HEADER_OFFSET, 0x4550); // PE signature.
	StoreValue<UINT16>(PE_IMAGE, TEST_PE_MACHINE_OFFSET, 0x8664); // x64 machine.
	StoreValue<UINT16>(PE_IMAGE, TEST_PE_OPTIONAL_SIZE_OFFSET, 112);
	StoreValue<UINT16>(PE_IMAGE, TEST_PE_OPTIONAL_HEADER_OFFSET, 0x20b); // PE32+.
	StoreValue<UINT16>(PE_IMAGE, TEST_PE_SUBSYSTEM_OFFSET, TEST_SUBSYSTEM_DRIVER);
	IMAGE_PATH = {};
	IMAGE_PATH.File.Type = MEDIA_DEVICE_PATH;
	IMAGE_PATH.File.SubType = MEDIA_FILEPATH_DP;
	SetDevicePathNodeLength(&IMAGE_PATH.File, sizeof(IMAGE_PATH.File) + sizeof(IMAGE_PATH.Path));
	memcpy(IMAGE_PATH.Path, L"\\EFI\\custom\\apple_set_os.efi", sizeof(IMAGE_PATH.Path));
	SetDevicePathEndNode(&IMAGE_PATH.End);
	DRIVER_IMAGE.FilePath = &IMAGE_PATH.File;
	memset(PARTITION_PATH, 0, sizeof(PARTITION_PATH));
	EFI_DEVICE_PATH partitionNode = {};
	partitionNode.Type = MEDIA_DEVICE_PATH;
	partitionNode.SubType = MEDIA_HARDDRIVE_DP;
	SetDevicePathNodeLength(&partitionNode, TEST_GPT_NODE_SIZE);
	memcpy(PARTITION_PATH, &partitionNode, sizeof(partitionNode));
	EFI_GUID partitionGuid = { .Data1 = 0x12345678, .Data2 = 0x1234, .Data3 = 0x5678,
		.Data4 = { 0x90, 0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78 } };
	memcpy(PARTITION_PATH + TEST_GPT_SIGNATURE_OFFSET, &partitionGuid, sizeof(partitionGuid));
	PARTITION_PATH[TEST_GPT_MBR_TYPE_OFFSET] = MBR_TYPE_EFI_PARTITION_TABLE_HEADER;
	PARTITION_PATH[TEST_GPT_SIGNATURE_TYPE_OFFSET] = SIGNATURE_TYPE_GUID;
	EFI_DEVICE_PATH endNode = {};
	SetDevicePathEndNode(&endNode);
	memcpy(PARTITION_PATH + TEST_GPT_NODE_SIZE, &endNode, sizeof(endNode));
	PROTOCOL = { 2, MockVersion, MockVendor };
	LOCATED_PROTOCOL = &PROTOCOL;
	ROOT_BRIDGE = {};
	ROOT_BRIDGE.Pci.Read = MockProbeRead;
	ROOT_BRIDGE.Pci.Write = MockProbeWrite;
	GPU_COUNT = 3;
	for (UINTN index = 0; index < GPU_COUNT; ++index) {
		GPUS[index] = {};
		GPUS[index].Pci.Pci.Read = MockPciRead;
		GPUS[index].Pci.Pci.Write = MockPciWrite;
		GPUS[index].Pci.GetLocation = MockLocation;
	}
	GPUS[0].Id = TEST_INTEL_ID;
	GPUS[0].ClassRevision = TEST_INTEL_CLASS_REVISION;
	GPUS[0].Device = 2;
	GPUS[1].Id = 0x73601002;
	GPUS[1].ClassRevision = 0x030000c1;
	GPUS[1].Bus = 3;
	GPUS[2].Id = 0x12348086;
	GPUS[2].ClassRevision = 0x02000001;
	GPUS[2].Device = 1;
}

static UINTN CountOccurrences(const wchar_t* needle) {
	UINTN length = 0;
	UINTN matches = 0;
	while (needle[length] != 0) {
		++length;
	}
	for (UINTN index = 0; index + length <= CONTENT_LENGTH; ++index) {
		if (memcmp(CONTENTS + index, needle, length * sizeof(CHAR16)) == 0) {
			++matches;
		}
	}
	return matches;
}

static UINTN FindFirstOccurrence(const wchar_t* needle) {
	UINTN length = 0;
	while (needle[length] != 0) {
		++length;
	}
	for (UINTN index = 0; index + length <= CONTENT_LENGTH; ++index) {
		if (memcmp(CONTENTS + index, needle, length * sizeof(CHAR16)) == 0) {
			return index;
		}
	}
	return CONTENT_LENGTH;
}

static EFI_STATUS RunCase() {
	const EFI_STATUS status = RunAppleSetOs(&DRIVER_IMAGE, &SYSTEM_TABLE);
	if (FORBIDDEN_OPERATIONS != 0 || CONSOLE_OVERFLOW) {
		return EFI_ACCESS_DENIED;
	}
	return status;
}

static bool Check(bool condition, const char* expectation) {
	if (!condition) {
		fprintf(stderr, "%s\n", expectation);
		return false;
	}
	return true;
}

static bool CheckBothCallbacks(const char* expectation) {
	const bool argumentsValid = VENDOR_CALLS == 1 && VERSION_CALLS == 1;
	return Check(argumentsValid && CallbacksInVendorVersionOrder(), expectation);
}

static bool CheckNoDiagnosticReads(const char* expectation) {
	const bool noReads = HANDLE_PROTOCOL_CALLS == 0 && LOCATE_BUFFER_CALLS == 0 &&
		PCI_READS == 0 && PROBE_READS == 0 && TIME_READS == 0 && MONOTONIC_READS == 0;
	return Check(noReads && POOL_OPERATIONS == 0, expectation);
}

static bool CheckInitialNoConsole() {
	// A silent run must not initialize logging or query unused diagnostic metadata.
	ResetCase();
	SYSTEM_TABLE.ConOut = nullptr;
	const EFI_STATUS noConsoleStatus = RunCase();
	return Check(noConsoleStatus == EFI_SUCCESS, "first call without console: status preserved") &&
		CheckBothCallbacks("first call without console: callbacks and order preserved") &&
		CheckNoDiagnosticReads("first call without console: no diagnostic firmware access") &&
		Check(PROTOCOL_LOCATE_COUNT == 1, "first call without console: Apple protocol still located") &&
		Check(CONTENT_LENGTH == 0, "first call without console: no output");
}

static bool CheckInitialOptionalServices() {
	// This must run before GNU-EFI initializes and caches the system table.
	ResetCase();
	SYSTEM_TABLE.RuntimeServices = nullptr;
	SERVICES.GetNextMonotonicCount = nullptr;
	const EFI_STATUS optionalServicesStatus = RunCase();
	return Check(optionalServicesStatus == EFI_SUCCESS, "first init without optional services: status preserved") &&
		Check(
			CallbacksInVendorVersionOrder(),
			"first init without optional services: callback order preserved");
}

static bool CheckSuccessfulTrace() {
	ResetCase();
	const EFI_STATUS normalStatus = RunCase();
	if (!Check(normalStatus == EFI_SUCCESS, "normal run: successful status")) {
		return false;
	}
	const bool vendorLogged = CountOccurrences(L"[vendor] call argument=\"Apple Inc.\"") == 1 &&
		CountOccurrences(L"[vendor] success") == 1;
	const bool versionLogged = CountOccurrences(L"[version] call argument=\"Mac OS X 10.9\"") == 1 &&
		CountOccurrences(L"[version] success") == 1;
	const bool obsoleteDiagnosticsAbsent = CountOccurrences(L"[observer]") == 0 &&
		CountOccurrences(L"[windows.image]") == 0 && CountOccurrences(L"[guard]") == 0 &&
		CountOccurrences(L"[log]") == 0;
	const bool oldMessagesAbsent = CountOccurrences(L"apple_set_os started") == 0 &&
		CountOccurrences(L"Set os vendor to Apple Inc.") == 0 &&
		CountOccurrences(L"Set os version to Mac OS X 10.9.") == 0;
	const UINTN runPosition = FindFirstOccurrence(L"[run]");
	const UINTN gpuBeforePosition = FindFirstOccurrence(L"[gpu.before]");
	const UINTN protocolPosition = FindFirstOccurrence(L"[protocol]");
	const UINTN vendorPosition = FindFirstOccurrence(L"[vendor] call");
	const UINTN versionPosition = FindFirstOccurrence(L"[version] call");
	const UINTN gpuAfterPosition = FindFirstOccurrence(L"[gpu.after]");
	const bool logOrderMatches = runPosition < gpuBeforePosition &&
		gpuBeforePosition < protocolPosition && protocolPosition < vendorPosition &&
		vendorPosition < versionPosition && versionPosition < gpuAfterPosition;

	return Check(PROTOCOL_LOCATE_COUNT == 1, "normal run: locate Apple protocol exactly once") &&
		Check(CallbacksInVendorVersionOrder(), "normal run: vendor precedes version") &&
		Check(PROBE_READS == 0, "normal run: known Intel location needs no fallback probe") &&
		Check(vendorLogged, "normal run: vendor argument and success logged exactly once") &&
		Check(versionLogged, "normal run: version argument and success logged exactly once") &&
		Check(CountOccurrences(L"[protocol]") == 1, "normal run: one protocol record") &&
		Check(CountOccurrences(L"[entry]") == 0, "normal run: no successful entry noise") &&
		Check(obsoleteDiagnosticsAbsent, "normal run: no observer, marker, or file logger records") &&
		Check(oldMessagesAbsent, "normal run: no duplicate startup or Set OS messages") &&
		Check(logOrderMatches, "normal run: environment, GPU before, callbacks, GPU after order") &&
		Check(CountOccurrences(L"[run] id=") == 1, "normal run: run identity logged exactly once") &&
		Check(
			CountOccurrences(L"[image] arch=x64 type=boot-services-driver") > 0,
			"normal run: PE machine and driver subsystem decoded") &&
		Check(
			CountOccurrences(L"[image] partition=12345678-1234-5678-90AB-CDEF12345678") == 1,
			"normal run: GUID decoded from the 42-byte GPT wire node") &&
		Check(CountOccurrences(L"esp=") == 0, "normal run: source partition is not assumed to be an ESP") &&
		Check(
			CountOccurrences(L"\\EFI\\custom\\apple_set_os.efi") > 0,
			"normal run: original generic image path logged") &&
		Check(
			CountOccurrences(L"[firmware] vendor=Apple revision=0x00001234") == 1,
			"normal run: firmware vendor and revision logged exactly once") &&
		Check(CountOccurrences(L"[gpu.before] count=2") == 1, "normal run: two GPUs before callbacks") &&
		Check(CountOccurrences(L"[gpu.after] count=2") == 1, "normal run: two GPUs after callbacks") &&
		Check(
			CountOccurrences(L"pci=0000:00:02.0 vendor=Intel id=8086:3E9B") == 2,
			"normal run: Intel identity and location in both snapshots") &&
		Check(
			CountOccurrences(L"pci=0000:03:00.0 vendor=AMD id=1002:7360") == 2,
			"normal run: AMD identity and location in both snapshots") &&
		Check(CountOccurrences(L"id=8086:1234") == 0, "normal run: network device excluded") &&
		Check(CountOccurrences(L"\r\r\n") == 0, "normal run: no doubled carriage returns") &&
		Check(CountOccurrences(L"\r\n") > 0, "normal run: EFI line endings present") &&
		Check(
			memcmp(CONTENTS, L"[run]", sizeof(L"[run]") - sizeof(wchar_t)) == 0,
			"normal run: first record is run identity, with no BOM or blank line");
}

static bool CheckApplicationMetadata() {
	ResetCase();
	StoreValue<UINT16>(PE_IMAGE, TEST_PE_SUBSYSTEM_OFFSET, TEST_SUBSYSTEM_APPLICATION);
	const EFI_STATUS applicationStatus = RunCase();
	return Check(applicationStatus == EFI_SUCCESS, "application image: status preserved") &&
		Check(CallbacksInVendorVersionOrder(), "application image: callback order preserved") &&
		Check(
			CountOccurrences(L"[image] arch=x64 type=application") == 1,
			"application image: subsystem is read dynamically");
}

static bool CheckRawPeMetadata() {
	ResetCase();
	const UINTN peSize = sizeof(PE_IMAGE) - TEST_PE_HEADER_OFFSET;
	memmove(PE_IMAGE, PE_IMAGE + TEST_PE_HEADER_OFFSET, peSize);
	DRIVER_IMAGE.ImageSize = peSize;
	NDebug::LogLoadedImage(&DRIVER_IMAGE, &SYSTEM_TABLE);
	return Check(
		CountOccurrences(L"[image] arch=x64 type=boot-services-driver") == 1,
		"raw PE image: DOS stub is optional") &&
		Check(CountOccurrences(L"reason=invalid-pe") == 0, "raw PE image: valid headers accepted") &&
		// Two metadata records, then the path prefix, payload, and newline.
		Check(OUTPUT_CALLS <= 5, "ordinary image path: no per-character console calls");
}

static bool CheckRunIdentityFallbacks() {
	ResetCase();
	SERVICES.GetNextMonotonicCount = nullptr;
	const EFI_STATUS timeOnlyStatus = RunCase();
	if (!Check(timeOnlyStatus == EFI_SUCCESS, "time-only identity: status preserved") ||
		!Check(CountOccurrences(L"[run] id=time-20261010-010203") == 1, "time-only identity: available time retained") ||
		!Check(CountOccurrences(L"counter_status=Unsupported") == 1, "time-only identity: missing counter reported"))
	{
		return false;
	}

	ResetCase();
	RUNTIME.GetTime = nullptr;
	const EFI_STATUS counterOnlyStatus = RunCase();
	if (!Check(counterOnlyStatus == EFI_SUCCESS, "counter-only identity: status preserved") ||
		!Check(CountOccurrences(L"[run] id=counter-") == 1, "counter-only identity: available counter retained") ||
		!Check(CountOccurrences(L"time_status=Unsupported") == 1, "counter-only identity: missing time reported"))
	{
		return false;
	}

	ResetCase();
	RUNTIME.GetTime = nullptr;
	SERVICES.GetNextMonotonicCount = nullptr;
	const EFI_STATUS unknownIdentityStatus = RunCase();
	return Check(unknownIdentityStatus == EFI_SUCCESS, "unknown identity: status preserved") &&
		Check(CountOccurrences(L"[run] id=unknown") == 1, "unknown identity: neither source available") &&
		Check(
			CountOccurrences(L"time_status=Unsupported counter_status=Unsupported") == 1,
			"unknown identity: both missing sources reported");
}

template<UINTN size>
static UINTN AppendFilePath(
	UINT8* bytes,
	UINTN offset,
	const wchar_t (&path)[size])
{
	EFI_DEVICE_PATH node = {};
	node.Type = MEDIA_DEVICE_PATH;
	node.SubType = MEDIA_FILEPATH_DP;
	const UINTN nodeSize = sizeof(node) + sizeof(path);
	SetDevicePathNodeLength(&node, nodeSize);
	memcpy(bytes + offset, &node, sizeof(node));
	memcpy(bytes + offset + sizeof(node), path, sizeof(path));
	return offset + nodeSize;
}

template<UINTN directorySize, UINTN filenameSize>
static bool CheckCompositeImagePath(
	const wchar_t (&directory)[directorySize],
	const wchar_t (&filename)[filenameSize])
{
	ResetCase();
	alignas(CHAR16) UINT8 path[3 * sizeof(EFI_DEVICE_PATH) + sizeof(directory) + sizeof(filename)] = {};
	const UINTN filenameOffset = AppendFilePath(path, 0, directory);
	const UINTN endOffset = AppendFilePath(path, filenameOffset, filename);
	EFI_DEVICE_PATH endNode = {};
	SetDevicePathEndNode(&endNode);
	memcpy(path + endOffset, &endNode, sizeof(endNode));
	DRIVER_IMAGE.FilePath = reinterpret_cast<EFI_DEVICE_PATH*>(path);
	NDebug::LogLoadedImage(&DRIVER_IMAGE, &SYSTEM_TABLE);
	return Check(
		CountOccurrences(L"[image] path=\\EFI\\custom\\apple_set_os.efi\r\n") == 1,
		"composite image path: directory and filename joined with one separator") &&
		Check(CountOccurrences(L"[image] path=unknown") == 0, "composite image path: accepted") &&
		// The two fragments and an optional separator add at most two output calls.
		Check(OUTPUT_CALLS <= 7, "composite image path: fragments logged in batches") &&
		Check(POOL_OPERATIONS == 0, "composite image path: no runtime pool copies") &&
		Check(FORBIDDEN_OPERATIONS == 0 && !CONSOLE_OVERFLOW, "composite image path: console-only output");
}

static bool CheckCompositeImagePaths() {
	return CheckCompositeImagePath(L"\\EFI\\custom", L"apple_set_os.efi") &&
		CheckCompositeImagePath(L"\\EFI\\custom\\", L"apple_set_os.efi") &&
		CheckCompositeImagePath(L"\\EFI\\custom", L"\\apple_set_os.efi") &&
		CheckCompositeImagePath(L"\\EFI\\custom\\", L"\\apple_set_os.efi") &&
		CheckCompositeImagePath(L"\\EFI\\custom\\\\", L"\\\\apple_set_os.efi");
}

static bool CheckUnalignedImagePath() {
	ResetCase();
	// A vendor node with one data byte puts the following UTF-16 payload at an odd offset.
	const UINTN vendorNodeSize = sizeof(EFI_DEVICE_PATH) + sizeof(EFI_GUID) + sizeof(UINT8);
	alignas(CHAR16) UINT8 path[vendorNodeSize + 2 * sizeof(EFI_DEVICE_PATH) + sizeof(L"\\EFI\\custom\\apple_set_os.efi")] = {};
	EFI_DEVICE_PATH vendorNode = {};
	vendorNode.Type = HARDWARE_DEVICE_PATH;
	vendorNode.SubType = HW_VENDOR_DP;
	SetDevicePathNodeLength(&vendorNode, vendorNodeSize);
	memcpy(path, &vendorNode, sizeof(vendorNode));
	const UINTN endOffset = AppendFilePath(path, vendorNodeSize, L"\\EFI\\custom\\apple_set_os.efi");
	EFI_DEVICE_PATH endNode = {};
	SetDevicePathEndNode(&endNode);
	memcpy(path + endOffset, &endNode, sizeof(endNode));
	DRIVER_IMAGE.FilePath = reinterpret_cast<EFI_DEVICE_PATH*>(path);
	NDebug::LogLoadedImage(&DRIVER_IMAGE, &SYSTEM_TABLE);
	return Check(
		CountOccurrences(L"[image] path=\\EFI\\custom\\apple_set_os.efi\r\n") == 1,
		"unaligned FilePath payload: UTF-16 characters decoded without aligned loads") &&
		Check(OUTPUT_CALLS <= 5, "unaligned short image path: characters logged in one batch") &&
		Check(POOL_OPERATIONS == 0, "unaligned image path: no runtime pool copies") &&
		Check(!CONSOLE_OVERFLOW, "unaligned image path: output remains bounded");
}

static bool CheckLongImagePath() {
	ResetCase();
	// Exceed GNU-EFI's formatting buffer to exercise streaming without truncation.
	constexpr UINTN pathLength = 512;
	static constexpr wchar_t PATH_PREFIX[] = L"\\EFI\\";
	constexpr UINTN prefixLength = sizeof(PATH_PREFIX) / sizeof(wchar_t) - 1;
	wchar_t pathText[pathLength + 1] = {};
	memcpy(pathText, PATH_PREFIX, prefixLength * sizeof(wchar_t));
	for (UINTN index = prefixLength; index < pathLength; ++index) {
		pathText[index] = L'a';
	}
	alignas(CHAR16) UINT8 path[2 * sizeof(EFI_DEVICE_PATH) + sizeof(pathText)] = {};
	const UINTN endOffset = AppendFilePath(path, 0, pathText);
	EFI_DEVICE_PATH endNode = {};
	SetDevicePathEndNode(&endNode);
	memcpy(path + endOffset, &endNode, sizeof(endNode));
	DRIVER_IMAGE.FilePath = reinterpret_cast<EFI_DEVICE_PATH*>(path);
	NDebug::LogLoadedImage(&DRIVER_IMAGE, &SYSTEM_TABLE);
	return Check(CountOccurrences(pathText) == 1, "long image path: entire payload logged") &&
		Check(CountOccurrences(L"[image] path=") == 1, "long image path: one path record") &&
		// Permit formatter flushes for the long payload, not one call per character.
		Check(OUTPUT_CALLS <= 8, "long image path: bounded console call count") &&
		Check(POOL_OPERATIONS == 0, "long image path: no runtime pool copies") &&
		Check(!CONSOLE_OVERFLOW, "long image path: output remains bounded");
}

static bool CheckLongUnalignedCompositeImagePath() {
	ResetCase();
	constexpr UINTN directoryLength = 256;
	constexpr UINTN filenameLength = 160;
	wchar_t directory[directoryLength + 1] = {};
	wchar_t filename[filenameLength + 1] = {};
	for (UINTN index = 0; index < directoryLength; ++index) {
		directory[index] = L'a';
	}
	for (UINTN index = 0; index < filenameLength; ++index) {
		filename[index] = L'b';
	}
	wchar_t expectedPath[directoryLength + 1 + filenameLength + 1] = {};
	memcpy(expectedPath, directory, directoryLength * sizeof(wchar_t));
	expectedPath[directoryLength] = L'\\';
	memcpy(expectedPath + directoryLength + 1, filename, sizeof(filename));

	// An odd-sized vendor node leaves both FilePath payloads unaligned.
	const UINTN vendorNodeSize = sizeof(EFI_DEVICE_PATH) + sizeof(EFI_GUID) + sizeof(UINT8);
	alignas(CHAR16) UINT8 path[vendorNodeSize + 3 * sizeof(EFI_DEVICE_PATH) + sizeof(directory) + sizeof(filename)] = {};
	EFI_DEVICE_PATH vendorNode = {};
	vendorNode.Type = HARDWARE_DEVICE_PATH;
	vendorNode.SubType = HW_VENDOR_DP;
	SetDevicePathNodeLength(&vendorNode, vendorNodeSize);
	memcpy(path, &vendorNode, sizeof(vendorNode));
	const UINTN filenameOffset = AppendFilePath(path, vendorNodeSize, directory);
	const UINTN endOffset = AppendFilePath(path, filenameOffset, filename);
	EFI_DEVICE_PATH endNode = {};
	SetDevicePathEndNode(&endNode);
	memcpy(path + endOffset, &endNode, sizeof(endNode));
	DRIVER_IMAGE.FilePath = reinterpret_cast<EFI_DEVICE_PATH*>(path);
	NDebug::LogLoadedImage(&DRIVER_IMAGE, &SYSTEM_TABLE);
	return Check(CountOccurrences(expectedPath) == 1, "long unaligned composite path: full joined payload logged") &&
		Check(CountOccurrences(L"[image] path=") == 1, "long unaligned composite path: one path record") &&
		// Two metadata records, prefix/separator/newline, and at most five chunks.
		Check(OUTPUT_CALLS <= 10, "long unaligned composite path: bounded console call count") &&
		Check(POOL_OPERATIONS == 0, "long unaligned composite path: no runtime pool copies") &&
		Check(!CONSOLE_OVERFLOW, "long unaligned composite path: output remains bounded");
}

static bool CheckInvalidImagePaths() {
	ResetCase();
	alignas(CHAR16) UINT8 path[3 * sizeof(EFI_DEVICE_PATH) + sizeof(L"\\EFI\\custom") + sizeof(L"apple_set_os.efi")] = {};
	const UINTN filenameOffset = AppendFilePath(path, 0, L"\\EFI\\custom");
	const UINTN endOffset = AppendFilePath(path, filenameOffset, L"apple_set_os.efi");
	EFI_DEVICE_PATH endNode = {};
	SetDevicePathEndNode(&endNode);
	memcpy(path + endOffset, &endNode, sizeof(endNode));
	// The first FilePath is valid; the later node is too short for its header.
	StoreValue<UINT16>(path, filenameOffset + offsetof(EFI_DEVICE_PATH, Length), 2);
	DRIVER_IMAGE.FilePath = reinterpret_cast<EFI_DEVICE_PATH*>(path);
	NDebug::LogLoadedImage(&DRIVER_IMAGE, &SYSTEM_TABLE);
	if (!Check(CountOccurrences(L"[image] path=unknown") == 1, "invalid later node: path rejected") ||
		!Check(CountOccurrences(L"[image] path=\\") == 0, "invalid later node: no partial success output"))
	{
		return false;
	}

	ResetCase();
	AppendFilePath(path, filenameOffset, L"apple_set_os.efi");
	StoreValue<CHAR16>(path, endOffset - sizeof(CHAR16), 'X');
	DRIVER_IMAGE.FilePath = reinterpret_cast<EFI_DEVICE_PATH*>(path);
	NDebug::LogLoadedImage(&DRIVER_IMAGE, &SYSTEM_TABLE);
	if (!Check(CountOccurrences(L"[image] path=unknown") == 1, "unterminated later fragment: path rejected") ||
		!Check(CountOccurrences(L"[image] path=\\") == 0, "unterminated later fragment: no partial success output"))
	{
		return false;
	}

	ResetCase();
	for (CHAR16& character : IMAGE_PATH.Path) {
		character = 'X';
	}
	NDebug::LogLoadedImage(&DRIVER_IMAGE, &SYSTEM_TABLE);
	if (!Check(CountOccurrences(L"[image] path=unknown") == 1, "unterminated FilePath: path rejected") ||
		!Check(CountOccurrences(L"[image] path=\\") == 0, "unterminated FilePath: no partial success output"))
	{
		return false;
	}

	ResetCase();
	SetDevicePathNodeLength(&IMAGE_PATH.End, 8);
	NDebug::LogLoadedImage(&DRIVER_IMAGE, &SYSTEM_TABLE);
	return Check(CountOccurrences(L"[image] path=unknown") == 1, "invalid END length: path rejected") &&
		Check(CountOccurrences(L"[image] path=\\") == 0, "invalid END length: no partial success output") &&
		Check(POOL_OPERATIONS == 0, "invalid image path: no runtime pool copies");
}

static bool CheckMultipleImagePathInstances() {
	ResetCase();
	alignas(CHAR16) UINT8 path[4 * sizeof(EFI_DEVICE_PATH) + sizeof(L"\\EFI\\first.efi") + sizeof(L"\\EFI\\second.efi")] = {};
	const UINTN firstEndOffset = AppendFilePath(path, 0, L"\\EFI\\first.efi");
	EFI_DEVICE_PATH instanceEnd = {};
	SetDevicePathEndNode(&instanceEnd);
	instanceEnd.SubType = END_INSTANCE_DEVICE_PATH_SUBTYPE;
	memcpy(path + firstEndOffset, &instanceEnd, sizeof(instanceEnd));
	const UINTN secondStart = firstEndOffset + sizeof(instanceEnd);
	const UINTN finalEndOffset = AppendFilePath(path, secondStart, L"\\EFI\\second.efi");
	EFI_DEVICE_PATH finalEnd = {};
	SetDevicePathEndNode(&finalEnd);
	memcpy(path + finalEndOffset, &finalEnd, sizeof(finalEnd));
	DRIVER_IMAGE.FilePath = reinterpret_cast<EFI_DEVICE_PATH*>(path);
	NDebug::LogLoadedImage(&DRIVER_IMAGE, &SYSTEM_TABLE);
	return Check(
		CountOccurrences(L"[image] path=unknown reason=multiple-device-path-instances") == 1,
		"multiple device-path instances: unsupported representation reported explicitly") &&
		Check(CountOccurrences(L"[image] path=\\") == 0, "multiple device-path instances: no partial success output") &&
		Check(POOL_OPERATIONS == 0, "multiple device-path instances: no runtime pool copies");
}

static bool CheckProtocolFailures() {
	ResetCase();
	LOCATE_STATUS = EFI_NOT_FOUND;
	const EFI_STATUS missingProtocolStatus = RunCase();
	if (!Check(missingProtocolStatus == EFI_NOT_FOUND, "missing protocol: preserve locate status") ||
		!Check(VENDOR_CALLS == 0 && VERSION_CALLS == 0, "missing protocol: no firmware setters") ||
		!Check(CountOccurrences(L"pointer=null") > 0, "missing protocol: null pointer logged") ||
		!Check(CountOccurrences(L"[entry] error=") > 0, "missing protocol: entry error logged") ||
		!Check(CountOccurrences(L"[gpu.after] count=2") > 0, "missing protocol: final GPU snapshot remains"))
	{
		return false;
	}

	ResetCase();
	LOCATE_RETURNS_NULL = 1;
	const EFI_STATUS nullProtocolStatus = RunCase();
	if (!Check(nullProtocolStatus == EFI_NOT_FOUND, "successful lookup with null pointer: normalize to not found") ||
		!Check(PROTOCOL_LOCATE_COUNT == 1, "null protocol: one lookup") ||
		!Check(VENDOR_CALLS == 0 && VERSION_CALLS == 0, "null protocol: no firmware setters") ||
		!Check(CountOccurrences(L"lookup=Success") > 0, "null protocol: raw successful lookup preserved") ||
		!Check(CountOccurrences(L"pointer=null") > 0, "null protocol: null pointer logged"))
	{
		return false;
	}

	ResetCase();
	LOCATE_STATUS = EFI_DEVICE_ERROR;
	LOCATE_FAIL_RETURNS_POINTER = 1;
	// A one-byte object is not a protocol; ASan catches any attempted field read.
	UINT8 invalidProtocol = 0;
	LOCATED_PROTOCOL = &invalidProtocol;
	const EFI_STATUS invalidProtocolStatus = RunCase();
	return Check(invalidProtocolStatus == EFI_DEVICE_ERROR, "failed lookup with invalid pointer: preserve status") &&
		Check(PROTOCOL_LOCATE_COUNT == 1, "failed lookup with invalid pointer: one lookup") &&
		Check(VENDOR_CALLS == 0 && VERSION_CALLS == 0, "failed lookup with invalid pointer: no setters") &&
		Check(CountOccurrences(L"pointer=present") > 0, "failed lookup: raw pointer presence logged") &&
		Check(
			CountOccurrences(L"vendor=unavailable version=unavailable") > 0,
			"failed lookup: callback availability never inferred from invalid pointer");
}

static bool CheckConsoleFailures() {
	enum class EFailure {
		MissingConsole,
		MissingOutputCallback,
		OutputError,
		MissingMode
	};
	const EFailure cases[] = {
		EFailure::MissingConsole, EFailure::MissingOutputCallback,
		EFailure::OutputError, EFailure::MissingMode
	};
	for (EFailure failure : cases) {
		ResetCase();
		switch (failure) {
		case EFailure::MissingConsole:
			SYSTEM_TABLE.ConOut = nullptr;
			break;
		case EFailure::MissingOutputCallback:
			CONSOLE.OutputString = nullptr;
			break;
		case EFailure::OutputError:
			OUTPUT_ERROR = EFI_DEVICE_ERROR;
			break;
		case EFailure::MissingMode:
			CONSOLE.Mode = nullptr;
			break;
		}
		const EFI_STATUS consoleFailureStatus = RunCase();
		if (!Check(consoleFailureStatus == EFI_SUCCESS, "console failure: status unchanged") ||
			!CheckBothCallbacks("console failure: both setters and their order unchanged"))
		{
			fprintf(stderr, "console failure case: %d\n", static_cast<int>(failure));
			return false;
		}
		if (failure != EFailure::OutputError) {
			if (!CheckNoDiagnosticReads("unusable console: no diagnostic firmware access") ||
				!Check(OUTPUT_CALLS == 0, "unusable console: no output attempts") ||
				!Check(PROTOCOL_LOCATE_COUNT == 1, "unusable console: Apple protocol still located"))
			{
				return false;
			}
		}
	}
	return true;
}

static bool CheckOptionalMetadataServices() {
	enum class EFailure {
		LoadedImageError,
		NullLoadedImage,
		MissingImageDevice,
		MissingHandleProtocol,
		MissingLocateHandleBuffer,
		MissingFirmwareMetadata
	};
	const EFailure cases[] = {
		EFailure::LoadedImageError, EFailure::NullLoadedImage, EFailure::MissingImageDevice,
		EFailure::MissingHandleProtocol, EFailure::MissingLocateHandleBuffer,
		EFailure::MissingFirmwareMetadata
	};
	for (EFailure failure : cases) {
		ResetCase();
		switch (failure) {
		case EFailure::LoadedImageError:
			HANDLE_ERROR = EFI_DEVICE_ERROR;
			break;
		case EFailure::NullLoadedImage:
			HANDLE_RETURNS_NULL = 1;
			break;
		case EFailure::MissingImageDevice:
			DRIVER_IMAGE.DeviceHandle = nullptr;
			break;
		case EFailure::MissingHandleProtocol:
			SERVICES.HandleProtocol = nullptr;
			break;
		case EFailure::MissingLocateHandleBuffer:
			SERVICES.LocateHandleBuffer = nullptr;
			break;
		case EFailure::MissingFirmwareMetadata:
			SYSTEM_TABLE.RuntimeServices = nullptr;
			SERVICES.GetNextMonotonicCount = nullptr;
			SYSTEM_TABLE.FirmwareVendor = nullptr;
			break;
		}
		const EFI_STATUS optionalMetadataStatus = RunCase();
		const UINTN runPosition = FindFirstOccurrence(L"[run]");
		const UINTN imagePosition = FindFirstOccurrence(L"[image]");
		if (!Check(optionalMetadataStatus == EFI_SUCCESS, "optional metadata unavailable: status unchanged") ||
			!CheckBothCallbacks("optional metadata unavailable: both setters and order unchanged") ||
			!Check(runPosition < imagePosition, "optional metadata unavailable: run record precedes image result") ||
			!Check(
				CountOccurrences(L"[protocol]") > 0,
				"optional metadata unavailable: protocol still logged"))
		{
			fprintf(stderr, "metadata failure case: %d\n", static_cast<int>(failure));
			return false;
		}
	}
	return true;
}

static bool CheckProtocolRevisionPrefixes() {
	// Exact-size prefixes let ASan detect reads of unsupported callback fields.
	struct TRevisionZero {
		UINTN Revision;
	};
	struct TRevisionOne {
		UINTN Revision;
		void (EFIAPI* SetOsVersion)(char* version);
	};
	ResetCase();
	TRevisionZero revisionZero = { 0 };
	LOCATED_PROTOCOL = &revisionZero;
	const EFI_STATUS revisionZeroStatus = RunCase();
	if (!Check(revisionZeroStatus == EFI_SUCCESS, "revision 0: status preserved") ||
		!Check(VENDOR_CALLS == 0 && VERSION_CALLS == 0, "revision 0: no setters") ||
		!Check(
			CountOccurrences(L"[vendor] skipped reason=unsupported_revision") > 0,
			"revision 0: vendor skip reason logged") ||
		!Check(
			CountOccurrences(L"[version] skipped reason=unsupported_revision") > 0,
			"revision 0: version skip reason logged"))
	{
		return false;
	}

	ResetCase();
	TRevisionOne revisionOne = { 1, MockVersion };
	LOCATED_PROTOCOL = &revisionOne;
	const EFI_STATUS revisionOneStatus = RunCase();
	if (!Check(revisionOneStatus == EFI_SUCCESS, "revision 1: status preserved") ||
		!Check(VENDOR_CALLS == 0 && VERSION_CALLS == 1, "revision 1: only version callback") ||
		!Check(
			CountOccurrences(L"[vendor] skipped reason=unsupported_revision") > 0,
			"revision 1: vendor skip reason logged"))
	{
		return false;
	}

	ResetCase();
	PROTOCOL.SetOsVendor = nullptr;
	PROTOCOL.SetOsVersion = nullptr;
	const EFI_STATUS nullCallbacksStatus = RunCase();
	return Check(nullCallbacksStatus == EFI_SUCCESS, "null callbacks: status preserved") &&
		Check(VENDOR_CALLS == 0 && VERSION_CALLS == 0, "null callbacks: no setters") &&
		Check(
			CountOccurrences(L"[vendor] skipped reason=null_callback") > 0,
			"null vendor callback: skip logged") &&
		Check(
			CountOccurrences(L"[version] skipped reason=null_callback") > 0,
			"null version callback: skip logged");
}

static bool CheckMalformedMetadata() {
	enum class EFailure {
		SmallImage,
		InvalidPePointer,
		MissingImagePath,
		ShortPathHeaders,
		EmptyFilePath
	};
	const EFailure cases[] = {
		EFailure::SmallImage, EFailure::InvalidPePointer, EFailure::MissingImagePath,
		EFailure::ShortPathHeaders, EFailure::EmptyFilePath
	};
	for (EFailure failure : cases) {
		ResetCase();
		const wchar_t* expectedReason = nullptr;
		switch (failure) {
		case EFailure::SmallImage:
			DRIVER_IMAGE.ImageSize = 1;
			expectedReason = L"reason=invalid-pe";
			break;
		case EFailure::InvalidPePointer:
			StoreValue<UINT32>(PE_IMAGE, TEST_DOS_PE_POINTER_OFFSET, 0xfffffff0);
			expectedReason = L"reason=invalid-pe";
			break;
		case EFailure::MissingImagePath:
			DRIVER_IMAGE.FilePath = nullptr;
			expectedReason = L"reason=missing-device-path";
			break;
		case EFailure::ShortPathHeaders:
			// A two-byte node is shorter than the mandatory four-byte header.
			SetDevicePathNodeLength(&IMAGE_PATH.File, 2);
			StoreValue<UINT16>(PARTITION_PATH, offsetof(EFI_DEVICE_PATH, Length), 2);
			expectedReason = L"reason=invalid-device-path";
			break;
		case EFailure::EmptyFilePath: {
			// Empty FilePath node followed by a valid END node: no text payload.
			SetDevicePathNodeLength(&IMAGE_PATH.File, sizeof(EFI_DEVICE_PATH));
			EFI_DEVICE_PATH endNode = {};
			SetDevicePathEndNode(&endNode);
			memcpy(IMAGE_PATH.Path, &endNode, sizeof(endNode));
			expectedReason = L"reason=invalid-filepath-node";
			break;
		}
		}
		const EFI_STATUS status = RunCase();
		const UINTN reasonCount = CountOccurrences(expectedReason);
		bool reasonLogged = reasonCount > 0;
		if (failure == EFailure::ShortPathHeaders) {
			reasonLogged = reasonCount == 2;
		}
		if (!Check(status == EFI_SUCCESS, "malformed metadata: status unchanged") ||
			!Check(CallbacksInVendorVersionOrder(), "malformed metadata: callback order unchanged") ||
			!Check(reasonLogged, "malformed metadata: rejection reason logged"))
		{
			fprintf(stderr, "malformed metadata case: %d\n", static_cast<int>(failure));
			return false;
		}
	}
	return true;
}

static bool CheckGpuFailuresAndFallback() {
	ResetCase();
	GPU_COUNT = 0;
	const EFI_STATUS emptyGpuListStatus = RunCase();
	if (!Check(emptyGpuListStatus == EFI_SUCCESS, "empty GPU list: status unchanged") ||
		!Check(PROBE_READS == 4, "empty GPU list: ID and class fallback reads in both snapshots") ||
		!Check(CountOccurrences(L"[gpu.before] count=0") > 0, "empty GPU list: count logged"))
	{
		return false;
	}

	ResetCase();
	GPU_LOCATE_ERROR = EFI_DEVICE_ERROR;
	PROBE_ERROR = EFI_DEVICE_ERROR;
	const EFI_STATUS pciFailureStatus = RunCase();
	if (!Check(pciFailureStatus == EFI_SUCCESS, "PCI locate/read errors: status unchanged") ||
		!Check(CallbacksInVendorVersionOrder(), "PCI locate/read errors: callback order unchanged") ||
		!Check(PROBE_READS > 0, "PCI locate/read errors: fallback attempted") ||
		!Check(CountOccurrences(L"presence=unknown") > 0, "PCI locate/read errors: never claim absence"))
	{
		return false;
	}

	ResetCase();
	GPUS[0].ReadError = EFI_DEVICE_ERROR;
	GPUS[1].LocationError = EFI_DEVICE_ERROR;
	const EFI_STATUS gpuFailureStatus = RunCase();
	if (!Check(gpuFailureStatus == EFI_SUCCESS, "GPU class/location errors: status unchanged") ||
		!Check(CallbacksInVendorVersionOrder(), "GPU class/location errors: callback order unchanged") ||
		!Check(PROBE_READS > 0, "GPU class/location errors: unknown Intel location needs fallback") ||
		!Check(
			CountOccurrences(L"presence=unknown class_read=Device Error") > 0,
			"GPU class read: error logged") ||
		!Check(
			CountOccurrences(L"pci=unknown location_status=Device Error") > 0,
			"GPU location read: error logged") ||
		!Check(
			CountOccurrences(L"count=1 source=pci-io incomplete=1") > 0,
			"GPU read errors: incomplete count logged"))
	{
		return false;
	}

	ResetCase();
	GPUS[0].Segment = 1;
	GPUS[0].Bus = 7;
	const EFI_STATUS relocatedIntelStatus = RunCase();
	return Check(relocatedIntelStatus == EFI_SUCCESS, "Intel outside known IGD location: status unchanged") &&
		Check(PROBE_READS == 4, "Intel outside known IGD location: still probe the known IGD address") &&
		Check(
			CountOccurrences(L"pci=0001:07:02.0 vendor=Intel id=8086:3E9B") == 2,
			"Intel outside known IGD location: full segment and bus logged in both snapshots");
}

int main() {
	if (!CheckInitialNoConsole() ||
		!CheckInitialOptionalServices() ||
		!CheckSuccessfulTrace() ||
		!CheckApplicationMetadata() ||
		!CheckRawPeMetadata() ||
		!CheckRunIdentityFallbacks() ||
		!CheckCompositeImagePaths() ||
		!CheckUnalignedImagePath() ||
		!CheckLongImagePath() ||
		!CheckLongUnalignedCompositeImagePath() ||
		!CheckInvalidImagePaths() ||
		!CheckMultipleImagePathInstances() ||
		!CheckProtocolFailures() ||
		!CheckConsoleFailures() ||
		!CheckOptionalMetadataServices() ||
		!CheckProtocolRevisionPrefixes() ||
		!CheckMalformedMetadata() ||
		!CheckGpuFailuresAndFallback())
	{
		return 1;
	}
	puts("debug tests passed");
	return 0;
}
