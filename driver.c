#include <ntddk.h>
#include <Wdm.h>
#include <Wdf.h>
#include <minwindef.h>
#include <ks.h>

static void WinSoftVol_PrintDeviceName(IN WDFDEVICE device) {
	WDF_OBJECT_ATTRIBUTES memoryAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&memoryAttributes);
	memoryAttributes.ParentObject = device;
	WDFMEMORY memory;
	const NTSTATUS queryPropertyStatus = WdfDeviceAllocAndQueryProperty(device, DevicePropertyFriendlyName, PagedPool, &memoryAttributes, &memory);
	if (!NT_SUCCESS(queryPropertyStatus)) {
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_WARNING_LEVEL, "WinSoftVol: WdfDeviceAllocAndQueryProperty() failed with status 0x%x\n", queryPropertyStatus));
		return;
	}

	KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_INFO_LEVEL, "WinSoftVol: attached to device `%S`\n", (wchar_t*)WdfMemoryGetBuffer(memory, /*BufferSize=*/NULL)));

	WdfObjectDelete(memory);
}

static void WinSoftVol_ForwardRequest(IN WDFDEVICE device, IN WDFREQUEST request) {
	WDF_REQUEST_SEND_OPTIONS requestSendOptions;
	WDF_REQUEST_SEND_OPTIONS_INIT(&requestSendOptions, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
	if (!WdfRequestSend(request, WdfDeviceGetIoTarget(device), &requestSendOptions)) {
		const NTSTATUS requestStatus = WdfRequestGetStatus(request);
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "WinSoftVol: forwarding WdfRequestSend() failed with status 0x%x\n", requestStatus));
		WdfRequestComplete(request, requestStatus);
	}
}

// We do not use EvtIoDeviceControl to handle IOCTLs.
// This is because KS uses METHOD_NEITHER IOCTLs, which are difficult to forward from EvtIoDeviceControl().
// See https://community.osr.com/discussion/comment/303709
static EVT_WDF_IO_IN_CALLER_CONTEXT WinSoftVol_EvtWdfIoInCallerContext;
static void WinSoftVol_EvtWdfIoInCallerContext(IN WDFDEVICE device, IN WDFREQUEST request) {
	WDF_REQUEST_PARAMETERS requestParameters;
	WDF_REQUEST_PARAMETERS_INIT(&requestParameters);
	WdfRequestGetParameters(request, &requestParameters);

	if (requestParameters.Type == WdfRequestTypeDeviceControl && requestParameters.Parameters.DeviceIoControl.IoControlCode == IOCTL_KS_PROPERTY) {
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_INFO_LEVEL, "WinSoftVol: got IOCTL_KS_PROPERTY\n"));
	}

	WinSoftVol_ForwardRequest(device, request);	
}

static EVT_WDF_DRIVER_DEVICE_ADD WinSoftVol_EvtWdfDriverDeviceAdd;
static NTSTATUS WinSoftVol_EvtWdfDriverDeviceAdd(IN WDFDRIVER driver, IN PWDFDEVICE_INIT deviceInit) {
	UNREFERENCED_PARAMETER(driver);

	WdfFdoInitSetFilter(deviceInit);
	WdfDeviceInitSetIoInCallerContextCallback(deviceInit, WinSoftVol_EvtWdfIoInCallerContext);

	WDFDEVICE device;
	const NTSTATUS deviceCreateStatus = WdfDeviceCreate(&deviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
	if (!NT_SUCCESS(deviceCreateStatus)) {
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "WinSoftVol: WdfDeviceCreate() failed with status 0x%x\n", deviceCreateStatus));
		return deviceCreateStatus;
	}

	WinSoftVol_PrintDeviceName(device);

	return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(IN PDRIVER_OBJECT driverObject, IN PUNICODE_STRING registryPath) {
	KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_INFO_LEVEL, "WinSoftVol: loading driver\n"));

	WDF_DRIVER_CONFIG config;
	WDF_DRIVER_CONFIG_INIT(&config, WinSoftVol_EvtWdfDriverDeviceAdd);

	const NTSTATUS driverCreateStatus = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
	if (!NT_SUCCESS(driverCreateStatus)) KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "WinSoftVol: WdfDriverCreate() failed with status 0x%x\n", driverCreateStatus));
	return driverCreateStatus;
}
