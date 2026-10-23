# 第三方说明

本 Mod 的原创代码和随附说明采用 MIT，Copyright (c) 2026 dherorunner。

- **Into the Breach**：游戏名称、程序、脚本、素材及其他游戏内容属于各自权利人。
  需用户自行拥有并安装游戏。本 Mod 并非 Subset Games 官方作品，MIT 不授权游戏内容。
- **ITB-ModLoader 2.9.6**：运行前置，用户从
  [官方仓库](https://github.com/itb-community/ITB-ModLoader)及
  [官方 Release](https://github.com/itb-community/ITB-ModLoader/releases/tag/v2.9.6)单独安装。
  本包不重新分发 ModLoader、它的代理 DLL 或扩展源码；其许可与贡献者权利不因本 Mod 的 MIT 而改变。
- **Lua 5.1／SDL2**：通过游戏／加载器已经提供的运行时接口动态访问；本包不附带这些库或头文件。
  原生桥只声明所需接口和 SDL 键盘事件 ABI。
- **Windows API／MSVC**：原生模块使用 Windows API，由 MSVC x86 工具链构建。
  本包不附编译器、SDK 或游戏 DLL；发布 DLL 的静态导入仅有 `bcrypt.dll`、`KERNEL32.dll`。

公开源码只包含本 Mod 所需源码及构建说明，不包含游戏本体、用户存档、日志、截图或本机 Git 历史。
