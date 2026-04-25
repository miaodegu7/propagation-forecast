# Visual Studio 构建说明

## 1. 打开方案

- 在 Visual Studio 中打开 `propagation-forecast.sln`

## 2. 配置与产物

- `Release|x64`
  - 构建：`propagation_bot.exe` / `propagation_desktop.exe` / `propagation_qt_desktop.exe`
- `SingleExe|x64`
  - 打包：`dist-vs-single\propagation_bot.exe`（尽量单文件）
- `PackageQt|x64`
  - 打包：`dist-vs-qt\`（Qt 桌面端便携包，含依赖）

## 3. MSYS2 路径

脚本默认按以下顺序查找 MSYS2：

1. 环境变量 `PROPAGATION_MSYS2_ROOT`
2. `H:\tools\msys64`
3. `C:\msys64`

如果你的路径不同，请在系统环境变量里设置：

```powershell
setx PROPAGATION_MSYS2_ROOT "D:\tools\msys64"
```
