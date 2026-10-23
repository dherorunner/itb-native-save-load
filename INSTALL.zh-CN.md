# 安装、升级、卸载与备份

本文适用于 **Windows 版游戏 + 键盘鼠标**。首次安装按下面顺序操作；已安装用户可直接看“更新现有版本”。
支持的游戏版本为 1.2.93，且 `Breach.exe` 必须匹配 [README 中的 SHA-256](README.md)。
SHA-256 是文件指纹，用于确认游戏程序是否与本 Mod 支持的版本完全一致；核对方法见本文末尾。

你需要下载两个不同的包：

| 用途 | 下载文件 | 安装位置 |
|---|---|---|
| 加载 Mod 的前置工具 | [ITB-ModLoader 2.9.6](https://github.com/itb-community/ITB-ModLoader/releases/tag/v2.9.6) 的 `ITB-ModLoader-2.9.6.zip` | 含 `Breach.exe` 的游戏根目录 |
| 本项目 | [玩家安装包](https://github.com/dherorunner/itb-native-save-load/releases/download/v0.4.7/ITB-Native-Save-Load-v0.4.7-windows.zip) `ITB-Native-Save-Load-v0.4.7-windows.zip` | 根目录下的 `mods/itb_sl` |

在 GitHub 发布页面展开 **Assets** 可看到附件。`Source code (zip)`、`Source code (tar.gz)` 和
仓库的“Code → Download ZIP”是源码，不能直接作为 Mod 安装。`SHA256SUMS.txt` 是下载校验表，不需要复制到游戏里。

## 先备份

关闭游戏后，复制**整个**游戏用户数据目录到你自己选择的安全位置，按日期命名。
通常在“文档 → My Games → Into the Breach”；可按 Win+R 输入 `shell:Personal` 打开当前 Windows 的“文档”目录，
再进入 `My Games/Into the Breach`。文档重定向到 OneDrive 时，以实际目录为准。
备份应包含所有 `profile_*`、设置及目录内其他文件，不要只备份一份 `saveData.lua`。

已经用过本 Mod 时，再单独备份 `%LOCALAPPDATA%/ITB-SL/P04`。
Win+R 可以直接打开这个路径。尚不存在时不用创建，它会在 Mod 初始化后生成。
额外槽文件包含战局数据，向别人反馈问题前不要直接公开整个目录。

## 第一次安装

1. 确认自己使用 Windows 版游戏；当前支持的 EXE 哈希见 README。
2. 到 [ModLoader 2.9.6 官方 Release](https://github.com/itb-community/ITB-ModLoader/releases/tag/v2.9.6)
   下载 `ITB-ModLoader-2.9.6.zip`。按[官方安装说明](https://github.com/itb-community/ITB-ModLoader/tree/v2.9.6#installation)
   解压到包含 `Breach.exe` 的游戏根目录，按其要求覆盖文件。这一步是安装加载器，会改动游戏安装文件。
   使用现有加载器时，先确认其版本；不要随意混装不同版本的 DLL。
3. 下载本 Mod 的 `ITB-Native-Save-Load-v0.4.7-windows.zip`，解压到临时位置。
4. 在 Steam 库中右键《陷阵之志》→“管理”→“浏览本地文件”，打开能看到 `Breach.exe` 的文件夹。
   这是游戏根目录；常见位置为 `C:\Program Files (x86)\Steam\steamapps\common\Into the Breach`，以实际打开的位置为准。
   把解压出来的整个 `itb_sl` 文件夹复制到这里的 `mods` 内；不存在 `mods` 时新建该文件夹。
   不要把 Mod 放到真实存档目录，也不要把 ZIP 文件直接放进 `mods`。
5. 启动游戏，在 ModLoader 的 Mod 管理界面启用“原生存读档 / Native Save & Load”，按加载器提示应用／重启。
   进入档案与战局后，按 Esc 查看“存档 / 读档”。安装与使用不需要管理员模式的外部控制器。

目录检查：

```text
Into the Breach/
  Breach.exe                 （你已有的游戏）
  mods/
    itb_sl/
      scripts/init.lua
      native/itb_sl_p04.dll
      README.md
      VERSION
      manifest.json
      ...说明与许可文件
```

如果变成 `mods/itb_sl/itb_sl/scripts/init.lua` 或 `mods/ITB-Native-Save-Load-v0.4.7-windows/itb_sl/...`，
说明多套了一层。应把直接包含 `scripts`、`native` 的那一层 `itb_sl` 放进 `mods`。

## 确认安装成功并开始使用

1. 在 ModLoader 管理界面确认“原生存读档 / Native Save & Load”已启用。
2. 进入档案及正常战局，按 Esc，确认有“存档 / 读档”入口。
3. 点击入口，应看到“战前自动档”“临时手动槽”和保存、读取按钮。
   刚安装时槽为空是正常现象；继续已有战斗也不会补造本场战前档。
4. 按 [README 的第一次保存与读取步骤](README.md)使用手动槽。

能打开面板只说明入口加载成功。按钮无反应、槽显示“本次进程外”或提示不支持版本时，
请按[问题处理表](KNOWN_ISSUES.zh-CN.md)检查，不要反复覆盖安装来尝试修复所有问题。

## 更新现有版本

先退出游戏并完成上述备份。把旧 `mods/itb_sl` 整个移到 `mods` **外面**保留，
再复制新版 `itb_sl`。不要只换一个 DLL 或把新旧脚本混在一起；也不要在 `mods` 内留一份改名的旧副本。
默认保留 `%LOCALAPPDATA%/ITB-SL/P04`；旧进程的槽显示为不可读是本版本的预期限制，更新不会迁移它们。

## 卸载与回退

关闭游戏后，将 `mods/itb_sl` 移到游戏目录外即可卸载本 Mod；也可先在加载器界面禁用并重启。
原生模块不能在运行中热卸载。额外数据默认保留；确实不再需要时，先备份，再自行移走整个外置数据目录。
不要因卸载本 Mod 删除游戏用户目录、档案或来源不明的 `p04_*.lua` 文件。

仅卸载本 Mod 不需要卸载 ModLoader。若要恢复完全原版游戏，参照加载器自己的
[卸载说明](https://github.com/itb-community/ITB-ModLoader/tree/v2.9.6#uninstallation)，
使用它提供的 `uninstall_modloader.bat`；本包不提供或代替这个脚本。

恢复旧 Mod 版本时，游戏关闭后移走新版，再放回自己备份的旧文件夹。
如果要恢复游玩前的游戏进度，先备份当前现场，再用对应时间的完整游戏用户目录备份进行恢复。
这会丢弃备份之后的进度；本 Mod 不自动执行这类恢复，也不会更改 Steam 云设置。

恢复时，先把当前 `Into the Breach` 用户数据文件夹移到别处保留，再将自己完整备份的文件夹复制回原位置，
核对最终是 `My Games/Into the Breach/profile_*`，不要套成两层 `Into the Breach`。
只恢复对应的用户数据目录，不用它覆盖含 `Breach.exe` 的安装目录；出现云存档冲突时先保留本地与云端副本并确认时间，避免盲目覆盖。

## 可选：核对 EXE

在资源管理器中打开含 `Breach.exe` 的游戏根目录，点击上方地址栏，输入 `powershell` 并按 Enter。
在弹出的窗口执行下面的只读命令；输出的 Hash 一列应与 README 中的哈希一致（大小写无关）。

```powershell
Get-FileHash -Algorithm SHA256 -LiteralPath .\Breach.exe
```

哈希不匹配时，当前版本不支持该 EXE；不要下载来源不明的游戏 EXE 来凑哈希。
