/*++
  SiUsbXpArm64 - USBXpress-compatible USB ICE function driver for Windows ARM64.
  Clean-room reimplementation from static analysis of SiUSBXp.sys 3.3 (x64).

  Shared definitions: IOCTL interface, control-packet layout, device context.
--*/
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <wdfusb.h>

/* Pool tag used by the original driver: immediate 0x53694558 ('X','E','i','S'). */
#define SI_POOL_TAG 'XEiS'

/*
 * IOCTL surface (all METHOD_BUFFERED, FILE_DEVICE_UNKNOWN = 0x22).
 * Recovered from the dispatch table at SiUSBXp.sys!0x13076 (subtract chain
 * starting at 0x222008).
 */
#define SI_IOCTL_GET_DEVICE_DESCRIPTOR  0x222008  /* func 0x802, METHOD_BUFFERED */
#define SI_IOCTL_GET_PART_STRING        0x22200C  /* func 0x803, METHOD_BUFFERED */
#define SI_IOCTL_GET_PART_RECORD        0x222014  /* func 0x805, METHOD_BUFFERED */
#define SI_IOCTL_GET_PRODUCT_STRING     0x222018  /* func 0x806, METHOD_BUFFERED */
#define SI_IOCTL_CONTROL_OUT            0x222024  /* func 0x809, METHOD_BUFFERED */
#define SI_IOCTL_CONTROL_IN             0x222028  /* func 0x80A, METHOD_BUFFERED */
#define SI_IOCTL_READ                   0x222080  /* func 0x820, METHOD_BUFFERED */
#define SI_IOCTL_CHECK_RX_QUEUE         0x222084  /* func 0x821, METHOD_BUFFERED */
#define SI_IOCTL_FLUSH                  0x222088  /* func 0x822, METHOD_BUFFERED */
#define SI_IOCTL_RESET_DEVICE           0x22208C  /* func 0x823, METHOD_BUFFERED */
/* The 3.3 x64 binary also probes 0x22208F (METHOD_NEITHER) from SI_ResetDevice;
 * the dispatch table has no entry for it and returns STATUS_INVALID_DEVICE_REQUEST.
 * See ANALYSIS.md, section "Reset". */

/* Hard limits observed in the original driver. */
#define SI_MAX_TRANSFER            0x1000u   /* page+0x189f: cmp edi,0x1000 */
#define SI_RX_BUFFER_SIZE          0x1000u   /* page+0x13112: buffer clear, 0x1000 */
#define SI_CONTROL_TIMEOUT_MS      5000u
#define SI_IO_TIMEOUT_MS           5000u
#define SI_DEVICE_DESCRIPTOR_SIZE  0x12u
#define SI_PART_STRING_SIZE        0x09u
#define SI_PART_RECORD_SIZE        0x30u

/*
 * Device interfaces registered by SiUSBXp.sys 3.3 (.rdata+0x160 / .rdata+0x174).
 * The GUID values are defined in Driver.c to keep the header free of
 * DEFINE_GUID instantiations.
 */
extern const GUID SI_GUID_DEVINTERFACE;
extern const GUID SI_GUID_DEVINTERFACE_ALT;

/*
 * Control-transfer packet accepted by IOCTL 0x222024 / 0x222028.
 *
 * Layout verified against SiUSBXp.dll (SI_SetBreak, SI_GetModemStatus) and the
 * kernel handlers at page+0x1351c / page+0x13476:
 *   [0]   bmRequestType
 *   [1]   bRequest
 *   [2]   wValue
 *   [4]   wIndex
 *   [6]   wLength
 *   [8..] data
 */
typedef struct _SI_CONTROL_PACKET {
    UCHAR  RequestType;
    UCHAR  Request;
    USHORT Value;
    USHORT Index;
    USHORT Length;
    UCHAR  Data[1];
} SI_CONTROL_PACKET, *PSI_CONTROL_PACKET;

#define SI_CONTROL_HEADER_SIZE  FIELD_OFFSET(SI_CONTROL_PACKET, Data)

/* Flush flags (SI_FlushBuffers): 4 = purge RX, 8 = purge TX, 0xC = both. */
#define SI_FLUSH_RX 0x04
#define SI_FLUSH_TX 0x08

/* Check-RX-queue flags byte (page+0x132df..0x132f3). */
#define SI_RX_FLAG_OVERFLOW   0x01
#define SI_RX_FLAG_DATA       0x02

typedef struct _SI_FILE_CONTEXT {
    BOOLEAN Opened;
} SI_FILE_CONTEXT, *PSI_FILE_CONTEXT;

typedef struct _SI_DEVICE_CONTEXT {
    WDFDEVICE     Device;
    WDFUSBDEVICE  UsbDevice;
    WDFUSBPIPE    BulkIn;
    WDFUSBPIPE    BulkOut;
    UCHAR         BulkInPacketSize;
    UCHAR         BulkOutPacketSize;
    BOOLEAN       DeviceConfigured;

    WDFQUEUE      PendingReads;

    /* RX ring: the original keeps a 0x1000-byte buffer plus head/tail/count
     * at extension offsets 0x1f8 and a "flush" flag at 0x200/0xcd. */
    KSPIN_LOCK     RxLock;
    UCHAR          RxBuffer[SI_RX_BUFFER_SIZE];
    ULONG          RxHead;
    ULONG          RxTail;
    ULONG          RxCount;
    BOOLEAN        RxOverflow;

    BOOLEAN        RxRunning;
    BOOLEAN        RxReaderConfigured;
    WDFWORKITEM    RxDrainWork;

    /* Single-open enforcement: original increments a counter at ext+0x50 and
     * fails the second IRP_MJ_CREATE with STATUS_ACCESS_DENIED. */
    LONG           OpenCount;

    /* State cached for the query IOCTLs. */
    USB_DEVICE_DESCRIPTOR DeviceDescriptor;
    UCHAR          PartString[SI_PART_STRING_SIZE];
    UCHAR          PartRecord[SI_PART_RECORD_SIZE];
    UCHAR          ProductString[256];
    USHORT         ProductStringLength;

    /* Mode byte returned by the open handshake (0xFF/0x370B read),
     * stored at ext+0x200 by the original. */
    UCHAR          ModeByte;

    /* Diagnostics: written to the device's hardware key so a Code 10 can be
     * traced without a kernel debugger. 0 = success. */
    ULONG          InitStage;
    NTSTATUS       InitStatus;
} SI_DEVICE_CONTEXT, *PSI_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SI_DEVICE_CONTEXT, SiGetDeviceContext)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SI_FILE_CONTEXT, SiGetFileContext)

/* Usb.c */
NTSTATUS SiUsbInitialize(_In_ WDFDEVICE Device);
NTSTATUS SiUsbOpenHandshake(_In_ PSI_DEVICE_CONTEXT Dev);
NTSTATUS SiUsbControlTransfer(
    _In_  PSI_DEVICE_CONTEXT Dev,
    _In_  BOOLEAN DeviceToHost,
    _In_  UCHAR RequestType,
    _In_  UCHAR Request,
    _In_  USHORT Value,
    _In_  USHORT Index,
    _Inout_updates_bytes_opt_(Length) PVOID Buffer,
    _In_  USHORT Length,
    _Out_opt_ PULONG Transferred);
VOID SiUsbResetPipes(_In_ PSI_DEVICE_CONTEXT Dev);

/* Read.c */
NTSTATUS SiRxStart(_In_ PSI_DEVICE_CONTEXT Dev);
VOID SiRxStop(_In_ PSI_DEVICE_CONTEXT Dev);
VOID SiRxPurge(_In_ PSI_DEVICE_CONTEXT Dev);
ULONG SiRxAvailable(_In_ PSI_DEVICE_CONTEXT Dev, _Out_ PBOOLEAN Overflow);
ULONG SiRxCopyOut(_In_ PSI_DEVICE_CONTEXT Dev, _Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length);
VOID SiRxDrainPending(_In_ PSI_DEVICE_CONTEXT Dev);
