# 故障排查

## 设备无法启动（代码 10）

代码 10 表示驱动已加载，但 `EvtDevicePrepareHardware` 中的 USB 初始化失败。
驱动会把失败阶段写入设备的硬件键，便于定位（无需内核调试器）：

```powershell
$d = Get-PnpDevice -PresentOnly | Where-Object InstanceId -like '*VID_04F3&PID_1006*'
$d | Format-List InstanceId, Status, Problem, ProblemDescription
Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)" |
    Select-Object SiInitStage, SiInitStatus
```

> 需要 v0.1.4 及以上版本；旧版本没有该字段。
> 修改/重装驱动后请禁用再启用设备（或卸载后扫描硬件改动）以重新执行初始化。

| SiInitStage | 含义 | 常见原因 |
|---|---|---|
| `0` | 初始化成功 | 问题在后续 IO（打开设备 / 端点读写） |
| `1` | 创建 USB 目标对象失败 | KMDF/USB 栈异常，`SiInitStatus` 为 NTSTATUS |
| `2` | 选择配置或查找批量端点失败 | 设备没有 2 个批量端点，或固件未就绪 |
| `4` | 读取描述符/字符串失败 | 设备描述符异常 |

`SiInitStatus` 是十进制 DWORD，转成十六进制即为 NTSTATUS，例如 `3221225473` = `0xC0000001`。

## 安装时报“没有建立对 Authenticode 签名的目录的发布者信任”

- 必须使用**同一个 zip** 里的 `SiUsbXpArm64Test.cer`（每次构建都会生成新证书）；
- 证书需导入**本地计算机**的两个存储区：`Root` 与 `TrustedPublisher`；
- 确认已 `bcdedit /set testsigning on` 并重启；
- `Get-AuthenticodeSignature .\SiUsbXpArm64.cat` 应为 `Valid`。

## 安装时报“哈希不在目录中”

目录与二进制不匹配，通常是把不同版本的 `.sys`/`.cat`/`.inf` 混用了。请整包解压使用。

## 驱动加载失败（代码 39/31）

多为 KMDF 版本不匹配：INF 的 `KmdfLibraryVersion` 必须与构建时使用的 KMDF 版本一致。
