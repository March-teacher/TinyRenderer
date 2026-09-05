#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// shaders —— 各种着色模型的具体实现
//
// 管线位置：可编程的两级。它们是 IShader 的派生类，被 our_gl 的光栅化器回调。
//
// 这里刻意把同一个模型用几种不同的着色方式实现了一遍，因为它们的差别正好是
// 图形学入门最核心的几个概念：
//   Flat     每个三角形一个法线      → 能看见网格的棱面，最直观地暴露"模型是多面体"
//   Gouraud  逐顶点算光照再插值      → 曲面变平滑了，但高光会被顶点密度吃掉
//   Phong    插值法线再逐像素算光照  → 高光正确，还能接上法线贴图与阴影
//
// 命名约定沿用 GLSL：
//   uniform_ 前缀 —— 整趟渲染不变的量
//   varying_ 前缀 —— 顶点着色器写入、光栅化器插值、片元着色器读取的量
//                    统一按"列"存放三个顶点，即第 i 列是第 i 个顶点的属性
// ─────────────────────────────────────────────────────────────────────────────
#include "geometry.h"
#include "model.h"
#include "our_gl.h"

// 命令行 --shader 选项对应的着色模式
enum class ShaderMode {
    Wire,     // 线框：不走光栅化，直接画三角形的边
    Flat,     // 平直着色
    Gouraud,  // 逐顶点光照
    Phong,    // 逐像素光照（默认）
    Normal,   // 调试：把法线方向可视化成颜色
    UV,       // 调试：把纹理坐标可视化成颜色
    Depth,    // 调试：输出深度缓冲
};

// ─────────────────────────────────────────────────────────────────────────────
// 整个场景共享的光照 / 阴影参数（相当于 GLSL 里的一组 uniform block）
// ─────────────────────────────────────────────────────────────────────────────
struct Lighting {
    // 由表面 **指向光源** 的单位向量（世界空间）。
    // 之所以存"指向光源"而不是"光的传播方向"，是因为兰伯特项 n·l 直接就要用它。
    vec3   light_dir   = normalized(vec3{ 1, 1, 1 });
    vec3   light_color = { 1, 1, 1 };
    double ambient     = 0.18;   // 环境光系数：粗暴地模拟"来自四面八方的间接光"，
                                 // 没有它的话背光面会是纯黑，看不出任何形状
    vec3   eye         = { 0, 0, 3 }; // 相机位置（世界空间），算高光的视线方向要用

    bool   use_normal_map = true;  // 是否启用法线贴图
    bool   use_spec       = true;  // 是否计算高光
    double shininess_override = 0; // >0 时覆盖材质的 Ns

    // ── 阴影贴图 ──────────────────────────────────────────────────────────
    const RenderTarget* shadow_map = nullptr;              // 从光源视角渲染出的深度图
    mat<4, 4> shadow_matrix = mat<4, 4>::identity();       // 世界空间 → 阴影图的屏幕空间
    double shadow_bias      = 2e-3;                        // 深度偏移，用于消除自遮蔽条纹
    double shadow_darkness  = 0.75;                        // 阴影区里直接光被削减的比例
};

// ─────────────────────────────────────────────────────────────────────────────
// 深度着色器：阴影贴图的第一趟
//
// 只关心"从光源看过去，每个方向上最近的表面有多远"，颜色完全无所谓，
// 所以片元着色器什么也不算，全部交给光栅化器自带的深度测试。
// ─────────────────────────────────────────────────────────────────────────────
struct DepthShader : IShader {
    const Model& model;
    explicit DepthShader(const Model& m) : model(m) {}

    vec4 vertex(int iface, int nthvert) override;
    bool fragment(const vec3& bar, TGAColor& color) override;
};

// ─────────────────────────────────────────────────────────────────────────────
// 平直着色：整个三角形共用一个法线（由三个顶点叉乘现算）
// ─────────────────────────────────────────────────────────────────────────────
struct FlatShader : IShader {
    const Model&    model;
    const Lighting& light;

    mat<3, 3> varying_world;  // 三个顶点的世界坐标（按列存），用来现算面法线
    mat<2, 3> varying_uv;     // 三个顶点的纹理坐标
    int       cur_face = 0;

    FlatShader(const Model& m, const Lighting& l) : model(m), light(l) {}
    vec4 vertex(int iface, int nthvert) override;
    bool fragment(const vec3& bar, TGAColor& color) override;
};

// ─────────────────────────────────────────────────────────────────────────────
// Gouraud 着色：在顶点上算光强，再由光栅化器线性插值
//
// 它是"用插值省计算"的经典代表，也是这一取巧的经典反例：
// 高光是个尖锐的非线性项，若高光点恰好落在三角形内部而非顶点上，就会被完全抹掉；
// 三角形较大时还会看到明显的马赫带。对比 PhongShader 就能看出差别。
// ─────────────────────────────────────────────────────────────────────────────
struct GouraudShader : IShader {
    const Model&    model;
    const Lighting& light;

    vec3      varying_intensity;  // 三个顶点各自的漫反射光强
    mat<2, 3> varying_uv;
    int       cur_face = 0;

    GouraudShader(const Model& m, const Lighting& l) : model(m), light(l) {}
    vec4 vertex(int iface, int nthvert) override;
    bool fragment(const vec3& bar, TGAColor& color) override;
};

// ─────────────────────────────────────────────────────────────────────────────
// Phong 着色：本项目的主力着色器
//
// 逐像素完成：法线插值 → 切线空间法线贴图扰动 → Blinn-Phong 光照
//            → 漫反射/高光贴图采样 → 阴影查询 → 环境光合成
// ─────────────────────────────────────────────────────────────────────────────
struct PhongShader : IShader {
    const Model&    model;
    const Lighting& light;

    mat<3, 3> varying_normal; // 三个顶点的世界空间法线
    mat<3, 3> varying_world;  // 三个顶点的世界坐标（算视线方向、切线标架和阴影都要用）
    mat<2, 3> varying_uv;     // 三个顶点的纹理坐标
    int       cur_face = 0;

    PhongShader(const Model& m, const Lighting& l) : model(m), light(l) {}
    vec4 vertex(int iface, int nthvert) override;
    bool fragment(const vec3& bar, TGAColor& color) override;
};

// ─────────────────────────────────────────────────────────────────────────────
// 调试着色器：把中间量直接画成颜色
//
// 渲染器出问题时，"把可疑的中间量画出来看一眼"往往比单步调试快得多：
// 法线图能一眼看出法线是否翻转、是否没归一化；
// uv 图能一眼看出纹理坐标是否越界、接缝在哪。
// ─────────────────────────────────────────────────────────────────────────────
struct DebugShader : IShader {
    enum class Kind { Normal, UV };

    const Model& model;
    Kind         kind;

    mat<3, 3> varying_normal;
    mat<2, 3> varying_uv;
    int       cur_face = 0;

    DebugShader(const Model& m, const Kind k) : model(m), kind(k) {}
    vec4 vertex(int iface, int nthvert) override;
    bool fragment(const vec3& bar, TGAColor& color) override;
};
