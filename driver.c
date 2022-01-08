#include <ntddk.h>
#include <Wdm.h>
#include <Wdf.h>
#include <minwindef.h>
#include <ks.h>
#include <windef.h>
#include <ksmedia.h>

#include <stdarg.h>

static void WinSoftVol_Log(IN const ULONG level, _In_z_ _Printf_format_string_ const PCCH format, ...) {
	va_list args;
	va_start(args, format);
	vDbgPrintExWithPrefix("WinSoftVol: ", DPFLTR_IHVAUDIO_ID, level, format, args);
	va_end(args);
}

static void WinSoftVol_PrintDeviceName(IN WDFDEVICE device) {
	WDF_OBJECT_ATTRIBUTES memoryAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&memoryAttributes);
	memoryAttributes.ParentObject = device;
	WDFMEMORY memory;
	const NTSTATUS queryPropertyStatus = WdfDeviceAllocAndQueryProperty(device, DevicePropertyFriendlyName, PagedPool, &memoryAttributes, &memory);
	if (!NT_SUCCESS(queryPropertyStatus)) {
		WinSoftVol_Log(DPFLTR_WARNING_LEVEL, "WdfDeviceAllocAndQueryProperty() failed with status 0x%x\n", queryPropertyStatus);
		return;
	}

	WinSoftVol_Log(DPFLTR_INFO_LEVEL, "Attached to device `%S`\n", (wchar_t*)WdfMemoryGetBuffer(memory, /*BufferSize=*/NULL));

	WdfObjectDelete(memory);
}

static void WinSoftVol_ForwardRequest(IN WDFDEVICE device, IN WDFREQUEST request) {
	WDF_REQUEST_SEND_OPTIONS requestSendOptions;
	WDF_REQUEST_SEND_OPTIONS_INIT(&requestSendOptions, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
	if (!WdfRequestSend(request, WdfDeviceGetIoTarget(device), &requestSendOptions)) {
		const NTSTATUS requestStatus = WdfRequestGetStatus(request);
		WinSoftVol_Log(DPFLTR_ERROR_LEVEL, "Forwarding WdfRequestSend() failed with status 0x%x\n", requestStatus);
		WdfRequestComplete(request, requestStatus);
	}
}

static BOOL WinSoftVol_IsGetKsTopologyNodesPropertyRequest(IN WDFREQUEST request) {
	WDF_REQUEST_PARAMETERS requestParameters;
	WDF_REQUEST_PARAMETERS_INIT(&requestParameters);
	WdfRequestGetParameters(request, &requestParameters);

	if (requestParameters.Type != WdfRequestTypeDeviceControl) return FALSE;
	if (requestParameters.Parameters.DeviceIoControl.IoControlCode != IOCTL_KS_PROPERTY) return FALSE;

	PVOID inputBuffer;
	const NTSTATUS retrieveInputBufferStatus = WdfRequestRetrieveUnsafeUserInputBuffer(request, sizeof(KSPROPERTY), &inputBuffer, /*Length=*/NULL);
	if (!NT_SUCCESS(retrieveInputBufferStatus) || inputBuffer == NULL) {
		WinSoftVol_Log(DPFLTR_ERROR_LEVEL, "WdfRequestRetrieveUnsafeUserInputBuffer() failed with status 0x%x\n", retrieveInputBufferStatus);
		return FALSE;
	}

	WDFMEMORY inputMemory;
	const NTSTATUS lockInputBufferStatus = WdfRequestProbeAndLockUserBufferForRead(request, inputBuffer, sizeof(KSPROPERTY), &inputMemory);
	if (!NT_SUCCESS(lockInputBufferStatus)) {
		WinSoftVol_Log(DPFLTR_ERROR_LEVEL, "WdfRequestProbeAndLockUserBufferForRead() failed with status 0x%x\n", lockInputBufferStatus);
		return FALSE;
	}

	const KSPROPERTY* const ksProperty = WdfMemoryGetBuffer(inputMemory, /*BufferSize=*/NULL);
	BOOL isPropertySetRequest = IsEqualGUID(&ksProperty->Set, &KSPROPSETID_Topology) && ksProperty->Id == KSPROPERTY_TOPOLOGY_NODES && ksProperty->Flags & KSPROPERTY_TYPE_GET;
	if (IsEqualGUID(&ksProperty->Set, &KSPROPSETID_Audio)) {
		isPropertySetRequest = TRUE;
		WinSoftVol_Log(DPFLTR_ERROR_LEVEL, "Got KSPROPSETID_Audio with Id %lu and Flags 0x%lx\n", ksProperty->Id, ksProperty->Flags);
		if (ksProperty->Flags & KSPROPERTY_TYPE_TOPOLOGY) {
			WinSoftVol_Log(DPFLTR_ERROR_LEVEL, "KSPROPERTY_TYPE_TOPOLOGY node ID %lu\n", ((const KSP_NODE*)ksProperty)->NodeId);
		}
	}
	
	WdfObjectDelete(inputMemory);
	return isPropertySetRequest;
}

static EVT_WDF_REQUEST_COMPLETION_ROUTINE WinSoftVol_WdfRequestCompletionRoutine;
static void WinSoftVol_WdfRequestCompletionRoutine(IN WDFREQUEST request, IN WDFIOTARGET ioTarget, IN PWDF_REQUEST_COMPLETION_PARAMS requestCompletionParams, IN WDFCONTEXT context) {
	UNREFERENCED_PARAMETER(ioTarget);
	UNREFERENCED_PARAMETER(context);

	WinSoftVol_Log(DPFLTR_INFO_LEVEL, "Request came back with status 0x%x\n", requestCompletionParams->IoStatus.Status);

	WdfRequestComplete(request, requestCompletionParams->IoStatus.Status);
}

static BOOL WinSoftVol_InterceptRequest(IN WDFREQUEST request, IN WDFDEVICE device) {
	WdfRequestFormatRequestUsingCurrentType(request);
	WdfRequestSetCompletionRoutine(request, WinSoftVol_WdfRequestCompletionRoutine, WDF_NO_CONTEXT);
	if (!WdfRequestSend(request, WdfDeviceGetIoTarget(device), WDF_NO_SEND_OPTIONS)) {
		const NTSTATUS requestStatus = WdfRequestGetStatus(request);
		WinSoftVol_Log(DPFLTR_ERROR_LEVEL, "Intercepting WdfRequestSend() failed with status 0x%x\n", requestStatus);
		WdfRequestComplete(request, requestStatus);
		return FALSE;
	}

	return TRUE;
}

// We do not use EvtIoDeviceControl to handle IOCTLs.
// This is because KS uses METHOD_NEITHER IOCTLs, which are difficult to forward from EvtIoDeviceControl().
// See https://community.osr.com/discussion/comment/303709
static EVT_WDF_IO_IN_CALLER_CONTEXT WinSoftVol_EvtWdfIoInCallerContext;
static void WinSoftVol_EvtWdfIoInCallerContext(IN WDFDEVICE device, IN WDFREQUEST request) {
	if (WinSoftVol_IsGetKsTopologyNodesPropertyRequest(request)) {
		WinSoftVol_Log(DPFLTR_INFO_LEVEL, "Got KS nodes property get request\n");
		if (WinSoftVol_InterceptRequest(request, device)) return;
	}

	// According to EVT_WDF_IO_IN_CALLER_CONTEXT docs we're not supposed to do this here.
	// However doing it in `EvtIoDeviceControl` is arguably even worse because it might not run in the original thread context, making it a poor choice for METHOD_NEITHER IOCTLs.
	// See https://community.osr.com/discussion/comment/303709
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
		WinSoftVol_Log(DPFLTR_ERROR_LEVEL, "WdfDeviceCreate() failed with status 0x%x\n", deviceCreateStatus);
		return deviceCreateStatus;
	}

	WinSoftVol_PrintDeviceName(device);

	return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(IN PDRIVER_OBJECT driverObject, IN PUNICODE_STRING registryPath) {
	KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_INFO_LEVEL, "Loading driver\n"));

	WDF_DRIVER_CONFIG config;
	WDF_DRIVER_CONFIG_INIT(&config, WinSoftVol_EvtWdfDriverDeviceAdd);

	const NTSTATUS driverCreateStatus = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
	if (!NT_SUCCESS(driverCreateStatus)) WinSoftVol_Log(DPFLTR_ERROR_LEVEL, "WdfDriverCreate() failed with status 0x%x\n", driverCreateStatus);
	return driverCreateStatus;
}
