#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// texture —— 贴图采样
//
// 管线位置：片元着色阶段。着色器拿到插值后的纹理坐标 uv，来这里换取颜色。
//
// 这一层存在的意义是把三件麻烦事挡在着色器外面：
//   1. TGAColor 内部是 BGRA 字节序，而着色计算要的是 RGB；
//   2. 贴图是离散的整数网格，而 uv 是连续的浮点数 —— 需要双线性插值，
//      否则模型放大后会出现明显的马赛克块；
//   3. uv 可能超出 [0,1]（建模时的平铺），需要环绕（repeat）处理。
//
// 约定：uv 采用 OBJ / OpenGL 的习惯，(0,0) 在贴图左下角，(1,1) 在右上角。
//       TGAImage 加载后第 0 行在顶部，sample() 通过 1-v 转换到该行序。
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include "tgaimage.h"
#include "geometry.h"

class Texture {
    TGAImage image;      // 贴图像素数据
    bool     loaded = false;

public:
    // 从磁盘加载 TGA 贴图。失败时不抛异常、只返回 false，
    // 调用方（Model）据此回落到材质颜色或程序化贴图。
    bool load(const std::string& filename);

    // 生成程序化棋盘格：给没有任何贴图的模型兜底。
    // 棋盘格能让 uv 的走向、拉伸和接缝一眼可见，本身也是个好用的调试图案。
    // squares 为整张贴图上棋盘格的格子数。
    void make_checker(int resolution, int squares, vec3 color_a, vec3 color_b);

    bool valid() const { return loaded; }
    int  width()  const { return image.width(); }
    int  height() const { return image.height(); }

    // 双线性采样，返回归一化到 [0,1] 的 RGB。
    // uv 超出 [0,1] 时按 repeat 环绕。贴图无效时返回洋红色以便一眼看出问题。
    vec3 sample(const vec2& uv) const;

    // 单通道采样（高光贴图只需要一个强度值），取 R 通道，范围 [0,1]。
    double sample_gray(const vec2& uv) const;

    // 把切线空间法线贴图解码为向量：贴图里存的是 [0,1] 的颜色，
    // 对应 [-1,1] 的法线分量（x=R, y=G, z=B），所以要做 n = 2c - 1。
    vec3 sample_normal(const vec2& uv) const;
};
