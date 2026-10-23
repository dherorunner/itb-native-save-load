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

从仓库页面的“Code → Download ZIP”下载源码并解压，或使用已安装的 Git 克隆本仓库。
进入能同时看到 `release.json`、`src` 和 `tools` 的文件夹，这就是仓库根目录。
在资源管理器地址栏输入 `powershell` 并按 Enter，先执行 `python --version` 确认 Python 可用，再执行：

```powershell
python -X utf8 -B tools/build_mod.py
```

脚本调用 x86 编译环境，使用 `ITB_SL_NATIVE_EXPERIMENT=1`、`ITB_SL_P04_SERVICE=1` 编译原生模块，
再把 Lua 模块与说明文档打包。输出在 `build/<时间戳>/package/itb_sl`，同时生成源码构建 ZIP；
每次创建新目录，不覆盖旧产物，也不触碰游戏安装目录或存档。

成功时终端输出 `BUILT` 及 `ITB-Native-Save-Load-v0.4.7-local-build.zip` 的路径。
ZIP 中应直接有 `itb_sl/scripts/init.lua` 和 `itb_sl/native/itb_sl_p04.dll`。
编译失败时，查看终端输出的 `OUTPUT` 目录下 `environment.log`、`native.log`、`service.log`、`link.log`
中已生成的日志；缺少工具链时先补齐环境，不要从未知来源补 DLL。

## 贡献与验证

- 使用问题先到 [Issues](https://github.com/dherorunner/itb-native-save-load/issues) 反馈，附上[问题报告所需信息](KNOWN_ISSUES.zh-CN.md)。
  功能建议请描述具体使用场景；较大的修改建议先开 Issue 沟通范围。
- 修改 Lua 时编辑 `src/mod/scripts/` 中的独立模块；构建器会合并为包内 `scripts/init.lua`，不要直接修改生成文件。
  修改原生逻辑时编辑 `src/native/`，保持 x86 目标和既有协议一致。
- 提交 Pull Request 时说明改动解决什么问题、怎样验证、哪些还未验证。保持修改聚焦；不要包含构建产物、
  游戏 EXE/DLL、个人存档或日志中的隐私信息。源码中的构建所需头文件应正常保留。
- 代码变更至少运行一次上述构建，并核对包内 DLL、Lua 入口、VERSION 和 manifest.json 是否存在。
  GitHub Actions 也会编译并检查这些文件；**它不启动游戏，也不验证存读结果**。
  纯文档修改通常只需检查步骤是否符合实现、链接是否有效，以及中英文关键内容是否一致。
- 若自行做实机验证，先备份完整用户数据，记录游戏哈希、ModLoader 版本、构建身份及操作步骤。
  只将实际观察到的行为写为通过；构建成功不能代替游戏内验证。

`SOURCE_PROVENANCE.json` 是首次导出的来源记录，修改源码后不要把其中的旧哈希当成当前文件哈希，
也不要为普通贡献伪造原始来源记录。新的源码构建由产物内 `manifest.json` 记录实际文件哈希。

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
