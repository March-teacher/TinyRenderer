#include <cmath>
#include <algorithm>
#include <iostream>
#include "texture.h"

namespace {
    // 把整数纹素坐标环绕到 [0, n)。用取模而非取余，保证负数也能正确回绕。
    inline int wrap(const int i, const int n) {
        if (n <= 0) return 0;
        const int m = i % n;
        return (m < 0) ? m + n : m;
    }
}

bool Texture::load(const std::string& filename) {
    loaded = image.read_tga_file(filename);
    return loaded;
}

void Texture::make_checker(const int resolution, const int squares,
                           const vec3 color_a, const vec3 color_b) {
    image = TGAImage(resolution, resolution, TGAImage::RGB);
    // 每个格子占多少像素；至少 1 像素，避免 squares 过大时除零
    const int cell = (squares > 0) ? std::max(1, resolution / squares) : 1;
    for (int y = 0; y < resolution; y++) {
        for (int x = 0; x < resolution; x++) {
            // (行号 + 列号) 的奇偶决定用哪种颜色，这就是棋盘格的全部秘密
            const bool even = ((x / cell) + (y / cell)) % 2 == 0;
            const vec3& c = even ? color_a : color_b;
            image.set(x, y, TGAColor{ // 注意 TGAColor 是 BGRA 序
                static_cast<std::uint8_t>(std::lround(c.z * 255)),
                static_cast<std::uint8_t>(std::lround(c.y * 255)),
                static_cast<std::uint8_t>(std::lround(c.x * 255)),
                255, 3 });
        }
    }
    loaded = true;
}

vec3 Texture::sample(const vec2& uv) const {
    if (!loaded) return { 1, 0, 1 }; // 洋红：约定俗成的"贴图缺失"色

    const int w = image.width(), h = image.height();

    // ── 双线性插值 ────────────────────────────────────────────────────────
    // 纹素的颜色值代表其"中心"处的采样，中心位于整数坐标 + 0.5，
    // 所以先减去 0.5 换算到"以纹素中心为格点"的坐标系，再取整数部分和小数部分。
    // 漏掉这个 -0.5 会让贴图整体偏移半个纹素（放大后肉眼可辨）。
    const double x = uv.x * w - 0.5;
    // TGAImage::read_tga_file 会把不同 TGA 原点统一成“第 0 行在图像顶部”，
    // 而 OBJ/OpenGL 的纹理坐标以左下角为 (0,0)，因此采样前要翻转 V。
    // 不做这一步时，使用图集的模型会采到上下相反的区域，例如脸部读到
    // 深色区域、衣服读到其他部件的颜色。
    const double y = (1.0 - uv.y) * h - 0.5;

    const double fx = std::floor(x), fy = std::floor(y);
    const double tx = x - fx, ty = y - fy;   // 两个方向上的插值权重，∈[0,1)

    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);

    // 取周围四个纹素，行列均做环绕
    vec3 c[2][2];
    for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
            const TGAColor t = image.get(wrap(x0 + dx, w), wrap(y0 + dy, h));
            // TGAColor 是 BGRA：灰度图只有一个通道，三通道都用它
            c[dy][dx] = (image.bytespp() == TGAImage::GRAYSCALE)
                      ? vec3{ t[0] / 255.0, t[0] / 255.0, t[0] / 255.0 }
                      : vec3{ t[2] / 255.0, t[1] / 255.0, t[0] / 255.0 };
        }
    }

    // 先沿 x 方向插值两次，再把结果沿 y 方向插值一次
    const vec3 top    = c[1][0] * (1 - tx) + c[1][1] * tx;
    const vec3 bottom = c[0][0] * (1 - tx) + c[0][1] * tx;
    return bottom * (1 - ty) + top * ty;
}

double Texture::sample_gray(const vec2& uv) const {
    if (!loaded) return 0;
    return sample(uv).x;
}

vec3 Texture::sample_normal(const vec2& uv) const {
    if (!loaded) return { 0, 0, 1 }; // 缺失时返回"不做扰动"的默认切线空间法线
    // 颜色 [0,1] → 法线分量 [-1,1]。贴图里 z 分量普遍接近 1（偏蓝），
    // 这正是切线空间法线贴图看起来一片淡蓝的原因。
    const vec3 c = sample(uv);
    const vec3 n = c * 2.0 - vec3{ 1, 1, 1 };
    const double len = norm(n);
    return (len > 1e-9) ? n / len : vec3{ 0, 0, 1 };
}
