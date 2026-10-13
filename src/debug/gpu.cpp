#include <debug/gpu.h>
#include <debug/protocol_lookup.h>
#include <logging/logging.h>

extern "C" {
#include <efi.h>
#include <efipciio.h>
}

namespace NDebug {

static EFI_GUID PCI_IO_GUID = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID PCI_ROOT_BRIDGE_IO_GUID = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;

// PCI configuration space: vendor/device IDs at 0, revision/class bytes at 8.
static constexpr uint32_t PCI_ID_OFFSET = 0;
static constexpr uint32_t PCI_CLASS_REVISION_OFFSET = 8;
static constexpr uint32_t PCI_DISPLAY_CLASS = 0x03;
static constexpr uint32_t PCI_VENDOR_INTEL = 0x8086;
static constexpr uint32_t PCI_VENDOR_AMD = 0x1002;
static constexpr uint32_t PCI_VENDOR_NVIDIA = 0x10de;
static constexpr uint32_t PCI_ABSENT_VENDOR = 0xffff;

struct TPciLocation {
	UINTN Segment = 0;
	UINTN Bus = 0;
	UINTN Device = 0;
	UINTN Function = 0;
};

static const CHAR16* AsEfiText(const wchar_t* text) {
	return reinterpret_cast<const CHAR16*>(text);
}

static const wchar_t* GetGpuVendorName(uint32_t vendor) {
	switch (vendor) {
	case PCI_VENDOR_INTEL: return L"Intel";
	case PCI_VENDOR_AMD: return L"AMD";
	case PCI_VENDOR_NVIDIA: return L"NVIDIA";
	default: return L"Unknown";
	}
}

static EFI_STATUS LocatePciHandles(
	EFI_SYSTEM_TABLE* systemTable,
	EFI_GUID* guid,
	UINTN* count,
	EFI_HANDLE** handles)
{
	const auto* bootServices = systemTable->BootServices;
	const EFI_STATUS status = bootServices->LocateHandleBuffer(
		/* searchType */ ByProtocol,
		/* protocol */ guid,
		/* searchKey */ nullptr,
		/* handleCount */ count,
		/* handles */ handles);
	if (status != EFI_SUCCESS) {
		return status;
	}
	if (*count > 0 && *handles == nullptr) {
		return EFI_DEVICE_ERROR;
	}
	return EFI_SUCCESS;
}

static void LogIgdProbe(EFI_SYSTEM_TABLE* systemTable, const wchar_t* phase) {
	const auto* bootServices = systemTable->BootServices;
	EFI_HANDLE* handles = nullptr;
	UINTN count = 0;
	const EFI_STATUS locateStatus = LocatePciHandles(
		/* systemTable */ systemTable,
		/* guid */ &PCI_ROOT_BRIDGE_IO_GUID,
		/* count */ &count,
		/* handles */ &handles);
	if (locateStatus != EFI_SUCCESS) {
		NLogging::Log(
			systemTable,
			L"[gpu.%s.probe] source=root-bridge pci=00:02.0 "
				L"presence=unknown locate_status=%r\n",
			/* phase */ AsEfiText(phase),
			/* locateStatus */ locateStatus);
		if (handles != nullptr) {
			bootServices->FreePool(handles);
		}
		return;
	}
	if (count == 0) {
		NLogging::Log(
			systemTable,
			L"[gpu.%s.probe] source=root-bridge pci=00:02.0 "
				L"presence=unknown reason=no-root-bridges\n",
			/* phase */ AsEfiText(phase));
	}
	// Root-bridge PCI encoding: bus << 24 | device << 16 | function << 8 | register.
	const uint64_t igdAddress = uint64_t{2} << 16; // Target Intel IGD: bus 0, device 2, function 0.
	for (UINTN index = 0; index < count; ++index) {
		EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL* bridge = nullptr;
		EFI_STATUS handleStatus = NPrivate::GetProtocol(
			/* systemTable */ systemTable,
			/* handle */ handles[index],
			/* guid */ &PCI_ROOT_BRIDGE_IO_GUID,
			/* protocol */ &bridge);
		if (handleStatus == EFI_SUCCESS && bridge->Pci.Read == nullptr) {
			handleStatus = EFI_UNSUPPORTED;
		}
		if (handleStatus != EFI_SUCCESS) {
			NLogging::Log(
				systemTable,
				L"[gpu.%s.probe] source=root-bridge root=%ld pci=00:02.0 "
					L"presence=unknown handle_status=%r\n",
				/* phase */ AsEfiText(phase),
				/* rootIndex */ index,
				/* handleStatus */ handleStatus);
			continue;
		}
		uint32_t vendorDevice = 0;
		const EFI_STATUS idStatus = bridge->Pci.Read(
			/* this */ bridge,
			/* width */ EfiPciIoWidthUint32,
			/* offset */ igdAddress + PCI_ID_OFFSET,
			/* count */ 1,
			/* buffer */ &vendorDevice);
		if (idStatus != EFI_SUCCESS) {
			NLogging::Log(
				systemTable,
				L"[gpu.%s.probe] source=root-bridge root=%ld pci=%04x:00:02.0 "
					L"presence=unknown read_status=%r\n",
				/* phase */ AsEfiText(phase),
				/* rootIndex */ index,
				/* segmentNumber */ bridge->SegmentNumber,
				/* idStatus */ idStatus);
			continue;
		}
		const uint32_t vendorId = vendorDevice & 0xffff;
		if (vendorId == PCI_ABSENT_VENDOR) {
			NLogging::Log(
				systemTable,
				L"[gpu.%s.probe] source=root-bridge root=%ld pci=%04x:00:02.0 "
					L"presence=absent\n",
				/* phase */ AsEfiText(phase),
				/* rootIndex */ index,
				/* segmentNumber */ bridge->SegmentNumber);
			continue;
		}
		const uint32_t deviceId = vendorDevice >> 16;
		const CHAR16* vendorName = AsEfiText(GetGpuVendorName(vendorId));
		uint32_t classRevision = 0;
		const EFI_STATUS classStatus = bridge->Pci.Read(
			/* this */ bridge,
			/* width */ EfiPciIoWidthUint32,
			/* offset */ igdAddress + PCI_CLASS_REVISION_OFFSET,
			/* count */ 1,
			/* buffer */ &classRevision);
		NLogging::Log(
			systemTable,
			L"[gpu.%s.probe] source=root-bridge root=%ld pci=%04x:00:02.0 "
				L"presence=present vendor=%s id=%04x:%04x class_read=%r\n",
			/* phase */ AsEfiText(phase),
			/* rootIndex */ index,
			/* segmentNumber */ bridge->SegmentNumber,
			/* vendorName */ vendorName,
			/* vendorId */ vendorId,
			/* deviceId */ deviceId,
			/* classStatus */ classStatus);
		if (classStatus == EFI_SUCCESS) {
			const uint32_t classCode = classRevision >> 8;
			const uint32_t revision = classRevision & 0xff;
			NLogging::Log(
				systemTable,
				L"[gpu.%s.probe] root=%ld pci=%04x:00:02.0 class=%06x revision=%02x\n",
				/* phase */ AsEfiText(phase),
				/* rootIndex */ index,
				/* segmentNumber */ bridge->SegmentNumber,
				/* classCode */ classCode,
				/* revision */ revision);
		}
	}
	if (handles != nullptr) {
		bootServices->FreePool(handles);
	}
}

void LogGpuList(EFI_SYSTEM_TABLE* systemTable, const wchar_t* phase) {
	if (!NLogging::IsAvailable(systemTable)) {
		return;
	}
	const auto* bootServices = systemTable->BootServices;
	if (bootServices == nullptr || bootServices->LocateHandleBuffer == nullptr ||
		bootServices->HandleProtocol == nullptr || bootServices->FreePool == nullptr)
	{
		NLogging::Log(
			systemTable,
			L"[gpu.%s] count=unknown source=pci-io locate_status=%r\n",
			/* phase */ AsEfiText(phase),
			/* locateStatus */ EFI_UNSUPPORTED);
		return;
	}
	EFI_HANDLE* handles = nullptr;
	UINTN count = 0;
	UINTN gpuCount = 0;
	bool hasIgd = false;
	bool incomplete = false;
	const EFI_STATUS locateStatus = LocatePciHandles(
		/* systemTable */ systemTable,
		/* guid */ &PCI_IO_GUID,
		/* count */ &count,
		/* handles */ &handles);
	if (locateStatus != EFI_SUCCESS) {
		NLogging::Log(
			systemTable,
			L"[gpu.%s] count=unknown source=pci-io locate_status=%r\n",
			/* phase */ AsEfiText(phase),
			/* locateStatus */ locateStatus);
		if (handles != nullptr) {
			bootServices->FreePool(handles);
		}
		LogIgdProbe(systemTable, phase);
		return;
	}
	for (UINTN index = 0; index < count; ++index) {
		EFI_PCI_IO_PROTOCOL* pci = nullptr;
		EFI_STATUS handleStatus = NPrivate::GetProtocol(
			/* systemTable */ systemTable,
			/* handle */ handles[index],
			/* guid */ &PCI_IO_GUID,
			/* protocol */ &pci);
		if (handleStatus == EFI_SUCCESS && pci->Pci.Read == nullptr) {
			handleStatus = EFI_UNSUPPORTED;
		}
		if (handleStatus != EFI_SUCCESS) {
			incomplete = true;
			NLogging::Log(
				systemTable,
				L"[gpu.%s] handle=%ld presence=unknown handle_status=%r\n",
				/* phase */ AsEfiText(phase),
				/* handleIndex */ index,
				/* handleStatus */ handleStatus);
			continue;
		}
		uint32_t classRevision = 0;
		const EFI_STATUS classStatus = pci->Pci.Read(
			/* this */ pci,
			/* width */ EfiPciIoWidthUint32,
			/* offset */ PCI_CLASS_REVISION_OFFSET,
			/* count */ 1,
			/* buffer */ &classRevision);
		if (classStatus != EFI_SUCCESS) {
			incomplete = true;
			NLogging::Log(
				systemTable,
				L"[gpu.%s] handle=%ld presence=unknown class_read=%r\n",
				/* phase */ AsEfiText(phase),
				/* handleIndex */ index,
				/* classStatus */ classStatus);
			continue;
		}
		const uint32_t baseClass = classRevision >> 24;
		if (baseClass != PCI_DISPLAY_CLASS) {
			continue;
		}
		uint32_t vendorDevice = 0;
		const EFI_STATUS idStatus = pci->Pci.Read(
			/* this */ pci,
			/* width */ EfiPciIoWidthUint32,
			/* offset */ PCI_ID_OFFSET,
			/* count */ 1,
			/* buffer */ &vendorDevice);
		const uint32_t vendorId = vendorDevice & 0xffff;
		if (idStatus != EFI_SUCCESS || vendorId == PCI_ABSENT_VENDOR) {
			incomplete = true;
			NLogging::Log(
				systemTable,
				L"[gpu.%s] handle=%ld presence=unknown id_read=%r id=%08x\n",
				/* phase */ AsEfiText(phase),
				/* handleIndex */ index,
				/* idStatus */ idStatus,
				/* vendorDevice */ vendorDevice);
			continue;
		}
		TPciLocation location;
		EFI_STATUS locationStatus = EFI_UNSUPPORTED;
		if (pci->GetLocation != nullptr) {
			locationStatus = pci->GetLocation(
				/* this */ pci,
				/* segment */ &location.Segment,
				/* bus */ &location.Bus,
				/* device */ &location.Device,
				/* function */ &location.Function);
		}
		const uint32_t deviceId = vendorDevice >> 16;
		const uint32_t classCode = classRevision >> 8;
		const uint32_t revision = classRevision & 0xff;
		const CHAR16* vendorName = AsEfiText(GetGpuVendorName(vendorId));
		if (locationStatus == EFI_SUCCESS) {
			const bool isKnownIgd = vendorId == PCI_VENDOR_INTEL && location.Segment == 0 &&
				location.Bus == 0 && location.Device == 2 && location.Function == 0;
			hasIgd = hasIgd || isKnownIgd;
			NLogging::Log(
				systemTable,
				L"[gpu.%s.%ld] pci=%04lx:%02lx:%02lx.%lx "
					L"vendor=%s id=%04x:%04x class=%06x revision=%02x\n",
				/* phase */ AsEfiText(phase),
				/* gpuIndex */ gpuCount,
				/* segment */ location.Segment,
				/* bus */ location.Bus,
				/* device */ location.Device,
				/* function */ location.Function,
				/* vendorName */ vendorName,
				/* vendorId */ vendorId,
				/* deviceId */ deviceId,
				/* classCode */ classCode,
				/* revision */ revision);
		} else {
			incomplete = true;
			NLogging::Log(
				systemTable,
				L"[gpu.%s.%ld] pci=unknown location_status=%r "
					L"vendor=%s id=%04x:%04x class=%06x revision=%02x\n",
				/* phase */ AsEfiText(phase),
				/* gpuIndex */ gpuCount,
				/* locationStatus */ locationStatus,
				/* vendorName */ vendorName,
				/* vendorId */ vendorId,
				/* deviceId */ deviceId,
				/* classCode */ classCode,
				/* revision */ revision);
		}
		++gpuCount;
	}
	NLogging::Log(
		systemTable,
		L"[gpu.%s] count=%ld source=pci-io incomplete=%d\n",
		/* phase */ AsEfiText(phase),
		/* gpuCount */ gpuCount,
		/* incomplete */ incomplete);
	if (handles != nullptr) {
		bootServices->FreePool(handles);
	}
	if (!hasIgd) {
		LogIgdProbe(systemTable, phase);
	}
}

} // namespace NDebug
