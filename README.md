# 陷阵之志：原生存读档

**Into the Breach: Native Save & Load · v0.4.7 · 作者 dherorunner · MIT**

[English](README.en.md) · [![build](https://github.com/dherorunner/itb-native-save-load/actions/workflows/build.yml/badge.svg)](https://github.com/dherorunner/itb-native-save-load/actions/workflows/build.yml)

在《陷阵之志》游戏内按 **Esc → 存档 / 读档**，把当前战局存下来，或读回本场战斗的存档。
它调用游戏自身的存读档函数，运行时不依赖 Python、控制台或任何游戏外存档工具。这是非官方 Mod；
游戏本体与 ModLoader 需自行准备，不随本仓库分发。

## 当前范围

- 一个临时手动槽 ＋ 一个战前自动档。
- 读取仅限**同一进程、同一场战斗**：退出重开后，磁盘上的额外槽不能被本版读取；跨战斗读取也不支持。
- 游戏自带的“继续游戏”与本 Mod 的临时槽是两套用途，互不替代。

0.4.7 通过了离线检查，以及菜单布局、代表按钮和 SL 入口开关的验证。手动与自动存读的闭环实机证据
来自 0.4.6（存读业务源码未变，本包沿用 v5 候选 DLL 的原字节），**0.4.7 未重新实测完整存读闭环**。
完整边界见[验证范围](VALIDATION.md)。

## 给玩家：安装

1. 关闭游戏，备份整个游戏用户数据目录，详见[安装、卸载与备份](INSTALL.zh-CN.md)。
2. 尚未安装 ModLoader 时，按[官方说明](https://github.com/itb-community/ITB-ModLoader/tree/v2.9.6#installation)
   安装 [ITB-ModLoader 2.9.6](https://github.com/itb-community/ITB-ModLoader/releases/tag/v2.9.6)，已装可跳过。
3. 从 Releases 下载 `ITB-Native-Save-Load-v0.4.7-windows.zip`，解压后把其中的 **`itb_sl` 文件夹**
   复制到游戏目录的 `mods` 下，再在 ModLoader 的 Mod 管理界面启用它。

最终应存在 `Into the Breach/mods/itb_sl/scripts/init.lua` 与
`Into the Breach/mods/itb_sl/native/itb_sl_p04.dll`，不要多套一层目录。
GitHub 自动生成的 “Source code” 压缩包不是可安装的 Mod。

## 给开发者：从源码构建

需要 Windows、Python 3.12（仅标准库）和已安装的 Visual Studio 2022／Build Tools（含 MSVC x86/x64
C++ 工具，脚本用 `vswhere.exe` 查找）。不需要游戏文件、第三方 Python 包或真实存档。

```powershell
python -X utf8 -B tools/build_mod.py
```

产物落在 `build/<时间戳>/package/itb_sl`，同时生成源码构建 ZIP。重新编译不承诺与发行 DLL 逐字节相同。
细节见[构建说明](BUILDING.md)；每次推送 main 时，GitHub Actions 也会在干净的 Windows 环境自动编译一遍。

## 仓库结构

| 路径 | 内容 |
|---|---|
| `src/mod/scripts/` | Lua 模块：入口、读取器、存档存储、控制器、界面、Esc 菜单 |
| `src/native/` | C++ 原生桥与文件服务，编译为 `itb_sl_p04.dll` |
| `tools/build_mod.py` | 独立构建脚本：编译 DLL，打包 Lua 与说明文档 |
| `release.json`、`SOURCE_PROVENANCE.json` | 版本元数据与导出来源清单 |
| `*.md` 文档 | 安装、限制、构建、验证、第三方说明 |

## 支持环境

| 项目 | 当前范围 |
|---|---|
| 系统、输入 | Windows 桌面版；键盘和鼠标；已有实机为 Windows Steam 版 |
| 游戏 | 已验证环境显示 1.2.93；同时必须匹配下方 EXE SHA-256 |
| 加载器 | ITB-ModLoader 2.9.6，自带 UI 与扩展；本 Mod 没有其他单独的 Mod 前置 |
| 游戏内界面 | 中文；名称中英双语不代表已经提供英文 UI |
| 存读范围 | 同进程、同战斗；一个手动槽、一个自动档 |

支持的 `Breach.exe` SHA-256：

```text
31fe352655982398fb3ee8b0bbe80efd5d65e3a9aa11e3dc39d0364354493fe9
```

游戏版本文字相同也不保证 EXE 相同。Mod 对不匹配的 EXE 会停用存读服务；不要修改哈希检查来强行使用。
游玩无需额外安装 Python、编译器或 VC 运行库。

## 文档

- [安装、升级、卸载与备份](INSTALL.zh-CN.md)
- [已知限制与问题处理](KNOWN_ISSUES.zh-CN.md)
- [版本更新说明](CHANGELOG.md) · [本版发布说明](RELEASE_NOTES.md)
- [验证范围](VALIDATION.md) · [构建说明](BUILDING.md) · [第三方说明](THIRD_PARTY_NOTICES.md)

## 数据与安全

Mod 额外数据保存在 `%LOCALAPPDATA%/ITB-SL/P04`。卸载不会删除这些数据，也不会替你恢复游戏进度。
首次使用前请备份；不要用本 Mod 的临时槽代替长期进度备份。

## 许可

原创 Mod 代码与文档采用 [MIT](LICENSE)。《陷阵之志》及其资源、ITB-ModLoader 归各自权利人所有，
不在本仓库的分发范围内。
