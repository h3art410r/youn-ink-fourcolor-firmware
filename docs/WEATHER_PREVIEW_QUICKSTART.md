# 天气页仿真预览

在刷机前用电脑运行固件真实的天气页面 Renderer，生成 PNG 检查布局。无需连接设备、联网或刷机。

## 运行

在仓库根目录执行：

```powershell
python firmware/tools/preview_weather.py
```

图片默认生成到 `firmware/build-s3/preview/weather.png`。可用 `--output path.png` 指定位置。首次运行会编译主机端程序，之后增量构建。

## 实现范围

- 页面布局、RawDraw 绘制代码、LVGL 字体和天气图标字体直接复用固件源码；天气数据是程序内固定的上海样例，不会请求网络。
- 输出为设备分辨率 400×300 的 PNG，颜色按黑、白、黄、红映射。
- 页面主体由真实 `WeatherRenderer` 绘制。预览入口还画出状态栏的 Wi-Fi 信号柱、日期、页标题、时间和电池图标，位置沿用固件状态栏布局；状态栏里的日期、时间和电量是固定样例。它不是整机模拟器，也不模拟墨水屏刷新、按键、联网或后台网页。
- Windows 首次使用需要 Python、CMake、Ninja 和 LLVM-MinGW。LLVM-MinGW 可安装到任意目录，并设置 `LLVM_MINGW_ROOT` 指向安装目录；ESP-IDF 工具安装中已有的 CMake/Ninja 可自动发现。

修改天气布局或图标后，先运行命令并打开 PNG 检查，再编译/刷写固件。主机预览能发现真实 Renderer 中的字体、图标和布局问题，但不替代真机刷新验证。
