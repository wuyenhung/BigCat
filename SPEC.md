# BigCat 项目规格

## 项目概述
- **名称**: BigCat — SOEM EtherCAT Master for Windows
- **用途**: 6-DoF Stewart 平台运动控制系统
- **作者**: wuyenhung (巫彥鴻)
- **技术栈**: Win32 C++, SOEM, HTTP/SSE, MMF

## 系统配置

### 网络接口
- EtherCAT 使用: `\Device\NPF_{GUID}` 格式
- 查询命令: `Get-NetAdapter | Format-Table Name, InterfaceDescription, ifIndex`
- 运行要求: **系统管理员权限**

### HTTP 服务器
- 端口: `8080` (默认关闭，由 BigCat.ini 控制)
- 页面:
  - `/auto.html` - 自动操作主页面
  - `/status.html` - 状态监控
  - `/sdo.html` - SDO 读写

### 授权
- 方式: `license.txt` 文件或 `BIGCAT_LICENSE_KEY` 环境变量
- 授权密钥: 64 字符 hex
- 开发绕过: `BIGCAT_DISABLE_LICENSE_CHECK=1` (仅 Debug 构建)

## 关键文件

### 参数配置
- `Par/motion.xml` - 运动参数
- `Par/machine.xml` - 机械参数
- `Par/motion.default.xml` - 默认运动参数

### ESI 定义
- `esi/` 目录 - EtherCAT 从站配置

### 状态日志
- `logs/status.log` - 运行日志 (可由 INI 控制开关)

## 构建

### CMake 选项
```powershell
cmake . -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

### 输出
- `build/Release/bin/main/BigCat.exe`

### 依赖
- Npcap SDK (设置 `-DNPCAP_SDK_DIR`)
- Visual Studio 2022+

## 特色功能

### 运动控制
- PDOFilter: 速度/加速度/位置限制
- Soft-Start: 缓启动保护
- Jog: 寸动操作
- Stewart IK: 逆运动学计算

### 进程间通信
- MMFReader/MMFWriter: Memory-Mapped File

### 安全
- TPM 硬件绑定授权
- 单例保护
- CrashHandler minidump

## INI 配置 (BigCat.ini)

```ini
[Server]
EnableLocalHttpServer=0

[Startup]
ShowMainWindow=0
AutoTrigger=1

[Logging]
EnableStatusLog=1
```

## 注意事项

- 运行需管理员权限
- 试用模式无人身安全保证
- 项目为专有授权，非开源