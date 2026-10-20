/*
 * SiUsbXpTest.c - user-mode conformance / smoke test for the ARM64
 * USBXpress-compatible driver. Mirrors the calls that SiUSBXp.dll 3.1 makes.
 *
 * Build (x86, x64 or ARM64 native):
 *   cl /W4 /D_CRT_SECURE_NO_WARNINGS SiUsbXpTest.c setupapi.lib
 *
 * Run:
 *   SiUsbXpTest.exe            - query device, modem status, set 115200 baud
 *   SiUsbXpTest.exe echo       - additionally write a pattern and read it back
 */
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DEFINE_GUID(GUID_DEVINTERFACE_SIUSBXP,
    0x3C5E1462, 0x5695, 0x4E18, 0x87, 0x6B, 0xF3, 0xF3, 0xD0, 0x8A, 0xAF, 0x18);
DEFINE_GUID(GUID_DEVINTERFACE_SIUSBXP_ALT,
    0x1A1C76CC, 0xE33A, 0x45F4, 0xAC, 0x7A, 0x1F, 0x27, 0xCB, 0x83, 0xED, 0x8B);

#define SI_IOCTL_GET_DEVICE_DESCRIPTOR  0x222008
#define SI_IOCTL_GET_PART_STRING        0x22200C
#define SI_IOCTL_GET_PART_RECORD        0x222014
#define SI_IOCTL_GET_PRODUCT_STRING     0x222018
#define SI_IOCTL_CONTROL_OUT            0x222024
#define SI_IOCTL_CONTROL_IN             0x222028
#define SI_IOCTL_READ                   0x222080
#define SI_IOCTL_CHECK_RX_QUEUE         0x222084
#define SI_IOCTL_FLUSH                  0x222088

#pragma pack(push, 1)
typedef struct {
    UCHAR  bLength;
    UCHAR  bDescriptorType;
    USHORT bcdUSB;
    UCHAR  bDeviceClass;
    UCHAR  bDeviceSubClass;
    UCHAR  bDeviceProtocol;
    UCHAR  bMaxPacketSize0;
    USHORT idVendor;
    USHORT idProduct;
    USHORT bcdDevice;
    UCHAR  iManufacturer;
    UCHAR  iProduct;
    UCHAR  iSerialNumber;
    UCHAR  bNumConfigurations;
} SI_DEVICE_DESCRIPTOR;

typedef struct {
    UCHAR  RequestType;
    UCHAR  Request;
    USHORT Value;
    USHORT Index;
    USHORT Length;
    UCHAR  Data[1];
} SI_CONTROL_PACKET;
#pragma pack(pop)

static BOOL
FindDevice(char *Path, size_t PathLen)
{
    HDEVINFO info;
    SP_DEVICE_INTERFACE_DATA iface;
    DWORD index = 0;

    info = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_SIUSBXP, NULL, NULL,
                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    iface.cbSize = sizeof(iface);
    if (!SetupDiEnumDeviceInterfaces(info, NULL, &GUID_DEVINTERFACE_SIUSBXP, index, &iface)) {
        SetupDiDestroyDeviceInfoList(info);
        return FALSE;
    }

    {
        DWORD needed = 0;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail;

        SetupDiGetDeviceInterfaceDetailA(info, &iface, NULL, 0, &needed, NULL);
        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(needed);
        if (!detail) {
            SetupDiDestroyDeviceInfoList(info);
            return FALSE;
        }
        detail->cbSize = sizeof(*detail);
        if (SetupDiGetDeviceInterfaceDetailA(info, &iface, detail, needed, &needed, NULL)) {
            strncpy_s(Path, PathLen, detail->DevicePath, _TRUNCATE);
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(info);
    return Path[0] != '\0';
}

static BOOL
ControlOut(HANDLE h, UCHAR requestType, UCHAR request, USHORT value, USHORT index,
           const void *data, USHORT length)
{
    size_t size = 8 + length;
    UCHAR *buf = (UCHAR *)calloc(1, size);
    DWORD returned = 0;
    BOOL ok;

    if (!buf) {
        return FALSE;
    }
    buf[0] = requestType;
    buf[1] = request;
    memcpy(buf + 2, &value, 2);
    memcpy(buf + 4, &index, 2);
    memcpy(buf + 6, &length, 2);
    if (length && data) {
        memcpy(buf + 8, data, length);
    }

    ok = DeviceIoControl(h, SI_IOCTL_CONTROL_OUT, buf, (DWORD)size,
                         buf, (DWORD)size, &returned, NULL);
    free(buf);
    return ok;
}

static BOOL
ControlIn(HANDLE h, UCHAR requestType, UCHAR request, USHORT value, USHORT index,
          void *data, USHORT length)
{
    size_t size = 8 + length;
    UCHAR *buf = (UCHAR *)calloc(1, size);
    DWORD returned = 0;
    BOOL ok;

    if (!buf) {
        return FALSE;
    }
    buf[0] = requestType;
    buf[1] = request;
    memcpy(buf + 2, &value, 2);
    memcpy(buf + 4, &index, 2);
    memcpy(buf + 6, &length, 2);

    ok = DeviceIoControl(h, SI_IOCTL_CONTROL_IN, buf, (DWORD)size,
                         buf, (DWORD)size, &returned, NULL);
    if (ok && length) {
        memcpy(data, buf + 8, length);
    }
    free(buf);
    return ok;
}

static BOOL
CheckRxQueue(HANDLE h, ULONG *Available, UCHAR *Flags)
{
    UCHAR out[4] = {0};
    DWORD returned = 0;

    if (!DeviceIoControl(h, SI_IOCTL_CHECK_RX_QUEUE, NULL, 0,
                         out, sizeof(out), &returned, NULL)) {
        return FALSE;
    }
    if (Available) {
        *Available = (ULONG)out[0] | ((ULONG)out[1] << 8) | ((ULONG)out[2] << 16);
    }
    if (Flags) {
        *Flags = out[3];
    }
    return TRUE;
}

static BOOL
FlushBuffers(HANDLE h, UCHAR flags)
{
    DWORD returned = 0;
    return DeviceIoControl(h, SI_IOCTL_FLUSH, &flags, 1, NULL, 0, &returned, NULL);
}

int
main(int argc, char **argv)
{
    char path[512] = {0};
    HANDLE h;
    DWORD returned = 0;

    if (!FindDevice(path, sizeof(path))) {
        printf("device not found (GUID 3C5E1462-5695-4E18-876B-F3F3D08AAF18)\n");
        return 1;
    }
    printf("device: %s\n", path);

    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("CreateFile failed: %lu\n", GetLastError());
        return 1;
    }

    {
        SI_DEVICE_DESCRIPTOR desc;
        DWORD got = 0;

        if (DeviceIoControl(h, SI_IOCTL_GET_DEVICE_DESCRIPTOR, NULL, 0,
                            &desc, sizeof(desc), &got, NULL)) {
            printf("device descriptor: VID=%04X PID=%04X bcd=%04X strings=%u/%u/%u\n",
                   desc.idVendor, desc.idProduct, desc.bcdDevice,
                   desc.iManufacturer, desc.iProduct, desc.iSerialNumber);
        } else {
            printf("GET_DEVICE_DESCRIPTOR failed: %lu\n", GetLastError());
        }
    }

    {
        UCHAR serial[9] = {0};
        UCHAR part[0x30] = {0};
        char product[256] = {0};
        DWORD got = 0;

        if (DeviceIoControl(h, SI_IOCTL_GET_PART_STRING, NULL, 0,
                            serial, sizeof(serial), &got, NULL)) {
            printf("part string (9B): %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                   serial[0], serial[1], serial[2], serial[3], serial[4],
                   serial[5], serial[6], serial[7], serial[8]);
        }
        if (DeviceIoControl(h, SI_IOCTL_GET_PART_RECORD, NULL, 0,
                            part, sizeof(part), &got, NULL)) {
            printf("part record (0x30B) first 8: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                   part[0], part[1], part[2], part[3],
                   part[4], part[5], part[6], part[7]);
        }
        if (DeviceIoControl(h, SI_IOCTL_GET_PRODUCT_STRING, NULL, 0,
                            product, sizeof(product), &got, NULL)) {
            printf("product string (%lu B): %s\n", got, product);
        }
    }

    {
        UCHAR status = 0;
        if (ControlIn(h, 0xC0, 0x08, 0, 0, &status, 1)) {
            printf("modem status (0xC0/0x08): 0x%02X\n", status);
        } else {
            printf("modem status failed: %lu\n", GetLastError());
        }
    }

    {
        USHORT divisor = 0x1A00;   /* 48 MHz / 115200 ~= 416 -> 0x1A0 */
        UCHAR flags = 0x04 | 0x08;
        if (ControlOut(h, 0x40, 0x01, divisor, 0, NULL, 0)) {
            printf("set baud divisor 0x%04X: ok\n", divisor);
        } else {
            printf("set baud divisor failed: %lu\n", GetLastError());
        }
        printf("flush (0x%02X): %s\n", flags, FlushBuffers(h, flags) ? "ok" : "failed");
    }

    if (argc > 1 && _stricmp(argv[1], "echo") == 0) {
        UCHAR tx[16];
        UCHAR rx[16] = {0};
        DWORD written = 0, got = 0;
        ULONG avail = 0;
        UCHAR rflags = 0;
        int i;

        for (i = 0; i < (int)sizeof(tx); i++) {
            tx[i] = (UCHAR)(0x40 + i);
        }
        if (!WriteFile(h, tx, sizeof(tx), &written, NULL)) {
            printf("WriteFile failed: %lu\n", GetLastError());
        } else {
            printf("wrote %lu bytes\n", written);
        }

        CheckRxQueue(h, &avail, &rflags);
        printf("rx queue: %lu bytes, flags 0x%02X\n", avail, rflags);

        if (DeviceIoControl(h, SI_IOCTL_READ, NULL, 0, rx, sizeof(rx), &got, NULL)) {
            printf("read %lu bytes:", got);
            for (i = 0; i < (int)got; i++) {
                printf(" %02X", rx[i]);
            }
            printf("\n");
        } else {
            printf("READ failed: %lu\n", GetLastError());
        }
    }

    CloseHandle(h);
    return 0;
}
