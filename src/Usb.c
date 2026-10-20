/*++
  SiUsbXpArm64 - Usb.c

  USB target creation, descriptor caching, control transfers and the
  open-time vendor handshake performed by SiUSBXp.sys!0x13db4.
--*/
#include "SiUsbXp.h"

static NTSTATUS
SiCacheDescriptors(
    _In_ WDFDEVICE Device,
    _In_ PSI_DEVICE_CONTEXT Dev
    )
{
    PUSB_DEVICE_DESCRIPTOR descriptor;
    USHORT langId = 0x0409;
    USHORT length;
    NTSTATUS status;

    descriptor = WdfUsbTargetDeviceGetDeviceDescriptor(Dev->UsbDevice);
    if (descriptor == NULL) {
        return STATUS_UNSUCCESSFUL;
    }
    RtlCopyMemory(&Dev->DeviceDescriptor, descriptor, sizeof(USB_DEVICE_DESCRIPTOR));

    /* Serial number string: IOCTL 0x22200C returns a 9-byte buffer (8
     * characters plus terminator) exactly like the original's cached copy. */
    RtlZeroMemory(Dev->PartString, sizeof(Dev->PartString));
    if (descriptor->iSerialNumber != 0) {
        length = (USHORT)sizeof(Dev->PartString);
        status = WdfUsbTargetDeviceQueryString(Dev->UsbDevice,
                                               NULL,
                                               NULL,
                                               (PUSHORT)Dev->PartString,
                                               &length,
                                               langId,
                                               descriptor->iSerialNumber);
        if (!NT_SUCCESS(status) && status != STATUS_BUFFER_TOO_SMALL) {
            /* Non-fatal: leave the buffer zeroed. */
            RtlZeroMemory(Dev->PartString, sizeof(Dev->PartString));
        }
    }

    /* Product string for IOCTL 0x222018. WdfUsbTargetDeviceQueryString
     * returns the byte count (including the terminating NUL) in Length. */
    RtlZeroMemory(Dev->ProductString, sizeof(Dev->ProductString));
    Dev->ProductStringLength = 0;
    if (descriptor->iProduct != 0) {
        length = (USHORT)sizeof(Dev->ProductString);
        status = WdfUsbTargetDeviceQueryString(Dev->UsbDevice,
                                               NULL,
                                               NULL,
                                               (PUSHORT)Dev->ProductString,
                                               &length,
                                               langId,
                                               descriptor->iProduct);
        if (NT_SUCCESS(status) || status == STATUS_BUFFER_TOO_SMALL) {
            Dev->ProductStringLength = length;
        }
    }

    /*
     * Part record (IOCTL 0x222014).
     *
     * The original keeps a 0x30-byte record in the device extension
     * (ext+0x78) that it also inspects during open: byte 0x1A & 0x0F selects
     * whether the 0xFF/0x370B status read is performed, and SI_SetLineControl
     * validates byte [1] against the values 1..3.
     *
     * The exact layout is device/firmware specific and is not derivable from
     * the binary alone. A record with part family 1 is synthesised here so
     * that the line-control validation accepts the device; see ANALYSIS.md.
     */
    RtlZeroMemory(Dev->PartRecord, sizeof(Dev->PartRecord));
    Dev->PartRecord[0] = 0x00;
    Dev->PartRecord[1] = 0x01;   /* part family, 1..3 are valid */
    Dev->PartRecord[2] = 0x00;

    UNREFERENCED_PARAMETER(Device);
    return STATUS_SUCCESS;
}

static NTSTATUS
SiSelectPipes(
    _In_ PSI_DEVICE_CONTEXT Dev
    )
{
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS params;
    WDF_USB_PIPE_INFORMATION pipeInfo;
    WDFUSBPIPE pipe;
    UCHAR i, count;
    WDFUSBINTERFACE interface;
    NTSTATUS status;

    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&params);
    status = WdfUsbTargetDeviceSelectConfig(Dev->UsbDevice,
                                            WDF_NO_OBJECT_ATTRIBUTES,
                                            &params);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (WdfUsbTargetDeviceGetNumInterfaces(Dev->UsbDevice) == 0) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    interface = WdfUsbTargetDeviceGetInterface(Dev->UsbDevice, 0);
    count = WdfUsbInterfaceGetNumEndpoints(interface, 0);

    /*
     * The original parses the configuration for exactly two endpoints and
     * fails with STATUS_REVISION_MISMATCH if bNumEndpoints != 2
     * (page+0x14496). Endpoint 0x83/0x02 pair is accepted as bulk IN/OUT;
     * any bulk pair is bound here so the driver also works with variants.
     */
    for (i = 0; i < count; i++) {
        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);
        pipe = WdfUsbInterfaceGetConfiguredPipe(interface, i, &pipeInfo);
        if (pipe == NULL) {
            continue;
        }
        if (pipeInfo.PipeType != WdfUsbPipeTypeBulk) {
            continue;
        }
        if ((pipeInfo.EndpointAddress & 0x80) != 0) {
            if (Dev->BulkIn == NULL) {
                Dev->BulkIn = pipe;
                Dev->BulkInPacketSize = (UCHAR)pipeInfo.MaximumPacketSize;
            }
        } else {
            if (Dev->BulkOut == NULL) {
                Dev->BulkOut = pipe;
                Dev->BulkOutPacketSize = (UCHAR)pipeInfo.MaximumPacketSize;
            }
        }
    }

    if (Dev->BulkIn == NULL || Dev->BulkOut == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
SiUsbInitialize(
    _In_ WDFDEVICE Device
    )
{
    PSI_DEVICE_CONTEXT dev = SiGetDeviceContext(Device);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_USB_DEVICE_CREATE_CONFIG config;
    NTSTATUS status;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;

    WDF_USB_DEVICE_CREATE_CONFIG_INIT(&config, USBD_CLIENT_CONTRACT_VERSION_602);

    status = WdfUsbTargetDeviceCreateWithParameters(Device,
                                                    &config,
                                                    &attributes,
                                                    &dev->UsbDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Configuration selection happens lazily; a USB ICE may be plugged in
     * before the firmware is ready, so retry once with a short delay. */
    status = SiSelectPipes(dev);
    if (!NT_SUCCESS(status)) {
        LARGE_INTEGER delay;
        delay.QuadPart = -200 * 1000 * 10;   /* 200 ms */
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        status = SiSelectPipes(dev);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    dev->DeviceConfigured = TRUE;
    return SiCacheDescriptors(Device, dev);
}

NTSTATUS
SiUsbControlTransfer(
    _In_  PSI_DEVICE_CONTEXT Dev,
    _In_  BOOLEAN DeviceToHost,
    _In_  UCHAR RequestType,
    _In_  UCHAR Request,
    _In_  USHORT Value,
    _In_  USHORT Index,
    _Inout_updates_bytes_opt_(Length) PVOID Buffer,
    _In_  USHORT Length,
    _Out_opt_ PULONG Transferred
    )
{
    WDF_USB_CONTROL_SETUP_PACKET setup;
    WDF_MEMORY_DESCRIPTOR memory;
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFREQUEST request = WDF_NO_HANDLE;
    NTSTATUS status;
    ULONG_PTR bytes = 0;

    if (Transferred != NULL) {
        *Transferred = 0;
    }

    /*
     * Direction is taken from the IOCTL (0x222024 = OUT, 0x222028 = IN),
     * exactly like the original, which always builds URB_FUNCTION_VENDOR_*
     * with a fixed TransferFlags direction and only takes the type/recipient
     * bits from the caller's bmRequestType.
     */
    WDF_USB_CONTROL_SETUP_PACKET_INIT(&setup,
                                      DeviceToHost ? BmRequestDeviceToHost
                                                   : BmRequestHostToDevice,
                                      (WDF_USB_BMREQUEST_RECIPIENT)(RequestType & 0x1F),
                                      (WDF_USB_BMREQUEST_TYPE)((RequestType >> 5) & 0x03),
                                      Request,
                                      Value,
                                      Index);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memory, (Length != 0) ? Buffer : NULL, Length);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Dev->UsbDevice;

    status = WdfRequestCreate(&attributes, NULL, &request);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout = WDF_REL_TIMEOUT_IN_MS(SI_CONTROL_TIMEOUT_MS);

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(Dev->UsbDevice,
                                                                request,
                                                                &options,
                                                                &setup,
                                                                &memory,
                                                                &bytes);
    if (NT_SUCCESS(status)) {
        if (Transferred != NULL) {
            *Transferred = (ULONG)bytes;
        }
    } else if (status == STATUS_IO_TIMEOUT) {
        /* Keep the same status the USB stack would produce. */
    }

    WdfObjectDelete(request);
    return status;
}

NTSTATUS
SiUsbOpenHandshake(
    _In_ PSI_DEVICE_CONTEXT Dev
    )
{
    NTSTATUS status;
    UCHAR mode = 0;
    ULONG transferred = 0;

    /*
     * Sequence from page+0x13db4 (IRP_MJ_CREATE):
     *
     *   1) vendor request 0, wValue 0xFFFF, no data   (via sub_111E4)
     *   2) vendor request 1, wValue 0x2000, no data
     *   3) vendor request 0xFF, wValue 0x370B, 1 byte IN, only when
     *      (partRecord[0x1A] & 0x0F) is not 2 or 3.
     *
     * The URBs are built with URB_FUNCTION_VENDOR_DEVICE, so the wire
     * bmRequestType is 0x40 (OUT) / 0xC0 (IN).
     */
    status = SiUsbControlTransfer(Dev, FALSE, 0x40, 0x00, 0xFFFF, 0,
                                  NULL, 0, NULL);
    if (!NT_SUCCESS(status)) {
        DbgPrint("SiUsbXp: vendor 0x00/0xFFFF failed 0x%08X\n", status);
        return status;
    }

    status = SiUsbControlTransfer(Dev, FALSE, 0x40, 0x01, 0x2000, 0,
                                  NULL, 0, NULL);
    if (!NT_SUCCESS(status)) {
        DbgPrint("SiUsbXp: vendor 0x01/0x2000 failed 0x%08X\n", status);
        return status;
    }

    if ((Dev->PartRecord[0x1A] & 0x0F) != 2 && (Dev->PartRecord[0x1A] & 0x0F) != 3) {
        status = SiUsbControlTransfer(Dev, TRUE, 0xC0, 0xFF, 0x370B, 0,
                                      &mode, 1, &transferred);
        if (NT_SUCCESS(status) && transferred == 1) {
            Dev->ModeByte = mode;
        } else {
            /* The original stores whatever the transfer returned; a failed
             * status read is not fatal to IRP_MJ_CREATE. */
            Dev->ModeByte = 0;
        }
    } else {
        Dev->ModeByte = 1;   /* page+0x13f1e: parts 2/3 default to 1 */
    }

    return STATUS_SUCCESS;
}

VOID
SiUsbResetPipes(
    _In_ PSI_DEVICE_CONTEXT Dev
    )
{
    if (Dev->BulkIn != NULL) {
        (VOID) WdfUsbTargetPipeResetSynchronously(Dev->BulkIn,
                                                  WDF_NO_HANDLE,
                                                  NULL);
    }
    if (Dev->BulkOut != NULL) {
        (VOID) WdfUsbTargetPipeResetSynchronously(Dev->BulkOut,
                                                  WDF_NO_HANDLE,
                                                  NULL);
    }
}
