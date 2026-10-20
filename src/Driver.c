/*++
  SiUsbXpArm64 - Driver.c

  Driver entry, device creation, queue setup, file-object semantics and the
  IOCTL dispatch that mirrors SiUSBXp.sys 3.3 (x64).
--*/
#include "SiUsbXp.h"

/* Interface GUIDs (values verified against SiUSBXp.sys .rdata+0x160 / +0x174). */
const GUID SI_GUID_DEVINTERFACE =
    { 0x3C5E1462, 0x5695, 0x4E18, { 0x87, 0x6B, 0xF3, 0xF3, 0xD0, 0x8A, 0xAF, 0x18 } };
const GUID SI_GUID_DEVINTERFACE_ALT =
    { 0x1A1C76CC, 0xE33A, 0x45F4, { 0xAC, 0x7A, 0x1F, 0x27, 0xCB, 0x83, 0xED, 0x8B } };

DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD            SiEvtDeviceAdd;
EVT_WDF_DEVICE_FILE_CREATE           SiEvtFileCreate;
EVT_WDF_FILE_CLEANUP                 SiEvtFileCleanup;
EVT_WDF_FILE_CLOSE                   SiEvtFileClose;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL   SiEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_WRITE            SiEvtIoWrite;
EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE SiEvtIoCanceledOnQueue;
EVT_WDF_DEVICE_PREPARE_HARDWARE        SiEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE        SiEvtReleaseHardware;

/* ------------------------------------------------------------------------- */
/* DriverEntry / EvtDeviceAdd                                                */
/* ------------------------------------------------------------------------- */

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, SiEvtDeviceAdd);
    config.DriverPoolTag = SI_POOL_TAG;

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES,
                             &config,
                             WDF_NO_HANDLE);
    return status;
}

static NTSTATUS
SiQueueInitialize(
    _In_ WDFDEVICE Device
    )
{
    WDF_IO_QUEUE_CONFIG queueConfig;
    PSI_DEVICE_CONTEXT dev = SiGetDeviceContext(Device);
    NTSTATUS status;

    /*
     * Default queue: create/cleanup/close, write and device control.
     * Parallel dispatch because the original driver keeps multiple user IRPs
     * outstanding (a pending read plus control transfers).
     */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = SiEvtIoDeviceControl;
    queueConfig.EvtIoWrite        = SiEvtIoWrite;
    queueConfig.AllowZeroLengthRequests = TRUE;

    status = WdfIoQueueCreate(Device,
                              &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Manual queue holding read requests that could not be satisfied yet. */
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.EvtIoCanceledOnQueue = SiEvtIoCanceledOnQueue;

    status = WdfIoQueueCreate(Device,
                              &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              &dev->PendingReads);
    return status;
}

NTSTATUS
SiEvtDeviceAdd(
    _In_ WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPower;
    WDFDEVICE device;
    PSI_DEVICE_CONTEXT dev;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Driver);

    /*
     * All IOCTLs use METHOD_BUFFERED in the original driver; force the same
     * for every request so SystemBuffer semantics match.
     */
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);

    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig,
                               SiEvtFileCreate,
                               SiEvtFileClose,
                               SiEvtFileCleanup);
    WdfDeviceInitSetFileObjectConfig(DeviceInit,
                                     &fileConfig,
                                     WDF_NO_OBJECT_ATTRIBUTES);

    /* The USB target is created in EvtDevicePrepareHardware, once PnP has
     * assigned resources, mirroring the original's StartDevice path. */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPower);
    pnpPower.EvtDevicePrepareHardware = SiEvtPrepareHardware;
    pnpPower.EvtDeviceReleaseHardware = SiEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPower);

    /*
     * Power: the original forwards power IRPs through SiLib's generic power
     * dispatcher, including PoRequestPowerIrp-based D0 entry. KMDF's default
     * power policy ownership for a function driver matches that behaviour, so
     * no override is needed here.
     */

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SI_DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = NULL;

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    dev = SiGetDeviceContext(device);
    dev->Device = device;
    dev->OpenCount = 0;
    SiRecordInit(dev, 0x10, STATUS_SUCCESS);   /* AddDevice reached */
    KeInitializeSpinLock(&dev->RxLock);

    status = SiQueueInitialize(device);
    if (!NT_SUCCESS(status)) {
        SiRecordInit(dev, 0x11, status);
        return status;
    }

    /* Register the interface GUID the user-mode SI_* API enumerates. */
    status = WdfDeviceCreateDeviceInterface(device,
                                            &SI_GUID_DEVINTERFACE,
                                            NULL);
    if (!NT_SUCCESS(status)) {
        SiRecordInit(dev, 0x12, status);
        return status;
    }

    /* The 3.3 binary carries a second GUID in .rdata; register it as well so
     * any consumer that keys on it still finds the device. */
    (VOID) WdfDeviceCreateDeviceInterface(device,
                                          &SI_GUID_DEVINTERFACE_ALT,
                                          NULL);

    return STATUS_SUCCESS;
}

NTSTATUS
SiEvtPrepareHardware(
    _In_ WDFDEVICE     Device,
    _In_ WDFCMRESLIST  ResourcesRaw,
    _In_ WDFCMRESLIST  ResourcesTranslated
    )
{
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    return SiUsbInitialize(Device);
}

NTSTATUS
SiEvtReleaseHardware(
    _In_ WDFDEVICE     Device,
    _In_ WDFCMRESLIST  ResourcesTranslated
    )
{
    PSI_DEVICE_CONTEXT dev = SiGetDeviceContext(Device);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    SiRxStop(dev);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* File object semantics                                                     */
/* ------------------------------------------------------------------------- */

VOID
SiEvtFileCreate(
    _In_ WDFDEVICE     Device,
    _In_ WDFREQUEST    Request,
    _In_ WDFFILEOBJECT FileObject
    )
{
    PSI_DEVICE_CONTEXT dev = SiGetDeviceContext(Device);
    PSI_FILE_CONTEXT file = SiGetFileContext(FileObject);
    LONG previous;
    NTSTATUS status;

    /*
     * Original IRP_MJ_CREATE handler (page+0x13db4):
     *   lock xadd [ext+0x50], 1 ; if (result > 1) status = 0xC0000022.
     */
    if (!dev->DeviceConfigured) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    previous = InterlockedIncrement(&dev->OpenCount);
    if (previous > 1) {
        InterlockedDecrement(&dev->OpenCount);
        WdfRequestComplete(Request, STATUS_ACCESS_DENIED);
        return;
    }

    file->Opened = TRUE;

    status = SiUsbOpenHandshake(dev);
    if (!NT_SUCCESS(status)) {
        /*
         * The original only fails the create if the handshake itself fails;
         * a rejected status read is tolerated (it treats the returned byte as
         * "mode").
         */
        file->Opened = FALSE;
        InterlockedDecrement(&dev->OpenCount);
        WdfRequestComplete(Request, status);
        return;
    }

    status = SiRxStart(dev);
    if (!NT_SUCCESS(status)) {
        file->Opened = FALSE;
        InterlockedDecrement(&dev->OpenCount);
        WdfRequestComplete(Request, status);
        return;
    }

    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID
SiEvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject
    )
{
    WDFDEVICE device = WdfFileObjectGetDevice(FileObject);
    PSI_DEVICE_CONTEXT dev = SiGetDeviceContext(device);
    PSI_FILE_CONTEXT file = SiGetFileContext(FileObject);
    WDFREQUEST request;

    if (!file->Opened) {
        return;
    }

    /*
     * Original IRP_MJ_CLEANUP (page+0x13d68) cancels every queued request with
     * STATUS_CANCELLED and stops the RX engine.
     */
    SiRxStop(dev);

    while (WdfIoQueueRetrieveNextRequest(dev->PendingReads, &request) == STATUS_SUCCESS) {
        WdfRequestComplete(request, STATUS_CANCELLED);
    }
}

VOID
SiEvtFileClose(
    _In_ WDFFILEOBJECT FileObject
    )
{
    WDFDEVICE device = WdfFileObjectGetDevice(FileObject);
    PSI_DEVICE_CONTEXT dev = SiGetDeviceContext(device);
    PSI_FILE_CONTEXT file = SiGetFileContext(FileObject);

    if (file->Opened) {
        file->Opened = FALSE;
        SiRxPurge(dev);
        InterlockedDecrement(&dev->OpenCount);
    }
}

/* ------------------------------------------------------------------------- */
/* Device control dispatch                                                   */
/* ------------------------------------------------------------------------- */

static NTSTATUS
SiQueryBuffer(
    _In_ WDFREQUEST Request,
    _In_ ULONG Required,
    _Outptr_ PVOID* Buffer
    )
{
    NTSTATUS status;
    size_t length = 0;

    status = WdfRequestRetrieveOutputBuffer(Request, Required, Buffer, &length);
    if (!NT_SUCCESS(status)) {
        return status == STATUS_BUFFER_TOO_SMALL ? STATUS_BUFFER_TOO_SMALL : status;
    }
    if (length < Required) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    return STATUS_SUCCESS;
}

VOID
SiEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
    )
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PSI_DEVICE_CONTEXT dev = SiGetDeviceContext(device);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;
    PVOID outBuffer = NULL;
    size_t inLength = 0;
    size_t outLength = 0;

    switch (IoControlCode) {

    case SI_IOCTL_GET_DEVICE_DESCRIPTOR:
    {
        /* page+0x136a7: requires OutputBufferLength >= 0x12 */
        status = SiQueryBuffer(Request, SI_DEVICE_DESCRIPTOR_SIZE, &outBuffer);
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(outBuffer, &dev->DeviceDescriptor, SI_DEVICE_DESCRIPTOR_SIZE);
            information = SI_DEVICE_DESCRIPTOR_SIZE;
        }
        break;
    }

    case SI_IOCTL_GET_PART_STRING:
    {
        /* page+0x13666: requires OutputBufferLength >= 9 */
        status = SiQueryBuffer(Request, SI_PART_STRING_SIZE, &outBuffer);
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(outBuffer, dev->PartString, SI_PART_STRING_SIZE);
            information = SI_PART_STRING_SIZE;
        }
        break;
    }

    case SI_IOCTL_GET_PART_RECORD:
    {
        /* page+0x1362f: requires OutputBufferLength >= 0x30 */
        status = SiQueryBuffer(Request, SI_PART_RECORD_SIZE, &outBuffer);
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(outBuffer, dev->PartRecord, SI_PART_RECORD_SIZE);
            information = SI_PART_RECORD_SIZE;
        }
        break;
    }

    case SI_IOCTL_GET_PRODUCT_STRING:
    {
        /* page+0x135d1: zero-fills the whole output buffer, then copies
         * ProductStringLength bytes; Information = ProductStringLength. */
        status = WdfRequestRetrieveOutputBuffer(Request, 0, &outBuffer, &outLength);
        if (NT_SUCCESS(status)) {
            if (outLength < dev->ProductStringLength) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else {
                RtlZeroMemory(outBuffer, outLength);
                RtlCopyMemory(outBuffer, dev->ProductString, dev->ProductStringLength);
                information = dev->ProductStringLength;
            }
        }
        break;
    }

    case SI_IOCTL_CONTROL_OUT:
    {
        /*
         * page+0x1351c. Buffer holds the setup packet plus payload.
         * Function selection: bRequest == 0x13 -> vendor interface request,
         * otherwise a vendor device request. wLength bytes at Data are sent.
         */
        PSI_CONTROL_PACKET packet;
        ULONG transferred = 0;

        status = WdfRequestRetrieveInputBuffer(Request, SI_CONTROL_HEADER_SIZE,
                                               (PVOID*)&packet, &inLength);
        if (!NT_SUCCESS(status)) {
            break;
        }
        if (inLength < (size_t)SI_CONTROL_HEADER_SIZE + packet->Length) {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }
        if (OutputBufferLength == 0) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        status = SiUsbControlTransfer(dev,
                                      FALSE,
                                      packet->RequestType,
                                      packet->Request,
                                      packet->Value,
                                      packet->Index,
                                      packet->Data,
                                      packet->Length,
                                      &transferred);
        /* page+0x13512: Information = InputBufferLength. */
        information = InputBufferLength;
        break;
    }

    case SI_IOCTL_CONTROL_IN:
    {
        /*
         * page+0x13476. Same packet, but data flows device -> host and the
         * result is written back into the caller's SystemBuffer.
         */
        PSI_CONTROL_PACKET packet;
        ULONG transferred = 0;

        status = WdfRequestRetrieveInputBuffer(Request, SI_CONTROL_HEADER_SIZE,
                                               (PVOID*)&packet, &inLength);
        if (!NT_SUCCESS(status)) {
            break;
        }
        if (inLength < (size_t)SI_CONTROL_HEADER_SIZE + packet->Length) {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }
        if (OutputBufferLength == 0) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        {
            /* In/out buffer is the same SystemBuffer for METHOD_BUFFERED, so
             * the payload can be received in place. */
            PUCHAR payload = packet->Data;
            status = SiUsbControlTransfer(dev,
                                          TRUE,
                                          packet->RequestType,
                                          packet->Request,
                                          packet->Value,
                                          packet->Index,
                                          payload,
                                          packet->Length,
                                          &transferred);
            /* page+0x13512: Information = InputBufferLength (not the number of
             * bytes the device returned). */
            information = InputBufferLength;
            UNREFERENCED_PARAMETER(transferred);
        }
        break;
    }

    case SI_IOCTL_READ:
    {
        /*
         * page+0x1334a. Return buffered RX data if available; otherwise leave
         * the IRP pending in the manual queue (the original inserts it into a
         * cancel-safe queue at ext+0x228 and returns STATUS_PENDING).
         */
        ULONG available;

        status = WdfRequestRetrieveOutputBuffer(Request, 1, &outBuffer, &outLength);
        if (!NT_SUCCESS(status)) {
            break;
        }

        available = SiRxCopyOut(dev, outBuffer, (ULONG)outLength);
        if (available != 0) {
            information = available;
            status = STATUS_SUCCESS;
            break;
        }

        status = WdfRequestForwardToIoQueue(Request, dev->PendingReads);
        if (!NT_SUCCESS(status)) {
            break;
        }
        return; /* request ownership transferred */
    }

    case SI_IOCTL_CHECK_RX_QUEUE:
    {
        /*
         * page+0x1328c. Returns 4 bytes:
         *   [0..2] 24-bit little-endian byte count
         *   [3]    flags: 0x01 overflow, 0x02 data available
         */
        BOOLEAN overflow = FALSE;
        ULONG available;
        PUCHAR p;

        status = SiQueryBuffer(Request, 4, &outBuffer);
        if (!NT_SUCCESS(status)) {
            break;
        }

        available = SiRxAvailable(dev, &overflow);
        p = (PUCHAR)outBuffer;
        p[0] = (UCHAR)(available & 0xFF);
        p[1] = (UCHAR)((available >> 8) & 0xFF);
        p[2] = (UCHAR)((available >> 16) & 0xFF);
        p[3] = (UCHAR)((overflow ? SI_RX_FLAG_OVERFLOW : 0) |
                       (available ? SI_RX_FLAG_DATA : 0));
        information = 4;
        status = STATUS_SUCCESS;
        break;
    }

    case SI_IOCTL_FLUSH:
    {
        /*
         * page+0x130d0. One input byte selects the queues:
         *   bit2 (0x04) purge RX, bit3 (0x08) purge TX, 0x0C both.
         * Information is 0.
         */
        PUCHAR flags;

        status = WdfRequestRetrieveInputBuffer(Request, 1, (PVOID*)&flags, &inLength);
        if (!NT_SUCCESS(status)) {
            break;
        }
        if (OutputBufferLength == 0) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (flags[0] & SI_FLUSH_RX) {
            SiRxPurge(dev);
        }
        if (flags[0] & SI_FLUSH_TX) {
            /*
             * Writes are synchronous here, so there is no TX queue to purge.
             * The original resets its write queue (page+0x13109: memset of the
             * 0x1000-byte TX buffer and zeroing of the pending byte count),
             * which is a no-op when nothing is queued.
             */
        }
        information = 0;
        status = STATUS_SUCCESS;
        break;
    }

    case SI_IOCTL_RESET_DEVICE:
    {
        /*
         * SI_ResetDevice in the DLL issues 0x22208F (METHOD_NEITHER). The
         * x64 driver's table only covers up to 0x222088 and the default arm
         * returns STATUS_INVALID_DEVICE_REQUEST, which the DLL maps to error
         * code 7. Reproduce that for 0x22208F; implement the documented
         * 0x22208C reset for completeness.
         */
        SiRxStop(dev);
        SiRxPurge(dev);
        SiUsbResetPipes(dev);
        (VOID) SiRxStart(dev);
        information = 0;
        status = STATUS_SUCCESS;
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, information);
}

VOID
SiEvtIoWrite(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     Length
    )
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PSI_DEVICE_CONTEXT dev = SiGetDeviceContext(device);
    WDFMEMORY memory;
    WDF_MEMORY_DESCRIPTOR memoryDescriptor;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG bytesWritten = 0;
    NTSTATUS status;

    /* page+0x1489f: writes larger than 0x1000 fail with STATUS_INVALID_BUFFER_SIZE. */
    if (Length > SI_MAX_TRANSFER) {
        WdfRequestComplete(Request, STATUS_INVALID_BUFFER_SIZE);
        return;
    }

    if (Length == 0) {
        /* page+0x1486f: zero-length write completes immediately with success. */
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
        return;
    }

    status = WdfRequestRetrieveInputMemory(Request, &memory);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&memoryDescriptor, memory, NULL);

    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout = WDF_REL_TIMEOUT_IN_MS(SI_IO_TIMEOUT_MS);

    status = WdfUsbTargetPipeWriteSynchronously(dev->BulkOut,
                                                NULL,
                                                &options,
                                                &memoryDescriptor,
                                                &bytesWritten);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, bytesWritten);
}

VOID
SiEvtIoCanceledOnQueue(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request
    )
{
    UNREFERENCED_PARAMETER(Queue);
    WdfRequestComplete(Request, STATUS_CANCELLED);
}
