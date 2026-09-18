# TinyRenderer 演示与复现

这份说明用于从源码运行当前项目，也可以作为面试时的演示顺序。项目输出静态图片，不需要启动图形窗口。

## 构建当前版本

在 Windows 上安装 Visual Studio 的 C++ 桌面开发工具，进入仓库根目录，在 PowerShell 执行：

```powershell
.\build.bat
```

成功后生成 `renderer.exe`。CMake 和 Visual Studio 解决方案的使用方法见 [README](../README.md#快速开始)。当前代码仍有旧画线示例的数值转换警告，构建可以完成。

## 三步演示

首先运行默认素材：

```powershell
.\renderer.exe
```

预期生成 `output.png`，尺寸为 800×800，使用洛天依素材和 MMD 参数预设。这个预设不加载 PMX，也不包含骨骼或动画。

然后用头部和眼球展示多模型与阴影：

```powershell
.\renderer.exe obj\african_head\african_head.obj obj\african_head\african_head_eye_inner.obj --floor -o output-shadow.png
.\renderer.exe obj\african_head\african_head.obj obj\african_head\african_head_eye_inner.obj --floor --no-shadow -o output-no-shadow.png
```

两张图片仅切换阴影设置。观察地板上的投影，并结合 `Scene::render()` 解释光源深度图和相机主渲染两次处理。

最后用调试视图说明排查方法：

```powershell
.\renderer.exe obj\african_head\african_head.obj -s uv -o output-uv.png
.\renderer.exe obj\african_head\african_head.obj -s normal -o output-normal.png
.\renderer.exe obj\african_head\african_head.obj --dump-depth output-depth.png -o output-head.png
```

UV 视图用于检查纹理坐标，法线视图用于观察方向，深度图用于检查遮挡。它们是排查问题的辅助工具，不等同于自动化正确性测试。

## 阅读代码时关注什么

| 问题 | 入口 |
|---|---|
| 一次渲染如何组织？ | `scene.cpp` 的 `Scene::render()` |
| 如何判断三角形覆盖像素，如何处理遮挡？ | `our_gl.cpp` 的光栅化与深度测试 |
| 为什么 UV 需要透视校正？ | `rasterize_sub_triangle()` 中的插值权重计算 |
| 切换着色方式为什么不需要重写光栅化？ | `IShader` 接口及 `shaders.cpp` |
| 角色贴图错位和画面偏暗如何修复？ | `Texture::sample()` 和着色器中的 sRGB 转换 |

## 历史图片与当前结果

README 中的 `demo-luotianyi.png` 是本次整理时由当前源码生成的结果。其他原有展示图保留为历史演示，不保证与当前默认参数完全一致。

`Project1/output_uvfix.png`、`output_gamma.png`、`output_soft.png`、`output_mmd.png` 是此前角色适配时留下的阶段图片，本次一并归档。因为没有完整记录每张图的命令参数，不将它们作为严格的单变量对照或性能依据。

## 版本和范围

本次经过构建与运行检查的版本以提交记录中的 `docs: prepare student portfolio showcase and annotated sources` 为定位入口。需要固定演示版本时，可以记录该提交的完整哈希，通过 GitHub 的提交页面下载对应源码。

目前没有独立打包的 Release、自动化图像回归测试或性能基准，本文的运行检查不能替代完整测试。高级渲染和优化功能仍属于后续学习方向。
