#include <cmath>
#include <algorithm>
#include <limits>
#include "our_gl.h"

// 三个全局矩阵的定义，初值为单位阵（等价于"什么都不变换"）
mat<4, 4> ModelView        = mat<4, 4>::identity();
mat<4, 4> ProjectionMatrix = mat<4, 4>::identity();
mat<4, 4> ViewportMatrix   = mat<4, 4>::identity();

// ─────────────────────────────────────────────────────────────────────────────
// 观察矩阵
//
// 思路：先在世界空间里为相机建立一组标准正交基 (x, y, z)，其中 z 指向相机
// "背后"（因为约定相机沿 -z 看）。这组基构成的旋转矩阵 R 能把世界方向转换到
// 相机方向；由于 R 是正交矩阵，其逆等于转置，所以直接把基向量按 **行** 摆放
// 就得到了 R⁻¹。再左乘一个把 eye 平移到原点的矩阵即可。
// ─────────────────────────────────────────────────────────────────────────────
void lookat(const vec3& eye, const vec3& center, const vec3& up) {
    const vec3 z = normalized(eye - center);       // 视线的反方向
    const vec3 x = normalized(cross(up, z));       // 相机的右方向
    const vec3 y = normalized(cross(z, x));        // 真正的上方向（把 up 正交化）

    mat<4, 4> rotation_inv = mat<4, 4>::identity();
    mat<4, 4> translation  = mat<4, 4>::identity();
    for (int i = 0; i < 3; i++) {
        rotation_inv[0][i] = x[i];                 // 按行摆放 = 转置 = 求逆
        rotation_inv[1][i] = y[i];
        rotation_inv[2][i] = z[i];
        translation[i][3]  = -eye[i];              // 把相机位置搬到原点
    }
    ModelView = rotation_inv * translation;        // 先平移后旋转
}

// ─────────────────────────────────────────────────────────────────────────────
// 透视投影矩阵
//
// 关键在第四行 (0,0,-1,0)：它把观察空间的 -z（深度）搬进齐次坐标的 w 分量。
// 之后光栅化阶段做的"除以 w"，就等于"除以深度"，这正是近大远小的来源。
// 第三行则把 z 重新映射到 NDC 的 [-1,1]，好让深度缓冲有固定的取值范围。
// ─────────────────────────────────────────────────────────────────────────────
void perspective(const double fov_deg, const double aspect,
                 const double znear, const double zfar) {
    const double f = 1.0 / std::tan(fov_deg * 3.14159265358979323846 / 360.0); // cot(fov/2)

    mat<4, 4> p;                                   // 全零起步，只填非零项
    p[0][0] = f / aspect;
    p[1][1] = f;
    p[2][2] = (zfar + znear) / (znear - zfar);
    p[2][3] = 2.0 * zfar * znear / (znear - zfar);
    p[3][2] = -1.0;                                // w' = -z_view，即 w' = 到相机的距离
    ProjectionMatrix = p;
}

// ─────────────────────────────────────────────────────────────────────────────
// 正交投影矩阵
//
// 没有"除以深度"这一步（w 恒为 1），所以物体不会近大远小。
// 平行光的阴影贴图必须用它：平行光的光线本来就互相平行，没有汇聚点。
// ─────────────────────────────────────────────────────────────────────────────
void orthographic(const double half_height, const double aspect,
                  const double znear, const double zfar) {
    mat<4, 4> p = mat<4, 4>::identity();
    p[0][0] = 1.0 / (half_height * aspect);
    p[1][1] = 1.0 / half_height;
    p[2][2] = -2.0 / (zfar - znear);
    p[2][3] = -(zfar + znear) / (zfar - znear);
    ProjectionMatrix = p;
}

// ─────────────────────────────────────────────────────────────────────────────
// 视口变换
//
// NDC 的 [-1,1] 是个"分辨率无关"的中间表示；这一步才把它落到具体像素上。
// z 分量顺带从 [-1,1] 压到 [0,1]，与深度缓冲的取值约定对齐。
// ─────────────────────────────────────────────────────────────────────────────
void viewport(const int x, const int y, const int w, const int h) {
    mat<4, 4> m = mat<4, 4>::identity();
    m[0][0] = w / 2.0;  m[0][3] = x + w / 2.0;
    m[1][1] = h / 2.0;  m[1][3] = y + h / 2.0;
    m[2][2] = 0.5;      m[2][3] = 0.5;
    ViewportMatrix = m;
}

// ─────────────────────────────────────────────────────────────────────────────
// 渲染目标
// ─────────────────────────────────────────────────────────────────────────────
RenderTarget::RenderTarget(const int width, const int height, const TGAColor background)
    : color(width, height, TGAImage::RGB),
      depth(static_cast<std::size_t>(width) * height, kFarDepth),
      w(width), h(height) {
    for (int j = 0; j < height; j++)
        for (int i = 0; i < width; i++)
            color.set(i, j, background);
}

namespace {

    // 裁剪过程中的顶点：除了裁剪空间坐标，还要记住它相对于 **原三角形** 的重心坐标。
    //
    // 这是本文件里最值得说的一个设计。近平面裁剪会切出新的顶点，如果只保留位置，
    // 后续插值就找不回原三角形的属性了；常见做法是让裁剪器去插值所有 varying，
    // 但那样 varying 就必须泛型化，着色器接口会被污染。
    // 换个角度：重心坐标本身就是三维空间中的仿射函数，可以像普通属性一样插值。
    // 于是只要给每个裁剪后的顶点带上"我在原三角形里的重心坐标"，
    // 光栅化时把子三角形的重心坐标换算回原三角形的重心坐标即可，
    // IShader 完全感知不到裁剪的存在。
    struct ClipVertex {
        vec4 pos;   // 裁剪空间齐次坐标
        vec3 bary;  // 相对于原三角形三个顶点的重心坐标
    };

    // 近平面的隐函数：d(v) = v.z + v.w。
    // d > 0 在近平面之内（相机前方），d = 0 恰在近平面上，d < 0 需要裁掉。
    // 之所以是这个式子：NDC 的近平面是 z/w = -1，即 z + w = 0。
    inline double near_plane_distance(const vec4& v) {
        return v.z + v.w;
    }

    // Sutherland–Hodgman 多边形裁剪，只针对近平面这一个平面。
    //
    // 为什么只裁近平面？左右上下四个面可以靠光栅化时的屏幕包围盒求交来"免费"完成；
    // 远平面裁掉与否对结果没影响。唯独近平面非裁不可——一旦顶点跑到相机后面，
    // w 会变成 0 或负数，透视除法要么除零，要么把几何翻到屏幕另一侧。
    int clip_against_near_plane(const ClipVertex in[3], ClipVertex out[4]) {
        int n = 0;
        for (int i = 0; i < 3; i++) {
            const ClipVertex& cur  = in[i];
            const ClipVertex& next = in[(i + 1) % 3];
            const double d_cur  = near_plane_distance(cur.pos);
            const double d_next = near_plane_distance(next.pos);

            if (d_cur >= 0) out[n++] = cur;                 // 当前点在内侧，保留

            if ((d_cur >= 0) != (d_next >= 0)) {            // 这条边穿过了近平面
                // 求交点参数：沿边线性插值到 d == 0 处
                const double t = d_cur / (d_cur - d_next);
                ClipVertex hit;
                hit.pos  = cur.pos  + (next.pos  - cur.pos)  * t;
                hit.bary = cur.bary + (next.bary - cur.bary) * t;
                out[n++] = hit;
            }
        }
        return n; // 0（全裁掉）、3（原样或切成三角形）或 4（切成四边形）
    }

    // 屏幕空间的两倍有向面积。正负号代表三角形的绕序，用来做背面剔除。
    inline double signed_area2(const vec3& a, const vec3& b, const vec3& c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    }

    // 光栅化一个"已经在近平面内侧"的子三角形。
    // screen[] 是屏幕空间坐标（x,y 为像素，z 为 [0,1] 深度），
    // w_clip[] 是各顶点在裁剪空间的 w（透视校正要用），
    // bary_of[] 是各顶点相对于原三角形的重心坐标。
    void rasterize_sub_triangle(const vec3 screen[3], const double w_clip[3],
                                const vec3 bary_of[3],
                                IShader& shader, RenderTarget& target,
                                const bool backface_cull) {
        const double area2 = signed_area2(screen[0], screen[1], screen[2]);

        // 退化三角形（三点共线）没有面积，直接跳过，同时避免后面除零
        if (std::abs(area2) < 1e-12) return;

        // 背面剔除：约定逆时针（面积为正）为正面。
        // 对封闭模型来说，背面本来就会被正面挡住，提前扔掉能省一半光栅化开销。
        if (backface_cull && area2 < 0) return;

        // 屏幕空间包围盒，并与画布求交——这一步顺带完成了左右上下四个面的裁剪
        int xmin = static_cast<int>(std::floor(std::min({ screen[0].x, screen[1].x, screen[2].x })));
        int xmax = static_cast<int>(std::ceil (std::max({ screen[0].x, screen[1].x, screen[2].x })));
        int ymin = static_cast<int>(std::floor(std::min({ screen[0].y, screen[1].y, screen[2].y })));
        int ymax = static_cast<int>(std::ceil (std::max({ screen[0].y, screen[1].y, screen[2].y })));
        xmin = std::max(0, xmin); ymin = std::max(0, ymin);
        xmax = std::min(target.w - 1, xmax); ymax = std::min(target.h - 1, ymax);

        for (int y = ymin; y <= ymax; y++) {
            for (int x = xmin; x <= xmax; x++) {
                // 采样点取像素中心，避免相邻三角形在共享边上反复命中同一像素
                const vec3 p{ x + 0.5, y + 0.5, 0 };

                // ── 屏幕空间重心坐标 ──
                // 每个分量都是"对面子三角形的面积占比"，三者之和恒为 1。
                vec3 bc_screen{
                    signed_area2(screen[1], screen[2], p) / area2,
                    signed_area2(screen[2], screen[0], p) / area2,
                    signed_area2(screen[0], screen[1], p) / area2
                };
                // 任一分量为负说明采样点落在三角形外
                if (bc_screen.x < 0 || bc_screen.y < 0 || bc_screen.z < 0) continue;

                // ── 深度插值 ──
                // 注意这里用的是 **未经透视校正** 的 bc_screen。
                // 因为屏幕空间中真正随 x,y 线性变化的量是 z/w，而不是 z 本身；
                // screen[i].z 已经是除过 w 的结果，所以直接线性插值就是对的。
                const double z = bc_screen.x * screen[0].z
                               + bc_screen.y * screen[1].z
                               + bc_screen.z * screen[2].z;

                const std::size_t idx = static_cast<std::size_t>(x) + static_cast<std::size_t>(y) * target.w;
                if (z >= target.depth[idx]) continue;   // 深度测试：被已有的更近的片元挡住

                // ── 透视校正 ──
                // 顶点属性（uv、法线……）在 **三维空间** 里线性变化，
                // 但投影是非线性的，所以在屏幕上它们不再线性。
                // 正确的权重是 bc_screen[i]/w[i] 再重新归一化。
                // 少了这一步，斜置平面上的贴图会明显扭曲（经典的"仿射贴图"瑕疵）。
                vec3 bc_clip{ bc_screen.x / w_clip[0],
                              bc_screen.y / w_clip[1],
                              bc_screen.z / w_clip[2] };
                const double sum = bc_clip.x + bc_clip.y + bc_clip.z;
                if (std::abs(sum) < 1e-12) continue;
                bc_clip = bc_clip / sum;

                // ── 换算回原三角形的重心坐标 ──
                // 重心坐标是仿射量，可以像普通属性一样用透视校正后的权重插值。
                // 没有发生裁剪时，bary_of 就是三个单位向量，这里等价于恒等变换。
                const vec3 bar = bary_of[0] * bc_clip.x
                               + bary_of[1] * bc_clip.y
                               + bary_of[2] * bc_clip.z;

                TGAColor color;
                if (!shader.fragment(bar, color)) continue; // 着色器主动丢弃该片元

                target.depth[idx] = z;
                target.color.set(x, y, color);
            }
        }
    }

} // namespace

void rasterize(const vec4 clip[3], IShader& shader, RenderTarget& target, const bool backface_cull) {
    // ── 第一步：近平面裁剪 ────────────────────────────────────────────────
    ClipVertex in[3] = {
        { clip[0], vec3{ 1, 0, 0 } },
        { clip[1], vec3{ 0, 1, 0 } },
        { clip[2], vec3{ 0, 0, 1 } },
    };
    ClipVertex poly[4];
    const int n = clip_against_near_plane(in, poly);
    if (n < 3) return; // 整个三角形都在近平面之外

    // ── 第二步：透视除法 + 视口变换 ──────────────────────────────────────
    vec3   screen[4];
    double w_clip[4];
    for (int i = 0; i < n; i++) {
        w_clip[i] = poly[i].pos.w;
        if (std::abs(w_clip[i]) < 1e-12) return;              // 理论上裁剪后不会发生，保险起见
        const vec4 ndc = poly[i].pos / w_clip[i];             // 透视除法：这一除，远处的东西就小了
        screen[i] = proj<3>(ViewportMatrix * ndc);            // NDC → 像素坐标 + [0,1] 深度
    }

    // ── 第三步：把裁剪后的多边形拆成三角形扇，逐个光栅化 ─────────────────
    // 裁剪结果最多 4 个顶点，所以这里最多迭代两次。
    for (int i = 1; i + 1 < n; i++) {
        const vec3   sub_screen[3] = { screen[0],   screen[i],   screen[i + 1]   };
        const double sub_w[3]      = { w_clip[0],   w_clip[i],   w_clip[i + 1]   };
        const vec3   sub_bary[3]   = { poly[0].bary, poly[i].bary, poly[i + 1].bary };
        rasterize_sub_triangle(sub_screen, sub_w, sub_bary, shader, target, backface_cull);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 深度缓冲可视化
// ─────────────────────────────────────────────────────────────────────────────
TGAImage depth_to_image(const RenderTarget& target) {
    // 用 RGB 而非灰度存放，这样它能和普通渲染结果走完全相同的降采样与保存路径
    TGAImage out(target.w, target.h, TGAImage::RGB);

    // 先统计有几何的像素的深度范围。直接按 [0,1] 映射的话，
    // 模型往往只占其中很窄的一段，出来的图几乎是纯色，看不出层次。
    double dmin = RenderTarget::kFarDepth, dmax = -RenderTarget::kFarDepth;
    for (const double d : target.depth) {
        if (d >= RenderTarget::kFarDepth) continue;
        dmin = std::min(dmin, d);
        dmax = std::max(dmax, d);
    }
    if (dmin > dmax) return out; // 整张图都是背景

    const double range = std::max(1e-9, dmax - dmin);
    for (int y = 0; y < target.h; y++) {
        for (int x = 0; x < target.w; x++) {
            const double d = target.depth[static_cast<std::size_t>(x) + static_cast<std::size_t>(y) * target.w];
            if (d >= RenderTarget::kFarDepth) continue;        // 背景保持黑色
            // 反转一下：近处亮、远处暗，更符合"离得近就更显眼"的直觉
            const double t = 1.0 - (d - dmin) / range;
            const auto g = static_cast<std::uint8_t>(std::lround(std::clamp(t, 0.0, 1.0) * 255));
            out.set(x, y, TGAColor{ g, g, g, 255, 3 });
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// 超采样降采样（SSAA 的第二步）
//
// 抗锯齿的本质是：一个像素本来就该是它覆盖范围内颜色的平均值，
// 而单点采样只取了中心那一个样本。SSAA 的做法很直接——
// 先按 factor² 倍的采样密度渲染，再把每个 factor×factor 的块求平均。
// 它无差别地作用于几何边缘、贴图细节和高光，缺点只有一个：慢 factor² 倍。
// ─────────────────────────────────────────────────────────────────────────────
TGAImage downsample(const TGAImage& src, const int factor) {
    if (factor <= 1) return src;

    const int w = src.width() / factor;
    const int h = src.height() / factor;
    TGAImage out(w, h, TGAImage::RGB);

    const int samples = factor * factor;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int acc[3] = { 0, 0, 0 };
            for (int dy = 0; dy < factor; dy++)
                for (int dx = 0; dx < factor; dx++) {
                    const TGAColor c = src.get(x * factor + dx, y * factor + dy);
                    acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2];
                }
            out.set(x, y, TGAColor{
                static_cast<std::uint8_t>(acc[0] / samples),
                static_cast<std::uint8_t>(acc[1] / samples),
                static_cast<std::uint8_t>(acc[2] / samples),
                255, 3 });
        }
    }
    return out;
}
