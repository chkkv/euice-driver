/*++
  SiUsbXpArm64 - Read.c

  Receive path. The original driver keeps a 0x1000-byte ring buffer in its
  device extension (ext+0x1f8 points at the ring descriptor) and services
  IOCTL 0x222080 from it, parking the IRP in a cancel-safe queue when the
  buffer is empty (page+0x1334a / page+0x1328c).

  A KMDF continuous reader feeds the ring from the bulk-IN pipe.
--*/
#include "SiUsbXp.h"

static EVT_WDF_USB_READER_COMPLETION_ROUTINE SiRxReadComplete;

/* ------------------------------------------------------------------------- */
/* Ring buffer                                                               */
/* ------------------------------------------------------------------------- */

static VOID
SiRxAppend(
    _In_ PSI_DEVICE_CONTEXT Dev,
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ ULONG Length
    )
{
    KIRQL irql;
    ULONG i;

    KeAcquireSpinLock(&Dev->RxLock, &irql);
    for (i = 0; i < Length; i++) {
        if (Dev->RxCount >= SI_RX_BUFFER_SIZE) {
            /* Original sets the overflow byte at ext+0xcd and drops the rest. */
            Dev->RxOverflow = TRUE;
            break;
        }
        Dev->RxBuffer[Dev->RxTail] = Data[i];
        Dev->RxTail = (Dev->RxTail + 1) % SI_RX_BUFFER_SIZE;
        Dev->RxCount++;
    }
    KeReleaseSpinLock(&Dev->RxLock, irql);
}

ULONG
SiRxAvailable(
    _In_ PSI_DEVICE_CONTEXT Dev,
    _Out_ PBOOLEAN Overflow
    )
{
    KIRQL irql;
    ULONG count;

    KeAcquireSpinLock(&Dev->RxLock, &irql);
    count = Dev->RxCount;
    if (Overflow != NULL) {
        *Overflow = Dev->RxOverflow;
    }
    KeReleaseSpinLock(&Dev->RxLock, irql);
    return count;
}

ULONG
SiRxCopyOut(
    _In_ PSI_DEVICE_CONTEXT Dev,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length
    )
{
    KIRQL irql;
    ULONG copied = 0;
    PUCHAR dst = (PUCHAR)Buffer;

    KeAcquireSpinLock(&Dev->RxLock, &irql);
    while (copied < Length && Dev->RxCount > 0) {
        dst[copied] = Dev->RxBuffer[Dev->RxHead];
        Dev->RxHead = (Dev->RxHead + 1) % SI_RX_BUFFER_SIZE;
        Dev->RxCount--;
        copied++;
    }
    KeReleaseSpinLock(&Dev->RxLock, irql);
    return copied;
}

VOID
SiRxPurge(
    _In_ PSI_DEVICE_CONTEXT Dev
    )
{
    KIRQL irql;

    KeAcquireSpinLock(&Dev->RxLock, &irql);
    Dev->RxHead = 0;
    Dev->RxTail = 0;
    Dev->RxCount = 0;
    Dev->RxOverflow = FALSE;
    KeReleaseSpinLock(&Dev->RxLock, irql);
}

/* ------------------------------------------------------------------------- */
/* Pending read completion                                                   */
/* ------------------------------------------------------------------------- */

static VOID
SiRxDrainWorkItem(
    _In_ WDFWORKITEM WorkItem
    )
{
    WDFDEVICE device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
    PSI_DEVICE_CONTEXT dev = SiGetDeviceContext(device);
    WDFREQUEST request;
    NTSTATUS status;

    /*
     * All draining happens at PASSIVE_LEVEL through a work item so that
     * WdfIoQueueRetrieveNextRequest / WdfRequestRetrieveOutputBuffer are
     * always called at a legal IRQL.
     */
    for (;;) {
        PVOID buffer = NULL;
        size_t length = 0;
        ULONG copied;

        if (SiRxAvailable(dev, NULL) == 0) {
            break;
        }

        status = WdfIoQueueRetrieveNextRequest(dev->PendingReads, &request);
        if (!NT_SUCCESS(status)) {
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(request, 1, &buffer, &length);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(request, status);
            continue;
        }

        copied = SiRxCopyOut(dev, buffer, (ULONG)length);
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, copied);
    }
}

VOID
SiRxDrainPending(
    _In_ PSI_DEVICE_CONTEXT Dev
    )
{
    if (Dev->RxDrainWork != NULL) {
        WdfWorkItemEnqueue(Dev->RxDrainWork);
    }
}

static VOID
SiRxReadComplete(
    _In_ WDFUSBPIPE Pipe,
    _In_ WDFMEMORY  Buffer,
    _In_ size_t     NumBytesTransferred,
    _In_ WDFCONTEXT Context
    )
{
    PSI_DEVICE_CONTEXT dev = (PSI_DEVICE_CONTEXT)Context;
    PVOID buffer;
    size_t length = 0;

    UNREFERENCED_PARAMETER(Pipe);

    if (NumBytesTransferred == 0) {
        return;
    }

    buffer = WdfMemoryGetBuffer(Buffer, &length);
    if (buffer != NULL) {
        if (NumBytesTransferred > length) {
            NumBytesTransferred = length;
        }
        SiRxAppend(dev, (const UCHAR*)buffer, (ULONG)NumBytesTransferred);
    }
    SiRxDrainPending(dev);
}

/* ------------------------------------------------------------------------- */
/* Start / stop                                                              */
/* ------------------------------------------------------------------------- */

NTSTATUS
SiRxStart(
    _In_ PSI_DEVICE_CONTEXT Dev
    )
{
    WDF_USB_CONTINUOUS_READER_CONFIG config;
    WDF_WORKITEM_CONFIG workConfig;
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    if (Dev->RxRunning) {
        return STATUS_SUCCESS;
    }

    /* Drain worker is created once and reused. */
    if (Dev->RxDrainWork == NULL) {
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = Dev->Device;
        WDF_WORKITEM_CONFIG_INIT(&workConfig, SiRxDrainWorkItem);
        status = WdfWorkItemCreate(&workConfig,
                                   &attributes,
                                   &Dev->RxDrainWork);
        if (!NT_SUCCESS(status)) {
            Dev->RxDrainWork = NULL;
            /* Draining is an optimisation; reads can still be satisfied
             * synchronously by SiEvtIoDeviceControl. */
        }
    }

    if (!Dev->RxReaderConfigured) {
        WDF_USB_CONTINUOUS_READER_CONFIG_INIT(&config,
                                              SiRxReadComplete,
                                              Dev,
                                              SI_RX_BUFFER_SIZE);
        config.NumPendingReads = 1;
        config.EvtUsbTargetPipeReadersFailed = NULL;

        status = WdfUsbTargetPipeConfigContinuousReader(Dev->BulkIn, &config);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        Dev->RxReaderConfigured = TRUE;
    }

    status = WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(Dev->BulkIn));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    Dev->RxRunning = TRUE;
    return STATUS_SUCCESS;
}

VOID
SiRxStop(
    _In_ PSI_DEVICE_CONTEXT Dev
    )
{
    if (!Dev->RxRunning) {
        return;
    }

    /*
     * Cancel any outstanding bulk-IN transfer and stop the reader. This is
     * the equivalent of the original's pipe abort in IRP_MJ_CLEANUP.
     */
    (VOID) WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(Dev->BulkIn),
                           WdfIoTargetCancelSentIo);
    Dev->RxRunning = FALSE;
}
