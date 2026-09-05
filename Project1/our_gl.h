#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// our_gl —— 渲染管线核心（相当于一块"软件 GPU"）
//
// 它复刻了固定管线中不可编程的那几级，把可编程的两级留给 IShader：
//
//   模型空间 ──ModelView──> 观察空间 ──Projection──> 裁剪空间
//        │                                              │
//        └─────────── 由 IShader::vertex() 负责 ─────────┘
//                                                        │
//                        近平面裁剪 → 透视除法 → 视口变换 → 光栅化
//                                                        │
//                                          逐片元调用 IShader::fragment()
//                                                        │
//                                              深度测试 → 写入帧缓冲
//
// 坐标空间速查：
//   模型空间  obj 文件里的原始坐标
//   世界空间  各模型摆放到一起之后的公共坐标系（本项目在加载后就烘焙好了）
//   观察空间  以相机为原点，相机朝 -z 方向看
//   裁剪空间  乘上投影矩阵之后、做透视除法之前的齐次坐标 (x,y,z,w)
//   NDC       裁剪空间除以 w 之后的结果，可见范围是 [-1,1]³
//   屏幕空间  x,y 是像素坐标，z 是 [0,1] 的深度值
// ─────────────────────────────────────────────────────────────────────────────
#include <vector>
#include "geometry.h"
#include "tgaimage.h"

// ── 三个全局变换矩阵 ─────────────────────────────────────────────────────────
// 之所以做成全局量，是为了贴近 OpenGL 固定管线的心智模型：先设置矩阵状态，
// 再提交几何。着色器在 vertex() 里读它们，管线在光栅化时读 ViewportMatrix。
extern mat<4, 4> ModelView;        // 世界空间 → 观察空间
extern mat<4, 4> ProjectionMatrix; // 观察空间 → 裁剪空间
extern mat<4, 4> ViewportMatrix;   // NDC → 屏幕空间

// 构造观察矩阵：把相机搬到原点、让它沿 -z 方向看向 center。
// up 只用来确定"哪边是上"，不需要严格垂直于视线方向。
void lookat(const vec3& eye, const vec3& center, const vec3& up);

// 透视投影：近大远小。fov_deg 是垂直视场角（度），aspect = 宽/高。
// znear/zfar 是近远裁剪面到相机的距离，均取正值。
void perspective(double fov_deg, double aspect, double znear, double zfar);

// 正交投影：平行光没有"投影中心"，做阴影贴图时必须用它，
// 否则阴影会像被一个点光源投出来一样发散。
// half_height 是可视范围的半高，可视半宽 = half_height * aspect。
void orthographic(double half_height, double aspect, double znear, double zfar);

// 视口变换：把 NDC 的 [-1,1]² 映射到 [x, x+w] × [y, y+h] 像素范围，
// 同时把 NDC 的 z ∈ [-1,1] 映射到深度缓冲惯用的 [0,1]。
void viewport(int x, int y, int w, int h);

// ─────────────────────────────────────────────────────────────────────────────
// 可编程着色器接口
//
// 与 GLSL 的对应关系：
//   uniform —— 派生类的成员变量（整趟渲染不变，如光照方向、各种矩阵）
//   attribute —— 通过 (iface, nthvert) 从 Model 现取
//   varying —— 派生类里按列存放三个顶点属性的小矩阵，由管线插值后交给 fragment
// ─────────────────────────────────────────────────────────────────────────────
struct IShader {
    virtual ~IShader() = default;

    // 顶点着色器：处理三角形 iface 的第 nthvert(0/1/2) 个顶点，
    // 顺便把需要插值的属性写进自己的 varying 成员。
    // 返回值必须是 **裁剪空间** 的齐次坐标（还没做透视除法）。
    virtual vec4 vertex(int iface, int nthvert) = 0;

    // 片元着色器：bar 是该像素相对于当前三角形三个顶点的重心坐标
    //（已做透视校正，三个分量之和为 1）。
    // 返回 false 表示丢弃该片元（不写颜色也不写深度）。
    virtual bool fragment(const vec3& bar, TGAColor& color) = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// 渲染目标：颜色缓冲 + 深度缓冲
//
// 深度约定：值越小越靠近相机；初值为 +∞（kFarDepth）表示"什么都没画"。
// ─────────────────────────────────────────────────────────────────────────────
struct RenderTarget {
    static constexpr double kFarDepth = 1e30;

    TGAImage            color;
    std::vector<double> depth;
    int w = 0, h = 0;

    RenderTarget() = default;
    RenderTarget(int width, int height, TGAColor background = {});

    // 深度缓冲是否被写过（用于判断像素是背景还是几何体）
    bool covered(int x, int y) const { return depth[static_cast<std::size_t>(x) + static_cast<std::size_t>(y) * w] < kFarDepth; }
};

// 光栅化一个三角形。clip 是三个顶点在 **裁剪空间** 的齐次坐标，
// 必须与调用 shader.vertex() 的顺序一致（片元里的重心坐标依此为准）。
// backface_cull 为 true 时丢弃背向相机的三角形。
void rasterize(const vec4 clip[3], IShader& shader, RenderTarget& target, bool backface_cull);

// 把深度缓冲可视化成灰度图：近处亮、远处暗，未覆盖的像素为黑。
// 只在有几何的像素范围内做归一化，否则整张图会被 +∞ 压成全黑。
TGAImage depth_to_image(const RenderTarget& target);

// 把高分辨率图像做盒式降采样（超采样抗锯齿 SSAA 的第二步）。
// factor 为 1 时直接返回原图。
TGAImage downsample(const TGAImage& src, int factor);
