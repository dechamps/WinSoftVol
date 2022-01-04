#include <ntddk.h>
#include <Wdm.h>
#include <Wdf.h>

static void WinSoftVol_PrintDeviceName(IN WDFDEVICE device) {
	WDFMEMORY memory;
	const NTSTATUS queryPropertyStatus = WdfDeviceAllocAndQueryProperty(device, DevicePropertyFriendlyName, PagedPool, WDF_NO_OBJECT_ATTRIBUTES, &memory);
	if (!NT_SUCCESS(queryPropertyStatus)) {
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_WARNING_LEVEL, "WinSoftVol: WdfDeviceAllocAndQueryProperty() failed with status 0x%x\n", queryPropertyStatus));
		return;
	}
	// TODO: the memory will live for as long as the driver - we should release it early

	KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_INFO_LEVEL, "WinSoftVol: attached to device `%S`\n", (wchar_t*)WdfMemoryGetBuffer(memory, /*BufferSize=*/NULL)));
}

static EVT_WDF_DRIVER_DEVICE_ADD WinSoftVol_EvtWdfDriverDeviceAdd;
static NTSTATUS WinSoftVol_EvtWdfDriverDeviceAdd(IN WDFDRIVER driver, IN PWDFDEVICE_INIT deviceInit) {
	UNREFERENCED_PARAMETER(driver);

	WdfFdoInitSetFilter(deviceInit);

	WDFDEVICE device;
	const NTSTATUS deviceCreateStatus = WdfDeviceCreate(&deviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
	if (!NT_SUCCESS(deviceCreateStatus)) {
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "WinSoftVol: WdfDeviceCreate() failed with status 0x%x\n", deviceCreateStatus));
		return deviceCreateStatus;
	}

	WinSoftVol_PrintDeviceName(device);
	return deviceCreateStatus;
}

NTSTATUS DriverEntry(IN PDRIVER_OBJECT driverObject, IN PUNICODE_STRING registryPath) {
	KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_INFO_LEVEL, "WinSoftVol: loading driver\n"));

	WDF_DRIVER_CONFIG config;
	WDF_DRIVER_CONFIG_INIT(&config, WinSoftVol_EvtWdfDriverDeviceAdd);

	const NTSTATUS driverCreateStatus = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
	if (!NT_SUCCESS(driverCreateStatus)) KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "WinSoftVol: WdfDriverCreate() failed with status 0x%x\n", driverCreateStatus));
	return driverCreateStatus;
}
