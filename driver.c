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

static BOOL WinSoftVol_IsGetKsTopologyNodesPropertyRequest(IN WDFREQUEST request) {
	WDF_REQUEST_PARAMETERS requestParameters;
	WDF_REQUEST_PARAMETERS_INIT(&requestParameters);
	WdfRequestGetParameters(request, &requestParameters);

	if (requestParameters.Type != WdfRequestTypeDeviceControl) return FALSE;
	if (requestParameters.Parameters.DeviceIoControl.IoControlCode != IOCTL_KS_PROPERTY) return FALSE;

	PVOID inputBuffer;
	const NTSTATUS retrieveInputBufferStatus = WdfRequestRetrieveUnsafeUserInputBuffer(request, sizeof(KSPROPERTY), &inputBuffer, /*Length=*/NULL);
	if (!NT_SUCCESS(retrieveInputBufferStatus) || inputBuffer == NULL) {
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "WinSoftVol: WdfRequestRetrieveUnsafeUserInputBuffer() failed with status 0x%x\n", retrieveInputBufferStatus));
		return FALSE;
	}

	WDFMEMORY inputMemory;
	const NTSTATUS lockInputBufferStatus = WdfRequestProbeAndLockUserBufferForRead(request, inputBuffer, sizeof(KSPROPERTY), &inputMemory);
	if (!NT_SUCCESS(lockInputBufferStatus)) {
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "WinSoftVol: WdfRequestProbeAndLockUserBufferForRead() failed with status 0x%x\n", lockInputBufferStatus));
		return FALSE;
	}

	const KSPROPERTY* const ksProperty = WdfMemoryGetBuffer(inputMemory, /*BufferSize=*/NULL);
	const BOOL isPropertySetRequest = IsEqualGUID(&ksProperty->Set, &KSPROPSETID_Topology) && ksProperty->Id == KSPROPERTY_TOPOLOGY_NODES && ksProperty->Flags & KSPROPERTY_TYPE_GET;
	WdfObjectDelete(inputMemory);
	return isPropertySetRequest;
}

typedef struct {
	WDFMEMORY outputMemory;
} RequestContext;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RequestContext, WinSoftVol_GetRequestContext);

static EVT_WDF_REQUEST_COMPLETION_ROUTINE WinSoftVol_WdfRequestCompletionRoutine;
static void WinSoftVol_WdfRequestCompletionRoutine(IN WDFREQUEST request, IN WDFIOTARGET ioTarget, IN PWDF_REQUEST_COMPLETION_PARAMS requestCompletionParams, IN WDFCONTEXT context) {
	UNREFERENCED_PARAMETER(ioTarget);
	UNREFERENCED_PARAMETER(context);

	if (!NT_SUCCESS(requestCompletionParams->IoStatus.Status)) {
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_INFO_LEVEL, "WinSoftVol: request came back with error status 0x%x\n", requestCompletionParams->IoStatus.Status));
	}
	else {
		size_t ksMultipleItemSize;
		const KSMULTIPLE_ITEM* const ksMultipleItem = WdfMemoryGetBuffer(WinSoftVol_GetRequestContext(request)->outputMemory, &ksMultipleItemSize);
		// TODO: this doesn't work. The size is correct (72 = sizeof(KSMULTIPLE_ITEM) + 4*sizeof(GUID) for 4 nodes) but the pointer is garbage
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_INFO_LEVEL, "WinSoftVol: request completed successfully - ksMultipleItem->Size = %lu, ksMultipleItem->Count = %lu, buffer size = %llu\n", ksMultipleItem->Size, ksMultipleItem->Count, ksMultipleItemSize));
	}

	WdfRequestComplete(request, requestCompletionParams->IoStatus.Status);
}

static BOOL WinSoftVol_InterceptRequest(IN WDFREQUEST request, IN WDFDEVICE device) {
	PVOID outputBuffer;
	size_t outputBufferLength;
	const NTSTATUS retrieveOutputBufferStatus = WdfRequestRetrieveUnsafeUserOutputBuffer(request, sizeof(KSMULTIPLE_ITEM), &outputBuffer, &outputBufferLength);
	if (!NT_SUCCESS(retrieveOutputBufferStatus) || outputBuffer == NULL) {
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "WinSoftVol: WdfRequestRetrieveUnsafeUserOutputBuffer() failed with status 0x%x\n", retrieveOutputBufferStatus));
		return FALSE;
	}

	WDF_OBJECT_ATTRIBUTES requestContextAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&requestContextAttributes, RequestContext);
	RequestContext* requestContext;
	const NTSTATUS allocateContextStatus = WdfObjectAllocateContext(request, &requestContextAttributes, &requestContext);
	if (!NT_SUCCESS(allocateContextStatus)) {
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "WinSoftVol: WdfObjectAllocateContext() failed with status 0x%x\n", allocateContextStatus));
		return FALSE;
	}

	const NTSTATUS lockOutputBufferStatus = WdfRequestProbeAndLockUserBufferForWrite(request, outputBuffer, outputBufferLength, &requestContext->outputMemory);
	if (!NT_SUCCESS(lockOutputBufferStatus)) {
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "WinSoftVol: WdfRequestProbeAndLockUserBufferForWrite() failed with status 0x%x\n", lockOutputBufferStatus));
		return FALSE;
	}

	WdfRequestFormatRequestUsingCurrentType(request);
	WdfRequestSetCompletionRoutine(request, WinSoftVol_WdfRequestCompletionRoutine, WDF_NO_CONTEXT);
	if (!WdfRequestSend(request, WdfDeviceGetIoTarget(device), WDF_NO_SEND_OPTIONS)) {
		const NTSTATUS requestStatus = WdfRequestGetStatus(request);
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "WinSoftVol: intercepting WdfRequestSend() failed with status 0x%x\n", requestStatus));
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
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_INFO_LEVEL, "WinSoftVol: got KS nodes property get request\n"));
		if (WinSoftVol_InterceptRequest(request, device)) return;
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
