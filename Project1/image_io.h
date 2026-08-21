#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// image_io —— 渲染结果的落盘出口
//
// 管线位置：最末端。所有渲染阶段结束后，帧缓冲（TGAImage）从这里写到磁盘。
//
// 为什么不直接用 tgaimage.cpp 里的 write_tga_file 就完事？
//   TGA 是个方便的中间格式（tinyrenderer 全系列都用它），但它几乎没有查看器支持，
//   也无法内嵌进 Markdown/网页。求职 demo 的效果图必须能直接展示，所以这里额外
//   实现了一个零依赖的 PNG 编码器（不链接 libpng / zlib）。
//
// PNG 编码的取巧之处：PNG 的图像数据是 zlib 流，而 zlib 允许 deflate 使用
// "stored（非压缩）块"。于是我们不需要实现 LZ77 + 霍夫曼编码，只要正确产出
// zlib 头、若干 stored 块、adler32 校验和以及每个 chunk 的 crc32 即可。
// 代价是文件偏大（比真正压缩的 PNG 大几倍），对 demo 完全够用。
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include "tgaimage.h"

// 按文件后缀分发：.png 走自带的 PNG 编码器，其余（含 .tga）走 TGA 写出。
// 后缀比较不区分大小写。返回是否写出成功。
bool save_image(const TGAImage& image, const std::string& filename);

// 直接写 PNG。灰度图（bpp==1）写成 PNG 灰度类型，其余写成 24 位真彩。
// 注意坐标系：TGAImage 的第 0 行在图像底部，而 PNG 的第 0 行在顶部，
// 因此内部会做一次上下翻转，保证两种格式看起来一致。
bool write_png_file(const TGAImage& image, const std::string& filename);
