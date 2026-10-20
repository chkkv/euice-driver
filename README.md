# euice-driver — ELAN USB UICE / USBXpress 3.3 驱动的 Windows ARM64 重构

[![build](https://github.com/chkkv/euice-driver/actions/workflows/build.yml/badge.svg)](https://github.com/chkkv/euice-driver/actions/workflows/build.yml)

本工程是对义隆（ELAN）**USB ICE（USBXpress 方案，`USB\VID_04F3&PID_1006`）**
驱动的净室重写，目标是让**未修改的 `SiUSBXp.dll`（以及 `eUIDE.exe`）在 Windows
ARM64 上继续工作**。

驱动通过静态分析 `SiUSBXp.sys` 3.3 (x64) 与 `SiUSBXp.dll` 3.1 得到完整的外部契约：
设备接口 GUID、IOCTL 编号与缓冲区语义、控制传输包格式、打开时序、读写队列与
4 KiB 上限等。逐条证据见 [docs/ANALYSIS.md](docs/ANALYSIS.md)。

> **当前状态**：已在 GitHub Actions（windows-2022 + WDK 10.1.26100）成功编译出
> **ARM64 驱动** `SiUsbXpArm64.sys`，并自动生成 INF/CAT 与测试签名包；**尚未在真实
> USB ICE 硬件上验证**。`0x222014` 部件记录等内容仍需真机比对，请勿直接用于
> 生产环境。

---

## 关键事实

| 项目 | 值 |
|---|---|
| 硬件 ID | `USB\VID_04F3&PID_1006` |
| 设备接口 GUID | `{3C5E1462-5695-4E18-876B-F3F3D08AAF18}`（另注册 `{1A1C76CC-E33A-45F4-AC7A-1F27CB83ED8B}`） |
| 服务名 | `SIUSBXP` |
| 端点 | 1 个批量 IN + 1 个批量 OUT（原始驱动要求 `bNumEndpoints == 2`） |
| IOCTL | `0x222008 / 0x22200C / 0x222014 / 0x222018 / 0x222024 / 0x222028 / 0x222080 / 0x222084 / 0x222088` |
| 单次最大写 | `0x1000` 字节（超出返回 `STATUS_INVALID_BUFFER_SIZE`） |
| 打开独占 | 同一时刻仅允许一个句柄（第二个 `CreateFile` 返回 `STATUS_ACCESS_DENIED`） |

## 目录结构

```
euice-driver/
├── .github/workflows/build.yml   CI：构建 + inf2cat + 测试签名 + 发布 Release
├── src/
│   ├── SiUsbXp.h     共享定义：IOCTL、控制包、设备上下文
│   ├── Driver.c      DriverEntry、设备/队列、文件语义、IOCTL 分发
│   ├── Usb.c         USB 目标、端点绑定、控制传输、打开握手
│   └── Read.c        RX 环形缓冲、连续读取、挂起读队列
├── inf/SiUsbXpArm64.inf   ARM64 安装包（KMDF）
├── build/
│   ├── SiUsbXpArm64.vcxproj
│   └── build.ps1      构建 + inf2cat + 测试签名
├── test/SiUsbXpTest.c 用户态一致性测试（枚举/查询/控制/读写）
└── docs/ANALYSIS.md   逆向分析与 ABI 说明
```

## 构建

### CI（推荐）

推送到 `main` 或打 `v*` 标签即自动构建：

- 每次 push / PR：构建 ARM64 驱动，上传 `SiUsbXpArm64` artifact；
- 打 `v*` 标签：额外生成 Release，附带
  `SiUsbXpArm64-<tag>.zip`（sys/inf/cat/测试证书/文档）。

最新构建产物可在 Actions 的 `SiUsbXpArm64` artifact 或 Releases 页面获取。

工作流在 `windows-2022` 上安装 Windows SDK + WDK 24H2（10.0.26100）后编译。

### 本地

需要 **Visual Studio 2022** + **WDK 10.0.26100**（含 ARM64 工具集）。

```powershell
# 在 "x64 Native Tools Command Prompt" 或任意 PowerShell 中
cd build
.\build.ps1 -Sign
```

产物：`artifacts\ARM64\Release\SiUsbXpArm64.{sys,inf,cat}`。
CI 中会对产物做**测试签名**（自签证书随包提供），生产使用需 EV 证书 +
微软 attestation 签名。

手工构建等价命令：

```powershell
msbuild SiUsbXpArm64.vcxproj /p:Configuration=Release /p:Platform=ARM64
inf2cat /driver:..\artifacts\ARM64\Release /os:10_ARM64,10_GE_ARM64
```

## 安装（ARM64 目标机，管理员）

```powershell
bcdedit /set testsigning on      # 测试签名；生产环境需 EV/微软认证签名
# 重启后：
pnputil /add-driver .\SiUsbXpArm64.inf /install
```

证书必须导入**本地计算机**（LocalMachine）两个存储区，用户级存储区对驱动安装无效：

```powershell
# 管理员 PowerShell
Import-Certificate -FilePath .\SiUsbXpArm64Test.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath .\SiUsbXpArm64Test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
pnputil /add-driver .\SiUsbXpArm64.inf /install
```

若报 “没有建立对 Authenticode(TM) 签名的目录的发布者信任”，按顺序检查：
证书是否在 `LocalMachine\TrustedPublisher`（不只是 Root）→ 是否已 `bcdedit /set testsigning on` 并重启
→ `Get-AuthenticodeSignature .\SiUsbXpArm64.cat` 的 `Status` 是否为 `Valid`。

安装后设备管理器中应出现 **ELAN USB UICE**（USB 控制器类别），无黄色感叹号。

## 验证

```powershell
cl /W4 SiUsbXpTest.c setupapi.lib
.\SiUsbXpTest.exe            # 打开设备、读描述符/部件记录、modem status、设置波特率
.\SiUsbXpTest.exe echo       # 追加一次写-读回环
```

然后可直接运行 `eUIDE.exe`（x86 仿真）：IDE 应能识别 ICE 并进入调试。

**建议的等价性验证**：用 USBPcap/Wireshark 或硬件分析仪分别抓取原 x64 驱动与
ARM64 驱动的枚举 + `SI_Open` 时序，逐条比对 `docs/ANALYSIS.md` §5 中的三条
厂商请求。

## 已知差异（详见 ANALYSIS.md §9）

1. **`0x222014` 部件记录**：原始内容依赖固件，无法从二进制完整还原；当前实现
   合成一条 `PartRecord[1]=1` 的记录，使行控制/流控的 1..3 校验通过。
2. **`0x222018` 产品串**：当前返回字符串描述符的 UTF-16 原始字节，原驱动缓存的
   是字节串，编码可能不同。
3. **超时参数**：控制 5 s、写 5 s，原驱动使用内部固定值，未必完全相同。
4. **`0x22208F`**：原驱动分发表未覆盖，返回 `STATUS_INVALID_DEVICE_REQUEST`；
   本实现保持一致。

## 说明

- 本工程为互操作性目的从零编写，**不包含** Silicon Laboratories / ELAN 的任何
  原始代码或二进制；仅在文档中引用其公开接口与逆向得到的常量。
- `SiLib.sys` 的 PnP/电源框架职责由 KMDF 承担，因此不需要（也不存在）ARM64 版
  `SiLib.sys`。
- 驱动按原 INF 的硬件 ID/服务名安装，可与原厂 x64 驱动共存于同一个 INF 包集合中
  （不同平台目录）。
