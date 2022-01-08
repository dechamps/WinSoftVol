#include <ntddk.h>
#include <Wdm.h>
#include <Wdf.h>
#include <minwindef.h>
#include <ks.h>
#include <windef.h>
#include <ksmedia.h>

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

typedef enum {
	BUFFER_CONTENT_TYPE_NULL,
	BUFFER_CONTENT_TYPE_KSMULTIPLEITEM_GUID,
	BUFFER_CONTENT_TYPE_KSPROPERTY_GET_TOPOLOGY_NODES,
	BUFFER_CONTENT_TYPE_UNKNOWN,
} BufferContentType;

static BufferContentType WinSoftVol_GuessBufferContentType(IN const void* const buffer) {
	if (buffer == NULL) return BUFFER_CONTENT_TYPE_NULL;

	const KSMULTIPLE_ITEM* const ksMultipleItem = buffer;
	if (ksMultipleItem->Size == sizeof(KSMULTIPLE_ITEM) + ksMultipleItem->Count * sizeof(GUID))
		return BUFFER_CONTENT_TYPE_KSMULTIPLEITEM_GUID;

	const KSPROPERTY* const ksProperty = buffer;
	if (IsEqualGUID(&ksProperty->Set, &KSPROPSETID_Topology) && ksProperty->Id == KSPROPERTY_TOPOLOGY_NODES && ksProperty->Flags & KSPROPERTY_TYPE_GET)
		return BUFFER_CONTENT_TYPE_KSPROPERTY_GET_TOPOLOGY_NODES;

	return BUFFER_CONTENT_TYPE_UNKNOWN;
}

static const char* WinSoftVol_DescribeBufferContentType(IN const BufferContentType bufferContentType) {
	switch (bufferContentType) {
	case BUFFER_CONTENT_TYPE_NULL: return "(null)";
	case BUFFER_CONTENT_TYPE_KSMULTIPLEITEM_GUID: return "KSMULTIPLE_ITEM response with GUIDs";
	case BUFFER_CONTENT_TYPE_KSPROPERTY_GET_TOPOLOGY_NODES: return "KSPROPERTY topology nodes get request";
	case BUFFER_CONTENT_TYPE_UNKNOWN: return "unknown/garbage";
	default: return "";
	}
}

static void WinSoftVol_DescribeBuffer(IN const char* const name, IN const void* const buffer) {
	KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_TRACE_LEVEL, "WinSoftVol: %s = 0x%p [%s]\n", name, buffer, WinSoftVol_DescribeBufferContentType(WinSoftVol_GuessBufferContentType(buffer))));
}

static BOOL WinSoftVol_ProbeIrpBuffers(IN const PIRP irp, const BufferContentType expectedBufferContentType) {
	return
		WinSoftVol_GuessBufferContentType(irp->AssociatedIrp.SystemBuffer) == expectedBufferContentType ||
		WinSoftVol_GuessBufferContentType(irp->UserBuffer) == expectedBufferContentType ||
		WinSoftVol_GuessBufferContentType(IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.Type3InputBuffer) == expectedBufferContentType;
}

static void WinSoftVol_PrintIrpBuffers(IN const PIRP irp) {
	WinSoftVol_DescribeBuffer("AssociatedIrp.SystemBuffer", irp->AssociatedIrp.SystemBuffer);
	WinSoftVol_DescribeBuffer("UserBuffer", irp->UserBuffer);
	WinSoftVol_DescribeBuffer("Parameters.DeviceIoControl.Type3InputBuffer", IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.Type3InputBuffer);
}

static EVT_WDF_REQUEST_COMPLETION_ROUTINE WinSoftVol_WdfRequestCompletionRoutine;
static void WinSoftVol_WdfRequestCompletionRoutine(IN WDFREQUEST request, IN WDFIOTARGET ioTarget, IN PWDF_REQUEST_COMPLETION_PARAMS requestCompletionParams, IN WDFCONTEXT context) {
	UNREFERENCED_PARAMETER(ioTarget);
	UNREFERENCED_PARAMETER(context);

	if (!NT_SUCCESS(requestCompletionParams->IoStatus.Status)) {
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_INFO_LEVEL, "WinSoftVol: request came back with error status 0x%x\n", requestCompletionParams->IoStatus.Status));
	}
	else {
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_INFO_LEVEL, "WinSoftVol: in completion routine:\n"));
		WinSoftVol_PrintIrpBuffers(WdfRequestWdmGetIrp(request));
	}

	WdfRequestComplete(request, requestCompletionParams->IoStatus.Status);
}

static BOOL WinSoftVol_InterceptRequest(IN WDFREQUEST request, IN WDFDEVICE device) {
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
	const PIRP irp = WdfRequestWdmGetIrp(request);
	if (WinSoftVol_ProbeIrpBuffers(irp, BUFFER_CONTENT_TYPE_KSPROPERTY_GET_TOPOLOGY_NODES)) {
		KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_INFO_LEVEL, "WinSoftVol: got KS nodes property get request:\n"));
		WinSoftVol_PrintIrpBuffers(irp);
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
