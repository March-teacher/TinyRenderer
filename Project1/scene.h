#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// scene —— 场景组装与渲染流程编排
//
// 管线位置：最外层的驱动者。它决定"渲染几趟、每趟用什么矩阵、结果怎么合并"，
// 而不关心单个三角形是怎么被填出来的（那是 our_gl 的事）。
//
// 一次完整渲染的流程：
//   1. 加载若干模型 → 统一归一化到单位立方体（保持模型之间的相对位置）
//   2. 可选地加一块程序化地板（用来接住阴影）
//   3. 阴影趟：以光源为相机渲染深度图，并记下"世界空间 → 阴影图"的矩阵
//   4. 主趟：以真实相机在 ssaa 倍分辨率下渲染
//   5. 把主趟结果盒式降采样回目标分辨率（抗锯齿）
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include <vector>
#include "model.h"
#include "our_gl.h"
#include "shaders.h"

// 一次渲染的全部可调参数，基本与命令行选项一一对应
struct RenderOptions {
    int  width  = 800;
    int  height = 800;
    int  ssaa   = 2;                       // 超采样倍率（1 = 关闭抗锯齿）

    ShaderMode mode = ShaderMode::Phong;

    // 相机
    // 默认取一个略偏右上的四分之三视角：正面视角看不出立体感，
    // 侧光加上这个角度才能把鼻梁、眼窝的起伏和投影一起交代清楚。
    // 距离留了一点余量，保证归一化到 [-1,1]³ 的模型不会被画幅切到。
    vec3   eye    = { 1.0, 0.55, 3.2 };
    vec3   center = { 0.0, 0.0, 0.0 };
    vec3   up     = { 0.0, 1.0, 0.0 };
    double fov    = 40.0;                  // 垂直视场角（度）

    // 光照
    vec3   light_dir = normalized(vec3{ 1.0, 1.0, 1.0 }); // 由表面指向光源
    double ambient   = 0.18;
    bool   normal_map = true;
    bool   spec       = true;
    double shininess  = 0;                 // >0 时覆盖材质的 Ns

    // 开关
    bool shadow = true;
    bool floor  = false;
    bool cull   = true;                    // 背面剔除
    bool fit    = true;                    // 把场景归一化到单位立方体

    TGAColor background = { 28, 22, 18, 255 }; // BGRA：一点暖调的深色背景
};

class Scene {
    std::vector<Model> models_;

public:
    // 加载一个模型；返回是否成功。失败时不会把模型加进场景。
    bool add_model(const std::string& path, bool force_checker = false);

    const std::vector<Model>& models() const { return models_; }
    bool empty() const { return models_.empty(); }

    // 场景整体的轴对齐包围盒
    void bbox(vec3& vmin, vec3& vmax) const;

    // 把 **整个场景** 平移到原点并等比缩放到 [-1,1]³。
    // 关键点是所有模型共用一次变换：如果各自归一化，
    // 人头和眼球就会被缩放到不同比例，五官全部错位。
    void fit_to_unit_cube();

    // 在场景底部加一块程序化地板。必须在 fit 之后调用，
    // 这样地板的尺寸才能按最终的场景大小来定。
    void add_floor();

    // 依据选项完成 fit / 加地板等预处理。渲染前调用一次。
    void prepare(const RenderOptions& opt);

    // 设置本趟渲染的三个全局矩阵（供主趟和线框模式共用）
    void setup_camera(const RenderOptions& opt, int w, int h) const;

    // 执行完整渲染（阴影趟 + 主趟 + 降采样），返回最终图像。
    // 线框模式不走这里，由 main.cpp 单独处理。
    // depth_out 非空时，额外输出主趟的深度可视化图（已降采样）。
    TGAImage render(const RenderOptions& opt, TGAImage* depth_out = nullptr) const;
};
