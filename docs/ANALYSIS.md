# ELAN USB UICE (USBXpress 3.3) — 行为分析与 ARM64 重构说明

本文件记录对 `SiUSBXp.sys` 3.3 (x64, 2010/2011, Silicon Laboratories) 以及配套
`SiUSBXp.dll` 3.1 的静态逆向结果，并说明 `SiUsbXpArm64` 如何逐项复现这些行为。

- 分析对象：`eUIDE 1.07.32.39` 安装包内 `Driver/U2P15/x64/{SiUSBXp.sys, SiLib.sys}`、
  `Bin/SiUSBXp.dll`
- 方法：PE 解析（pefile）、反汇编（objdump / capstone）、导入导出表、字符串、
  目录签名；未在真机上运行
- 证据地址：`page+0xNNN` 指 `SiUSBXp.sys` 的 `page` 节（RVA 0x3000）偏移，
  `.text+` / `.rdata+` 同理；DLL 地址为 VA（ImageBase 0x10000000）

---

## 1. 架构分层

```
eUIDE.exe (x86)
   │  SI_Open/SI_Read/SI_Write/...           ← 静态链接 SiUSBXp.dll 3.1
   ▼
SiUSBXp.dll  (Win32 用户态 API + 协议封装)
   │  SetupDiGetClassDevs({3C5E1462-...}) → CreateFileA(interface path)
   │  DeviceIoControl(0x2220xx) / WriteFile
   ▼
SiUSBXp.sys  (WDM 功能驱动, 服务 SIUSBXP)
   │  USBD_* / URB / IoBuildDeviceIoControlRequest
   │  同时动态链接 SiLib.sys（GenericDispatchPnp / GenericRegisterInterface 等）
   ▼
USB 设备 (VID 04F3, PID 1006)
```

- `SiUSBXp.sys` 是 WDM 驱动，导入 `SiLib.sys`（Silicon Labs 通用 WDM 支持库）的
  `InitializeGenericExtension`、`GenericDispatchPnp`、`GenericDispatchPower`、
  `GenericRegisterInterface`、`GenericAcquireRemoveLock` 等。
- `SiLib.sys` 只是 PnP/电源/IRP 队列的通用框架库，本身不含 USBXpress 协议逻辑，
  **ARM64 版本用 KMDF 直接替代**，因此不需要（也不存在）ARM64 的 SiLib。
- 设备端：VID `0x04F3`、PID `0x1006`，接口 GUID 见下。

## 2. 设备接口

| 项目 | 值 | 证据 |
|---|---|---|
| 主接口 GUID | `{3C5E1462-5695-4E18-876B-F3F3D08AAF18}` | `.rdata+0x160`，`page+0xb3b` 传给 `GenericRegisterInterface` |
| 次接口 GUID | `{1A1C76CC-E33A-45F4-AC7A-1F27CB83ED8B}` | `.rdata+0x174` |
| 硬件 ID | `USB\VID_04F3&PID_1006` | `SiU2P15.inf` |
| 服务名 | `SIUSBXP` | INF / 服务安装节 |
| 设备名 | `\Device\...`、符号链接 `\DosDevices\USBINT` 字符串 | `.rdata` 中的 `\DosDevices\USBINT` |

`SiUSBXp.dll` 的 `SI_GetNumDevices` / `SI_Open` 走
`SetupDiGetClassDevsA(&GUID, …) → SetupDiEnumDeviceInterfaces →
SetupDiGetDeviceInterfaceDetailA → CreateFileA`，**只依赖接口 GUID**，
所以 ARM64 驱动只要注册同一 GUID 即可被未修改的 `SiUSBXp.dll` 打开。

## 3. 配置与端点

`page+0x13f0`–`page+0x14750`（`IRP_MJ_PNP` 的启动设备回调）流程：

1. `USBD_ParseConfigurationDescriptorEx(…, -1, -1, -1, -1, -1)` 取第一个接口；
2. 若 `bNumEndpoints != 2` → `STATUS_REVISION_MISMATCH (0xC0000182)`（`page+0x14496`）；
3. `USBD_ParseDescriptors(Type=5, ENDPOINT)` 两次取两个端点描述符；
4. 校验端点属性/包长（`page+0x14606` 起，最大包 `0x20`、方向位 `0x80`）；
5. `USBD_CreateConfigurationRequestEx` 建立管道，管道句柄存入设备扩展
   `ext+0xa0`（IN）与 `ext+0xa8`（OUT），并按端点方向交换（`page+0x146b9`）。

结论：设备必须是 **1 个接口 + 2 个批量端点**（一个 IN、一个 OUT，wMaxPacketSize
典型为 0x20/0x40）。ARM64 版本在 `SiSelectPipes()` 中做等价绑定：遍历端点，
取第一个 Bulk IN 与第一个 Bulk OUT。

## 4. IOCTL ABI

全部 `METHOD_BUFFERED`，`DeviceType = 0x22`（`FILE_DEVICE_UNKNOWN`）。
分发表位于 `page+0x76`，以 `ioctl - 0x222008` 的减法链实现：

| IOCTL | 功能码 | 处理函数 | 语义 |
|---|---|---|---|
| `0x222008` | 0x802 | `page+0x6a7` | 返回 18 字节设备描述符（缓存于 `ext+0x54`）；`OutputBufferLength < 0x12` → `STATUS_BUFFER_TOO_SMALL`；Information = 0x12 |
| `0x22200C` | 0x803 | `page+0x666` | 返回 9 字节（8 字符 + 结尾）缓存串（`ext+0x70`）；需要 ≥ 9 字节 |
| `0x222014` | 0x805 | `page+0x62f` | 返回 0x30 字节“部件记录”（`ext+0x78`） |
| `0x222018` | 0x806 | `page+0x5d1` | 先按 `OutputBufferLength` 全量清零，再拷贝 `ext+0x88` 长度的字符串（`ext+0x90`）；Information = 该长度 |
| `0x222024` | 0x809 | `page+0x51c` | 控制传输 OUT（见 §5） |
| `0x222028` | 0x80A | `page+0x476` | 控制传输 IN（见 §5） |
| `0x222080` | 0x820 | `page+0x34a` | 从环形缓冲读；无数据则挂起 IRP |
| `0x222084` | 0x821 | `page+0x28c` | 返回 4 字节：24 位计数 + 标志 |
| `0x222088` | 0x822 | `page+0xd0` | 清空 RX/TX |
| 其它 | — | 默认 | `STATUS_INVALID_DEVICE_REQUEST` |

输出缓冲区不足统一返回 `STATUS_BUFFER_TOO_SMALL (0xC0000023)`。

### 4.1 控制传输包格式（0x222024 / 0x222028）

由 DLL 侧构造（`SI_SetBreak` 0x10002e80、`SI_GetModemStatus` 0x10002d00）并与内核
处理函数逐字节对齐：

```
offset 0 : bmRequestType
offset 1 : bRequest
offset 2 : wValue
offset 4 : wIndex
offset 6 : wLength
offset 8 : data[wLength]
```

- `0x222024`（OUT）：`wLength` 字节随请求发往设备；内核侧根据 `bRequest == 0x13`
  选择 `URB_FUNCTION_VENDOR_INTERFACE`（否则 `URB_FUNCTION_VENDOR_DEVICE`），
  并在 `bRequest == 0x13` 时用部件记录第 3 字节替换 `wIndex`（`page+0x1353a`）。
- `0x222028`（IN）：`bmRequestType & 0x40` 非零 → 厂商请求
  （`URB_FUNCTION_VENDOR_DEVICE`，`TransferFlags = IN | SHORT_TRANSFER_OK`），
  否则 `URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE`（`page+0x13480`）。
  返回数据写回同一 `SystemBuffer` 的 `offset 8` 处。

典型请求（来自 DLL）：

| API | 请求 | 说明 |
|---|---|---|
| `SI_SetBaudDivisor` | `0x40, 0x01, wValue=divisor` | 波特率分频（`SI_SetBaudRate` 计算 `clock/baud`） |
| `SI_SetBreak` | `0x40, 0x05, wValue=break` | 设置/清除 break |
| `SI_GetModemStatus` | `0xC0, 0x08, wLength=1` | 读 1 字节 modem 状态 |

## 5. 打开时序（`page+0x13db4`，`IRP_MJ_CREATE`）

1. 独占计数：`lock xadd [ext+0x50], 1`，结果 > 1 → `STATUS_ACCESS_DENIED (0xC0000022)`；
2. 厂商请求 **0x00 / wValue=0xFFFF**（`sub_111E4`，无数据）；
3. 厂商请求 **0x01 / wValue=0x2000**（无数据）；
4. 若 `partRecord[0x1A] & 0x0F` 不是 2 或 3：厂商请求
   **0xFF / wValue=0x370B，IN 1 字节**，结果写入 `ext+0x200`（mode）；
   否则 `mode = 1`（`page+0x13f1e`）；
5. `ext+0xcf = 1`，启动读引擎（`sub_11640`），完成 IRP。

ARM64 版本按相同顺序在 `SiUsbOpenHandshake()` 中实现；第 4 步失败不致命（与原实现一致）。

## 6. 读路径

- 设备扩展 `ext+0x1f8` 指向 0x1000 字节环形缓冲描述符，字段：`+0x08` 读指针、
  `+0x0C` 写指针、`+0x10` 容量、`+0x18` 数据指针；
- 有数据：按 `min(OutputBufferLength, 可读字节)` 拷贝，Information = 拷贝字节数；
- 无数据：IRP 插入 `ext+0x228` 的 cancel-safe 队列并返回 `STATUS_PENDING`
  （`page+0x133d6`–`page+0x1345e`）；
- `0x222084` 输出：`[0..2]` 为 24 位小端计数，`[3]` 为标志
  （bit0 溢出 / bit1 有数据），见 `page+0x132df`；
- `0x222088`：输入 1 字节，`0x04` 清 RX、`0x08` 清 TX、`0x0C` 两者都清
  （DLL 侧 `SI_FlushBuffers` 0x10001ed0 的位运算与之吻合）。

ARM64 版本：`SiRxBuffer` 0x1000 环形缓冲 + `SiRxReadComplete` 连续读取填充 +
`PendingReads` 手动队列，语义一一对应。

## 7. 写路径（`page+0x1834`，`IRP_MJ_WRITE`）

- 长度 0 → 直接成功；
- 长度 > **0x1000** → `STATUS_INVALID_BUFFER_SIZE (0xC0000206)`（`page+0x1489f`）；
- 否则分配 0x80 字节请求块（tag `0x53694558`）并提交到写队列。
- 无 IRP_MJ_READ 处理函数：读只能走 IOCTL 0x222080（DLL 也只导入 `WriteFile`）。

ARM64 版本用 `WdfUsbTargetPipeWriteSynchronously` 直接发送，同样限制 4 KiB。

## 8. 复位 / 其它

- `SI_ResetDevice` 在 DLL 中压入 `0x22208F`（`CTL_CODE(0x22,0x823,METHOD_NEITHER,0)`），
  但 x64 驱动的分发表只到 `0x222088`，因此该请求实际返回
  `STATUS_INVALID_DEVICE_REQUEST`，DLL 侧映射为错误码 7。
  ARM64 版本对 `0x22208F` 保持“无效请求”，另实现 `0x22208C` 作为可用的复位路径。
- `IRP_MJ_CLEANUP`（`page+0xd68`）取消所有排队请求（`STATUS_CANCELLED`）。
- 设备扩展 `ext+0x200` 保存 mode 字节，`ext+0xcd` 为 RX 溢出标志。

## 9. ARM64 重构中的取舍与已知差异

| 项目 | 原始 (WDM + SiLib) | ARM64 重构 (KMDF) | 影响 |
|---|---|---|---|
| PnP/电源 | SiLib `Generic*` | KMDF 默认策略 | 语义等价 |
| 队列 | 自建 cancel-safe 队列 | KMDF 默认队列 + 手动读队列 | 等价 |
| 读引擎 | 自建 0x1000 环形缓冲 | KMDF 连续读取 + 同尺寸环形缓冲 | 等价 |
| 控制传输 | 手工构造 URB（`URB_FUNCTION_VENDOR_*`） | `WdfUsbTargetDeviceSendControlTransferSynchronously` | 线上 `bmRequestType` 一致（0x40/0xC0） |
| 超时 | 内部固定超时 | 控制 5 s / 写 5 s | 时间参数可能不同 |
| 部件记录 `0x222014` | 由固件/配置推导的 0x30 字节结构 | **合成的记录**（`[1]=1` 部件族） | 见下 |
| 产品串 `0x222018` | 缓存的字节串 | 字符串描述符（UTF-16 原始字节） | 编码可能不同 |
| 0x22208F | 无效请求 | 无效请求 | 一致 |

**关于 `0x222014`（部件记录）**：该 0x30 字节块在原始驱动中由配置/端点信息与设备
返回的数据拼装（`page+0x1466d` 处把指针存入 `ext+0x78`，随后用它取管道句柄），
其精确字节布局依赖固件，无法仅从二进制完全还原。重构版本填充
`PartRecord[1] = 1`（合法部件族，使 `SI_SetLineControl` / `SI_SetFlowControl`
的 1..3 校验通过），其余置零。**如果目标应用依赖该记录的具体内容，需要用真机
抓取原驱动返回值后替换 `SiCacheDescriptors()` 中的填充逻辑。**

## 10. 验证计划

1. 在 ARM64 目标机开启测试签名，安装本驱动；
2. 运行 `SiUsbXpTest.exe`：应能打开设备、读到 18 字节设备描述符、
   `0xC0/0x08` 返回 1 字节 modem 状态；
3. 用 `USB 协议分析仪`（或 USBPcap + Wireshark）同时抓取原 x64 驱动与 ARM64 驱动
   的枚举和 `SI_Open` 时序，逐条比对：
   - 枚举：GET_DESCRIPTOR / SET_CONFIGURATION / 端点；
   - 打开：`40 00 FF FF 00 00`、`40 01 00 20 00 00`、`C0 FF 0B 37 00 01`；
4. 替换 `SiUSBXp.dll` 为原版 3.1（x86 进程在 ARM64 上通过 x86 仿真运行），
   确认 `eUIDE.exe` 可识别 ICE 并完成一次下载/调试；
5. 差异项（§9）逐条确认后再投入生产使用。
