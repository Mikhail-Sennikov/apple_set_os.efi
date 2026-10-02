// One-shot Clover-resident diagnostics for the Apple Set OS boot path.

#include "debug.h"

#include <efilib.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define PCI_CONFIG_ADDRESS(bus, device, function, reg) \
	((((UINT64)(bus)) << 24) | (((UINT64)(device)) << 16) | \
	 (((UINT64)(function)) << 8) | ((UINT64)(reg)))

static CHAR16 WINDOWS_BOOT_PATH[] =
	L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
static CHAR16 ARM_MARKER_PATH[] =
	L"\\EFI\\CLOVER\\misc\\apple-set-os-driver-diag.once";
static CHAR16 CONSUMED_MARKER_PATH[] =
	L"\\EFI\\CLOVER\\misc\\apple-set-os-driver-diag.consumed";
static CHAR16 DIAG_LOG_PATH[] =
	L"\\EFI\\CLOVER\\misc\\apple-set-os-driver-diag.log";
static CHAR8 CONSUMED_MARKER_TOKEN[] =
	"apple-set-os-driver-diag-v1";

/* GNU-EFI 3.0.4 has no root-bridge header. This prefix follows UEFI 2.x. */
static EFI_GUID PCI_ROOT_BRIDGE_IO_GUID = {
	0x2f707ebb, 0x4a1a, 0x11d4,
	{ 0x9a, 0x38, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d }
};

typedef struct pci_root_bridge_io PCI_ROOT_BRIDGE_IO;

typedef enum {
	PciWidthUint8,
	PciWidthUint16,
	PciWidthUint32,
	PciWidthUint64,
	PciWidthFifoUint8,
	PciWidthFifoUint16,
	PciWidthFifoUint32,
	PciWidthFifoUint64,
	PciWidthFillUint8,
	PciWidthFillUint16,
	PciWidthFillUint32,
	PciWidthFillUint64,
	PciWidthMaximum
} PCI_ROOT_BRIDGE_IO_WIDTH;

typedef EFI_STATUS (EFIAPI *PCI_ROOT_BRIDGE_IO_RW)(
	IN PCI_ROOT_BRIDGE_IO *self,
	IN PCI_ROOT_BRIDGE_IO_WIDTH width,
	IN UINT64 address,
	IN UINTN count,
	IN OUT VOID *buffer
);

typedef EFI_STATUS (EFIAPI *PCI_ROOT_BRIDGE_IO_POLL)(
	IN PCI_ROOT_BRIDGE_IO *self,
	IN PCI_ROOT_BRIDGE_IO_WIDTH width,
	IN UINT64 address,
	IN UINT64 mask,
	IN UINT64 value,
	IN UINT64 delay,
	OUT UINT64 *result
);

typedef struct {
	PCI_ROOT_BRIDGE_IO_RW read;
	PCI_ROOT_BRIDGE_IO_RW write;
} PCI_ROOT_BRIDGE_IO_ACCESS;

/* Only the protocol prefix through Pci is needed by this application. */
struct pci_root_bridge_io {
	EFI_HANDLE parent_handle;
	PCI_ROOT_BRIDGE_IO_POLL poll_mem;
	PCI_ROOT_BRIDGE_IO_POLL poll_io;
	PCI_ROOT_BRIDGE_IO_ACCESS mem;
	PCI_ROOT_BRIDGE_IO_ACCESS io;
	PCI_ROOT_BRIDGE_IO_ACCESS pci;
};

typedef struct {
	EFI_FILE *root;
	EFI_FILE *file;
} DIAG_LOG;

typedef struct {
	DIAG_LOG *log;
	BOOLEAN setter_started;
	EFI_STATUS first_log_error;
} SET_OS_LOG_CONTEXT;

static EFI_HANDLE diag_device;
static EFI_EVENT loaded_image_event;
static VOID *loaded_image_registration;
static BOOLEAN bootmgfw_seen;
static BOOLEAN loaded_image_notify_busy;
static BOOLEAN loaded_image_notify_ready;
static BOOLEAN setters_attempted;
static BOOLEAN set_os_protocol_present;
static APPLE_SET_OS_RESULT set_os_result;
static DIAG_LOG main_log;
static SET_OS_LOG_CONTEXT set_os_log_context;

static EFI_STATUS
write_all(IN EFI_FILE *file, IN VOID *buffer, IN UINTN size)
{
	UINT8 *cursor = (UINT8 *)buffer;

	while (size > 0) {
		UINTN written = size;
		EFI_STATUS status = file->Write(file, &written, cursor);

		if (status != EFI_SUCCESS)
			return status;
		if (written == 0 || written > size)
			return EFI_DEVICE_ERROR;

		cursor += written;
		size -= written;
	}

	return EFI_SUCCESS;
}

static EFI_STATUS
log_vprintf(IN DIAG_LOG *log, IN CHAR16 *format, IN va_list args)
{
	CHAR16 buffer[384];
	UINTN size;
	EFI_STATUS status;

	if (log == NULL || log->file == NULL)
		return EFI_NOT_READY;

	VSPrint(buffer, sizeof(buffer), format, args);
	buffer[ARRAY_SIZE(buffer) - 1] = L'\0';
	size = StrLen(buffer) * sizeof(buffer[0]);

	status = write_all(log->file, buffer, size);
	if (status != EFI_SUCCESS)
		return status;

	return log->file->Flush(log->file);
}

static EFI_STATUS
log_printf(IN DIAG_LOG *log, IN CHAR16 *format, ...)
{
	va_list args;
	EFI_STATUS status;

	va_start(args, format);
	status = log_vprintf(log, format, args);
	va_end(args);
	return status;
}

static EFI_STATUS
close_log(IN OUT DIAG_LOG *log)
{
	EFI_STATUS result = EFI_SUCCESS;
	EFI_STATUS status;

	if (log->file != NULL) {
		status = log->file->Flush(log->file);
		if (result == EFI_SUCCESS && status != EFI_SUCCESS)
			result = status;
		status = log->file->Close(log->file);
		if (result == EFI_SUCCESS && status != EFI_SUCCESS)
			result = status;
		log->file = NULL;
	}
	if (log->root != NULL) {
		status = log->root->Close(log->root);
		if (result == EFI_SUCCESS && status != EFI_SUCCESS)
			result = status;
		log->root = NULL;
	}

	return result;
}

static EFI_STATUS
open_log(IN EFI_HANDLE device, OUT DIAG_LOG *log)
{
	EFI_FILE_IO_INTERFACE *file_system = NULL;
	UINT64 position = 0;
	EFI_STATUS status;

	log->root = NULL;
	log->file = NULL;

	status = BS->HandleProtocol(device, &FileSystemProtocol,
		(VOID **)&file_system);
	if (status != EFI_SUCCESS || file_system == NULL)
		return status != EFI_SUCCESS ? status : EFI_NOT_FOUND;

	status = file_system->OpenVolume(file_system, &log->root);
	if (status != EFI_SUCCESS || log->root == NULL) {
		close_log(log);
		return status != EFI_SUCCESS ? status : EFI_DEVICE_ERROR;
	}

	status = log->root->Open(log->root, &log->file, DIAG_LOG_PATH,
		EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
		0);
	if (status != EFI_SUCCESS || log->file == NULL) {
		close_log(log);
		return status != EFI_SUCCESS ? status : EFI_DEVICE_ERROR;
	}

	status = log->file->SetPosition(log->file, ~(UINT64)0);
	if (status == EFI_SUCCESS)
		status = log->file->GetPosition(log->file, &position);
	if (status != EFI_SUCCESS) {
		close_log(log);
		return status;
	}

	if (position == 0) {
		CHAR16 byte_order_mark = 0xfeff;

		status = write_all(log->file, &byte_order_mark,
			sizeof(byte_order_mark));
		if (status == EFI_SUCCESS)
			status = log->file->Flush(log->file);
		if (status != EFI_SUCCESS) {
			close_log(log);
			return status;
		}
	}

	return EFI_SUCCESS;
}

static EFI_STATUS
open_root(IN EFI_HANDLE device, OUT EFI_FILE **root)
{
	EFI_FILE_IO_INTERFACE *file_system = NULL;
	EFI_STATUS status;

	*root = NULL;
	status = BS->HandleProtocol(device, &FileSystemProtocol,
		(VOID **)&file_system);
	if (status != EFI_SUCCESS || file_system == NULL)
		return status != EFI_SUCCESS ? status : EFI_NOT_FOUND;

	status = file_system->OpenVolume(file_system, root);
	if (status != EFI_SUCCESS || *root == NULL)
		return status != EFI_SUCCESS ? status : EFI_DEVICE_ERROR;

	return EFI_SUCCESS;
}

/*
 * Persist and verify the consumed marker before Apple Set OS is called. The
 * separate consumed marker protects the next boot even if hard power-off
 * prevents the armed-file deletion from reaching the FAT directory.
 */
static EFI_STATUS
consume_arm_marker(IN EFI_HANDLE device)
{
	EFI_FILE *root = NULL;
	EFI_FILE *marker = NULL;
	EFI_FILE *consumed = NULL;
	CHAR8 verification[sizeof(CONSUMED_MARKER_TOKEN)];
	UINTN verification_size;
	UINTN index;
	EFI_STATUS status;
	EFI_STATUS close_status;

	status = open_root(device, &root);
	if (status != EFI_SUCCESS)
		return status;

	status = root->Open(root, &consumed, CONSUMED_MARKER_PATH,
		EFI_FILE_MODE_READ, 0);
	if (status == EFI_SUCCESS) {
		if (consumed == NULL) {
			root->Close(root);
			return EFI_DEVICE_ERROR;
		}
		consumed->Close(consumed);
		root->Close(root);
		return EFI_ALREADY_STARTED;
	}
	if (status != EFI_NOT_FOUND) {
		if (consumed != NULL)
			consumed->Close(consumed);
		root->Close(root);
		return status;
	}

	status = root->Open(root, &marker, ARM_MARKER_PATH,
		EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
	if (status != EFI_SUCCESS || marker == NULL) {
		root->Close(root);
		return status != EFI_SUCCESS ? status : EFI_DEVICE_ERROR;
	}

	status = root->Open(root, &consumed, CONSUMED_MARKER_PATH,
		EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
		0);
	if (status != EFI_SUCCESS || consumed == NULL) {
		marker->Close(marker);
		root->Close(root);
		return status != EFI_SUCCESS ? status : EFI_DEVICE_ERROR;
	}

	status = write_all(consumed, CONSUMED_MARKER_TOKEN,
		sizeof(CONSUMED_MARKER_TOKEN));
	if (status == EFI_SUCCESS)
		status = consumed->Flush(consumed);
	close_status = consumed->Close(consumed);
	consumed = NULL;
	if (status == EFI_SUCCESS)
		status = close_status;
	if (status != EFI_SUCCESS) {
		marker->Close(marker);
		root->Close(root);
		return status;
	}

	status = root->Open(root, &consumed, CONSUMED_MARKER_PATH,
		EFI_FILE_MODE_READ, 0);
	if (status != EFI_SUCCESS || consumed == NULL) {
		marker->Close(marker);
		root->Close(root);
		return status != EFI_SUCCESS ? status : EFI_DEVICE_ERROR;
	}
	verification_size = sizeof(verification);
	status = consumed->Read(consumed, &verification_size, verification);
	close_status = consumed->Close(consumed);
	consumed = NULL;
	if (status == EFI_SUCCESS)
		status = close_status;
	if (status != EFI_SUCCESS ||
		verification_size != sizeof(CONSUMED_MARKER_TOKEN)) {
		marker->Close(marker);
		root->Close(root);
		return status != EFI_SUCCESS ? status : EFI_VOLUME_CORRUPTED;
	}
	for (index = 0; index < sizeof(CONSUMED_MARKER_TOKEN); ++index) {
		if (verification[index] != CONSUMED_MARKER_TOKEN[index]) {
			marker->Close(marker);
			root->Close(root);
			return EFI_SECURITY_VIOLATION;
		}
	}

	status = marker->Delete(marker);
	close_status = root->Close(root);
	if (status == EFI_SUCCESS)
		status = close_status;
	return status;
}

static EFI_STATUS
log_igd_snapshot(IN DIAG_LOG *log, IN CHAR16 *phase)
{
	EFI_HANDLE *handles = NULL;
	UINTN handle_count = 0;
	UINTN index;
	EFI_STATUS locate_status;
	EFI_STATUS log_status;

	locate_status = BS->LocateHandleBuffer(ByProtocol,
		&PCI_ROOT_BRIDGE_IO_GUID, NULL, &handle_count, &handles);
	log_status = log_printf(log,
		L"pci phase=%s locate=%r raw=0x%016lx roots=%d\r\n",
		phase, locate_status, (UINT64)locate_status,
		(UINT32)handle_count);
	if (log_status != EFI_SUCCESS)
		goto done;

	if (locate_status != EFI_SUCCESS)
		goto done;
	if (handle_count > 0 && handles == NULL) {
		log_status = log_printf(log,
			L"pci phase=%s error=null_handle_buffer\r\n", phase);
		goto done;
	}

	for (index = 0; index < handle_count; ++index) {
		PCI_ROOT_BRIDGE_IO *bridge = NULL;
		UINT32 vendor_device = 0xffffffff;
		UINT32 class_revision = 0xffffffff;
		EFI_STATUS handle_status;
		EFI_STATUS id_status = EFI_NOT_READY;
		EFI_STATUS class_status = EFI_NOT_READY;

		handle_status = BS->HandleProtocol(handles[index],
			&PCI_ROOT_BRIDGE_IO_GUID, (VOID **)&bridge);
		if (handle_status == EFI_SUCCESS && bridge != NULL &&
			bridge->pci.read != NULL) {
			id_status = bridge->pci.read(bridge, PciWidthUint32,
				PCI_CONFIG_ADDRESS(0, 2, 0, 0x00), 1,
				&vendor_device);
			class_status = bridge->pci.read(bridge, PciWidthUint32,
				PCI_CONFIG_ADDRESS(0, 2, 0, 0x08), 1,
				&class_revision);
		}

		log_status = log_printf(log,
			L"pci phase=%s root=%d bdf=00:02.0 handle=%r handle_raw=0x%016lx "
			L"id_status=%r id_status_raw=0x%016lx id=0x%08x "
			L"class_status=%r class_status_raw=0x%016lx class_rev=0x%08x\r\n",
			phase, (UINT32)index,
			handle_status, (UINT64)handle_status,
			id_status, (UINT64)id_status, vendor_device,
			class_status, (UINT64)class_status, class_revision);
		if (log_status != EFI_SUCCESS)
			break;
	}

done:
	if (handles != NULL)
		BS->FreePool(handles);
	return log_status;
}

EFI_STATUS
apple_set_os_debug_observer(
	IN VOID *context,
	IN APPLE_SET_OS_STAGE stage
)
{
	SET_OS_LOG_CONTEXT *log_context = &set_os_log_context;
	EFI_STATUS status;

	(void)context;

	switch (stage) {
	case APPLE_SET_OS_BEFORE_VENDOR:
		status = log_printf(log_context->log,
			L"set_os_vendor stage=before\r\n");
		break;
	case APPLE_SET_OS_AFTER_VENDOR:
		status = log_printf(log_context->log,
			L"set_os_vendor stage=returned\r\n");
		break;
	case APPLE_SET_OS_BEFORE_VERSION:
		status = log_printf(log_context->log,
			L"set_os_version stage=before\r\n");
		break;
	case APPLE_SET_OS_AFTER_VERSION:
		status = log_printf(log_context->log,
			L"set_os_version stage=returned\r\n");
		break;
	default:
		return EFI_INVALID_PARAMETER;
	}

	if (status != EFI_SUCCESS) {
		if (log_context->first_log_error == EFI_SUCCESS)
			log_context->first_log_error = status;
		/* Before the first setter, fail closed with firmware unchanged. */
		if (!log_context->setter_started &&
			(stage == APPLE_SET_OS_BEFORE_VENDOR ||
			 stage == APPLE_SET_OS_BEFORE_VERSION))
			return status;
		/* After a setter starts, finish the Linux sequence and boot. */
		return EFI_SUCCESS;
	}

	if (stage == APPLE_SET_OS_BEFORE_VENDOR ||
		stage == APPLE_SET_OS_BEFORE_VERSION)
		log_context->setter_started = 1;

	return EFI_SUCCESS;
}

static CHAR16
lower_ascii(IN CHAR16 character)
{
	if (character >= L'A' && character <= L'Z')
		return character + (L'a' - L'A');
	return character;
}

static BOOLEAN
file_path_node_matches(
	IN FILEPATH_DEVICE_PATH *file_path,
	IN UINTN node_length
)
{
	UINTN path_bytes;
	UINTN path_characters;
	UINTN target_length;
	UINTN index;

	if (node_length < SIZE_OF_FILEPATH_DEVICE_PATH)
		return 0;

	path_bytes = node_length - SIZE_OF_FILEPATH_DEVICE_PATH;
	path_characters = path_bytes / sizeof(CHAR16);
	target_length = StrLen(WINDOWS_BOOT_PATH);
	if (path_bytes % sizeof(CHAR16) != 0 ||
		path_characters != target_length + 1)
		return 0;

	for (index = 0; index < target_length; ++index) {
		if (lower_ascii(file_path->PathName[index]) !=
			lower_ascii(WINDOWS_BOOT_PATH[index]))
			return 0;
	}

	return file_path->PathName[target_length] == L'\0';
}

static BOOLEAN
is_windows_boot_manager_path(IN EFI_DEVICE_PATH *device_path)
{
	EFI_DEVICE_PATH *node = device_path;
	UINTN node_count = 0;

	while (node != NULL && !IsDevicePathEnd(node) && node_count < 64) {
		UINTN node_length = DevicePathNodeLength(node);

		if (node_length < sizeof(EFI_DEVICE_PATH))
			return 0;
		if (DevicePathType(node) == MEDIA_DEVICE_PATH &&
			DevicePathSubType(node) == MEDIA_FILEPATH_DP &&
			file_path_node_matches((FILEPATH_DEVICE_PATH *)node,
				node_length) &&
			IsDevicePathEnd(NextDevicePathNode(node)))
			return 1;

		node = NextDevicePathNode(node);
		++node_count;
	}

	return 0;
}

static EFI_STATUS
log_protocol_snapshot(IN DIAG_LOG *log, IN CHAR16 *phase)
{
	APPLE_SET_OS_INTERFACE *set_os = NULL;
	EFI_STATUS status;

	status = apple_set_os_locate(BS, &set_os);
	return log_printf(log,
		L"set_os_protocol phase=%s status=%r raw=0x%016lx present=%d "
		L"revision=0x%016lx version_callback=%d vendor_callback=%d\r\n",
		phase, status, (UINT64)status,
		(UINT32)(status == EFI_SUCCESS && set_os != NULL),
		status == EFI_SUCCESS && set_os != NULL ?
			set_os->revision : (UINT64)0,
		(UINT32)(status == EFI_SUCCESS && set_os != NULL &&
			set_os->set_os_version != NULL),
		(UINT32)(status == EFI_SUCCESS && set_os != NULL &&
			set_os->set_os_vendor != NULL));
}

static VOID
close_loaded_image_notification(VOID)
{
	loaded_image_notify_ready = 0;
	if (loaded_image_event != NULL) {
		BS->CloseEvent(loaded_image_event);
		loaded_image_event = NULL;
	}
	loaded_image_registration = NULL;
}

/*
 * Clover installs LoadedImageProtocol during LoadImage(), then calls
 * StartImage(). This notification is the last reliable observation point
 * before Windows without replacing the normal Clover Windows entry.
 */
static VOID EFIAPI
on_loaded_image(IN EFI_EVENT event, IN VOID *context)
{
	EFI_STATUS status;

	(void)event;
	(void)context;
	/* Keep queued handles pending until the main log has been closed. */
	if (!loaded_image_notify_ready || bootmgfw_seen || loaded_image_notify_busy ||
		loaded_image_registration == NULL)
		return;
	loaded_image_notify_busy = 1;

	for (;;) {
		EFI_HANDLE handle = NULL;
		EFI_LOADED_IMAGE *loaded_image = NULL;
		UINTN handle_size = sizeof(handle);

		status = BS->LocateHandle(ByRegisterNotify, NULL,
			loaded_image_registration, &handle_size, &handle);
		if (status != EFI_SUCCESS)
			break;

		status = BS->HandleProtocol(handle, &LoadedImageProtocol,
			(VOID **)&loaded_image);
		if (status != EFI_SUCCESS || loaded_image == NULL ||
			loaded_image->FilePath == NULL ||
			!is_windows_boot_manager_path(loaded_image->FilePath))
			continue;

		bootmgfw_seen = 1;
		{
			DIAG_LOG log;

			if (open_log(diag_device, &log) == EFI_SUCCESS) {
				log_printf(&log,
					L"windows_image phase=loaded handle=0x%016lx "
					L"device=0x%016lx setters_attempted=%d\r\n",
					(UINT64)(UINTN)handle,
					(UINT64)(UINTN)loaded_image->DeviceHandle,
					(UINT32)setters_attempted);
				log_protocol_snapshot(&log, L"windows_image_loaded");
				log_igd_snapshot(&log, L"windows_image_loaded");
				log_printf(&log,
					L"windows_image phase=snapshot_complete\r\n");
				close_log(&log);
			}
		}

		close_loaded_image_notification();
		break;
	}

	loaded_image_notify_busy = 0;
}

static EFI_STATUS EFIAPI
driver_unload(IN EFI_HANDLE image)
{
	(void)image;
	close_loaded_image_notification();
	return EFI_SUCCESS;
}

EFI_STATUS
apple_set_os_debug_begin(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table)
{
	EFI_LOADED_IMAGE *loaded_image = NULL;
	EFI_STATUS status;
	EFI_STATUS log_status;
	EFI_STATUS time_status;
	EFI_TIME run_time;

	InitializeLib(image, system_table);
	loaded_image_notify_ready = 0;
	set_os_protocol_present = 0;
	set_os_log_context.log = &main_log;
	set_os_log_context.setter_started = 0;
	set_os_log_context.first_log_error = EFI_SUCCESS;

	status = BS->HandleProtocol(image, &LoadedImageProtocol,
		(VOID **)&loaded_image);
	if (status != EFI_SUCCESS || loaded_image == NULL ||
		loaded_image->DeviceHandle == NULL)
		return status != EFI_SUCCESS ? status : EFI_NOT_FOUND;

	diag_device = loaded_image->DeviceHandle;
	status = consume_arm_marker(diag_device);
	if (status == EFI_NOT_FOUND || status == EFI_ALREADY_STARTED)
		return EFI_UNSUPPORTED;
	if (status != EFI_SUCCESS)
		return EFI_ERROR(status) ? status : EFI_DEVICE_ERROR;

	status = open_log(diag_device, &main_log);
	if (status != EFI_SUCCESS)
		return status;

	time_status = RT->GetTime(&run_time, NULL);
	if (time_status == EFI_SUCCESS) {
		status = log_printf(&main_log,
			L"\r\nrun stage=driver_begin time=%t marker=consumed\r\n",
			&run_time);
	} else {
		status = log_printf(&main_log,
			L"\r\nrun stage=driver_begin time_status=%r raw=0x%016lx "
			L"marker=consumed\r\n",
			time_status, (UINT64)time_status);
	}
	if (status != EFI_SUCCESS)
		goto fail_before_setter;

	status = BS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
		on_loaded_image, NULL, &loaded_image_event);
	log_status = log_printf(&main_log,
		L"loaded_image_notify stage=create status=%r raw=0x%016lx\r\n",
		status, (UINT64)status);
	if (log_status != EFI_SUCCESS) {
		status = log_status;
		goto fail_before_setter;
	}
	if (status != EFI_SUCCESS)
		goto fail_before_setter;

	status = BS->RegisterProtocolNotify(&LoadedImageProtocol,
		loaded_image_event, &loaded_image_registration);
	log_status = log_printf(&main_log,
		L"loaded_image_notify stage=register status=%r raw=0x%016lx\r\n",
		status, (UINT64)status);
	if (log_status != EFI_SUCCESS) {
		status = log_status;
		goto fail_before_setter;
	}
	if (status != EFI_SUCCESS)
		goto fail_before_setter;

	loaded_image->Unload = driver_unload;

	status = log_igd_snapshot(&main_log, L"driver_before");
	if (status != EFI_SUCCESS)
		goto fail_before_setter;

	return EFI_SUCCESS;

fail_before_setter:
	close_log(&main_log);
	close_loaded_image_notification();
	return status;
}

EFI_STATUS
apple_set_os_debug_before_apply(
	IN EFI_STATUS locate_status,
	IN APPLE_SET_OS_INTERFACE *protocol
)
{
	set_os_protocol_present = locate_status == EFI_SUCCESS && protocol != NULL;
	return log_printf(&main_log,
		L"set_os_protocol phase=driver status=%r raw=0x%016lx "
		L"present=%d revision=0x%016lx\r\n",
		locate_status, (UINT64)locate_status,
		(UINT32)(locate_status == EFI_SUCCESS && protocol != NULL),
		locate_status == EFI_SUCCESS && protocol != NULL ?
			(UINT64)protocol->revision : (UINT64)0);
}

EFI_STATUS
apple_set_os_debug_finish(
	IN EFI_STATUS apply_status,
	IN CONST APPLE_SET_OS_RESULT *result
)
{
	EFI_STATUS status = apply_status;
	BOOLEAN logging_degraded =
		set_os_log_context.first_log_error != EFI_SUCCESS;

	if (result == NULL) {
		status = EFI_INVALID_PARAMETER;
		goto fail_before_setter;
	}
	set_os_result = *result;
	setters_attempted = set_os_result.vendor_attempted ||
		set_os_result.version_attempted;
	if (status != EFI_SUCCESS)
		goto fail_before_setter;

	if (set_os_protocol_present) {
		status = log_printf(&main_log,
			L"set_os_summary revision=0x%016lx vendor_attempted=%d "
			L"version_attempted=%d callback_abi=void\r\n",
			(UINT64)set_os_result.revision,
			(UINT32)set_os_result.vendor_attempted,
			(UINT32)set_os_result.version_attempted);
		if (status != EFI_SUCCESS) {
			if (!setters_attempted)
				goto fail_before_setter;
			logging_degraded = 1;
		}
	}

	status = log_igd_snapshot(&main_log, L"driver_after");
	if (status != EFI_SUCCESS) {
		if (!setters_attempted)
			goto fail_before_setter;
		logging_degraded = 1;
	}

	status = log_printf(&main_log,
		L"run stage=driver_end logging_degraded=%d notify_active=%d\r\n",
		(UINT32)logging_degraded,
		(UINT32)(loaded_image_event != NULL));
	if (status != EFI_SUCCESS && !setters_attempted)
		goto fail_before_setter;

	status = close_log(&main_log);
	if (status != EFI_SUCCESS) {
		/* An uncertain close must not let another writer open the log. */
		close_loaded_image_notification();
		return setters_attempted ? EFI_SUCCESS : status;
	}

	/*
	 * RegisterProtocolNotify may already have signalled this event. Its
	 * callback leaves those handles queued while the main file is open.
	 * Release that writer before draining or accepting future images, so a
	 * pre-existing bootmgfw snapshot cannot be overwritten by driver_end.
	 */
	loaded_image_notify_ready = 1;
	on_loaded_image(NULL, NULL);

	return EFI_SUCCESS;

fail_before_setter:
	close_log(&main_log);
	close_loaded_image_notification();
	return status;
}
