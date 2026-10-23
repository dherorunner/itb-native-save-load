# 从源码构建

玩家直接用 Release 里的 `ITB-Native-Save-Load-v0.4.7-windows.zip` 即可，不需要下面这些工具。
本页面向想自行编译、修改或审查源码的开发者。

## 环境要求

- Windows。产物是 **x86 DLL**，不能改用 x64 编译替代。
- Python 3.12（构建脚本只使用标准库）。
- Visual Studio 2022 或 Build Tools，包含 MSVC x86/x64 C++ 工具与 Windows SDK。
  脚本用 `vswhere.exe` 查找已安装的工具链；**缺失时直接报错，不会自动安装任何依赖**。
- 不需要游戏文件、Lua/SDL 开发包、第三方 Python 包或真实存档。

若缺少 C++ 工具，请通过 [Visual Studio 官方安装器](https://visualstudio.microsoft.com/downloads/)
安装“使用 C++ 的桌面开发”组件。只为运行玩家包不需要安装开发工具。

## 构建

在仓库根目录执行：

```powershell
python -X utf8 -B tools/build_mod.py
```

脚本调用 x86 编译环境，使用 `ITB_SL_NATIVE_EXPERIMENT=1`、`ITB_SL_P04_SERVICE=1` 编译原生模块，
再把 Lua 模块与说明文档打包。输出在 `build/<时间戳>/package/itb_sl`，同时生成源码构建 ZIP；
每次创建新目录，不覆盖旧产物，也不触碰游戏安装目录或存档。

## 产物内容

| 文件 | 说明 |
|---|---|
| `native/itb_sl_p04.dll` | 原生桥与文件服务，x86 DLL |
| `scripts/init.lua` | 各 Lua 模块合并后的入口 |
| `VERSION` | 版本号 |
| `manifest.json` | 版本元数据与逐文件 SHA-256 |
| 说明文档 | README、安装、限制、许可等，随包分发 |

## 可复现性说明

- 重新编译**不承诺**与发行包 DLL 逐字节相同：工具链版本与时间戳都会影响结果。
- `release.json` 记录版本信息与发行 DLL 的 SHA-256；`SOURCE_PROVENANCE.json` 记录源码来源与导出哈希。
- 每次推送 `main`，GitHub Actions 会在干净的 `windows-latest` 环境执行同一条构建命令，
  用来证明源码可以独立构建；该 CI 产物未做实机验证。

## 目录

```text
src/mod/scripts/        Lua 模块：入口、读取器、存储、控制器、界面、Esc 菜单
src/native/             C++ 原生桥与文件服务、所需头文件
tools/build_mod.py      独立构建脚本
release.json            版本元数据
SOURCE_PROVENANCE.json  源码来源清单
*.md                    说明文档
```

当前支持的游戏版本见 [README](README.md)，已知限制见 [KNOWN_ISSUES.zh-CN.md](KNOWN_ISSUES.zh-CN.md)，
实际验证范围见 [VALIDATION.md](VALIDATION.md)。本源码不提供跨进程、跨战斗或十槽功能。
