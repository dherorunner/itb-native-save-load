# v0.4.7 — 陷阵之志：原生存读档 / Into the Breach: Native Save & Load

作者 **dherorunner** · **MIT** · Windows · 中文游戏内界面 · 非官方 Mod

在游戏中按 **Esc → 存档 / 读档**，调用游戏自身的存读档功能，无需游戏外控制器或 Python。
这一版提供一个临时手动槽和一个战前自动档，**只支持本次启动内、同一场战斗的读取**。
十槽、命名和排序留给以后更新，不属于本版范围。

## 下载与安装

下载 **`ITB-Native-Save-Load-v0.4.7-windows.zip`**。关闭游戏并备份完整的游戏用户数据目录；
安装 [ITB-ModLoader 2.9.6](https://github.com/itb-community/ITB-ModLoader/releases/tag/v2.9.6) 后，
把 ZIP 内的 `itb_sl` 文件夹复制到游戏的 `mods` 目录，再在加载器中启用。
运行不需要 Python 或编译器；GitHub 自动生成的“Source code”压缩包不是可安装的 Mod。

## 兼容性

支持 Windows 版游戏 1.2.93，且 `Breach.exe` 的 SHA-256 必须为：

```text
31fe352655982398fb3ee8b0bbe80efd5d65e3a9aa11e3dc39d0364354493fe9
```

版本号相同不保证 EXE 相同；其他平台、其他 EXE、跨战斗以及重启后读回都不在支持范围内。
从地图自然进入新的普通战斗才会尝试生成战前档；继续已有战斗时可先使用手动槽。

## 验证范围

v0.4.7 完成了离线检查，以及菜单间距、代表按钮与 SL 入口开关的最小实机验证。
manual／auto 存读闭环的单案例证据来自 0.4.6；存读业务源码未变，本包沿用 v5 候选 DLL 的原字节，
**0.4.7 没有重新实测整个闭环**。其他任务、其他 Mod 组合、全部单位字段和窗口缩放未完整覆盖。
细节见包内 README、安装备份说明、已知限制与 [VALIDATION.md](VALIDATION.md)。
