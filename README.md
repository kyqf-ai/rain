# rain

**雨量采集托盘程序**：通过串口读取雨量计，将小时及分钟雨量保存到 SQLite，并提供 Windows 托盘程序和本地网页查询界面。

> Windows 可执行文件位于 [`Rain_New/dist/rain.exe`](Rain_New/dist/rain.exe)（如果该构建产物未随仓库发布，请按下方说明自行构建）。详细使用说明、配置项和故障排查见 [`Rain_New/README.md`](Rain_New/README.md)，设计说明见 [`Rain_New/docs/DESIGN.md`](Rain_New/docs/DESIGN.md)。

## 功能

- 后台采集雨量计数据，写入 `rain.db`；支持历史数据补齐。
- 最小化到 Windows 系统托盘，并通过图标颜色和提醒显示设备或串口状态。
- 内置本地网页，可查看当日雨量、小时图表、统计报表及报警记录。
- 提供诊断命令、单元测试和模拟设备工具。

## 构建

Windows 构建需要 MinGW-w64。进入 `Rain_New` 目录，在 PowerShell 中运行：

```powershell
powershell -ExecutionPolicy Bypass -File tools\fetch_sqlite.ps1
build_win.bat test
build_win.bat
```

构建产物为 `Rain_New/dist/rain.exe`。也可以将仓库推送到 GitHub，通过 **Actions** 中的 `build-windows` 工作流获取 Windows 构建产物。

Linux 开发测试（进入 `Rain_New` 目录）：

```sh
make STUB=1 test
```

## 快速开始

1. 将 `rain.exe` 放到固定目录；数据库、配置和日志会保存在该目录。
2. 退出其他正在使用该串口的采集程序，再启动 `rain.exe`。
3. 首次启动会生成 `rain.ini`。按实际设备修改串口配置，确认状态显示在线。
4. 本地网页默认地址为 <http://127.0.0.1:9099/>；也可从托盘菜单打开。

网页服务默认绑定本机。若将 `rain.ini` 中 `[web] bind` 改为 `0.0.0.0`，同一网络内的其他设备也可访问；**网页没有登录和密码，请仅在可信网络中开放**。不需要网页服务时可设置 `[web] enable=0`。

## 首次上线注意

雨量数据帧的字节序和单位尚需用真实设备核验。正式切换前，请依照 [`Rain_New/README.md` 的首次上线检查](Rain_New/README.md#4-首次上线检查重要约-10-分钟) 使用 `rain.exe probe` 对照已知数据，并与旧程序错开运行一段时间，确认同一小时数据一致后再切换。

## 项目结构

主要源码、构建脚本和文档位于 [`Rain_New/`](Rain_New/)：

- `src/`：采集、协议、存储、串口、网页服务及 Windows 界面
- `web/index.html`：网页界面源码
- `tests/`：单元测试与端到端测试
- `tools/`：网页生成、SQLite 获取及设备模拟工具

许可证及更多配置说明请见仓库中的相关文件和 [`Rain_New/README.md`](Rain_New/README.md)。
