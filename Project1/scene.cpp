#include <cmath>
#include <memory>
#include <iostream>
#include <algorithm>
#include "scene.h"

bool Scene::add_model(const std::string& path, const bool force_checker) {
    // Model 构造函数负责真正的 OBJ/MTL/贴图加载。
    // Scene 这里只做"是否接受进场景"这一层判断，避免半加载失败的模型污染后续流程。
    Model m(path, force_checker);
    if (!m.valid()) return false;
    models_.push_back(std::move(m));
    return true;
}

void Scene::bbox(vec3& vmin, vec3& vmax) const {
    if (models_.empty()) { vmin = vmax = vec3{}; return; }

    // 多模型场景要合并每个模型自己的 AABB。
    // first 用来处理"第一个有效模型"：不能把 vmin/vmax 初始化成 0，
    // 否则全在正坐标或全在负坐标的模型会被原点错误地撑大包围盒。
    bool first = true;
    for (const Model& m : models_) {
        if (m.nverts() == 0) continue;
        vec3 lo, hi;
        m.bbox(lo, hi);
        if (first) { vmin = lo; vmax = hi; first = false; continue; }
        for (const int i : {0, 1, 2}) {
            vmin[i] = std::min(vmin[i], lo[i]);
            vmax[i] = std::max(vmax[i], hi[i]);
        }
    }
    if (first) vmin = vmax = vec3{};
}

void Scene::fit_to_unit_cube() {
    if (models_.empty()) return;

    // 归一化发生在 Scene 层，而不是 Model 层。
    // 这样多个 OBJ（例如头、眼睛、附件）会一起缩放和平移，彼此的相对位置不会变。
    vec3 vmin, vmax;
    bbox(vmin, vmax);

    const vec3 center = (vmin + vmax) * 0.5;

    // 用最长边的一半作为缩放基准，保证三个轴都落进 [-1,1]。
    // 用最长边而非各轴分别缩放，是为了不破坏模型的长宽比。
    double half = 0;
    for (const int i : {0, 1, 2}) half = std::max(half, (vmax[i] - vmin[i]) * 0.5);
    if (half < 1e-12) half = 1.0;  // 退化场景（所有点重合）防除零

    // 先平移到原点，再等比缩放。矩阵按"从右往左作用"读：S · T
    mat<4, 4> T = mat<4, 4>::identity();
    for (const int i : {0, 1, 2}) T[i][3] = -center[i];
    mat<4, 4> S = mat<4, 4>::identity();
    for (const int i : {0, 1, 2}) S[i][i] = 1.0 / half;

    const mat<4, 4> M = S * T;
    for (Model& m : models_) m.transform(M);
}

void Scene::add_floor() {
    vec3 vmin, vmax;
    bbox(vmin, vmax);

    // 地板铺在场景底部，稍微下沉一点点，避免与贴地的模型面重合导致 z-fighting
    const double y = vmin.y - (vmax.y - vmin.y) * 0.002 - 1e-4;
    const vec3 c{ (vmin.x + vmax.x) * 0.5, y, (vmin.z + vmax.z) * 0.5 };

    // 地板尺寸取场景水平跨度的 2.5 倍，既能接住阴影又不会大到抢镜
    const double span = std::max({ vmax.x - vmin.x, vmax.z - vmin.z, 1e-3 });
    models_.push_back(Model::make_floor(c, span * 1.25, 10));
}

void Scene::prepare(const RenderOptions& opt) {
    // 预处理顺序很重要：先把模型 fit 到标准空间，再按标准空间的尺寸添加地板。
    // 反过来会把地板也一起参与 fit，导致主体模型在画面里变小。
    if (opt.fit)   fit_to_unit_cube();
    if (opt.floor) add_floor();
}

void Scene::setup_camera(const RenderOptions& opt, const int w, const int h) const {
    // 三个矩阵依次设置全局管线状态：
    //   lookat      世界空间 → 相机/观察空间
    //   perspective 观察空间 → 裁剪空间
    //   viewport    NDC → 像素坐标
    // 顶点着色器和光栅化器会读取这些全局矩阵。
    lookat(opt.eye, opt.center, opt.up);
    perspective(opt.fov, static_cast<double>(w) / h, 0.05, 100.0);
    viewport(0, 0, w, h);
}

namespace {

    // 按着色模式创建对应的着色器。
    // 用工厂函数而非在渲染循环里 switch，是为了让循环体保持"提交几何"这一件事。
    std::unique_ptr<IShader> make_shader(const ShaderMode mode,
                                         const Model& model,
                                         const Lighting& light) {
        // 每个 Model 拿到自己的 shader 实例，因为 varying_* 成员会在 draw_model()
        // 里被当前三角形不断覆盖，不能在多个模型之间共享。
        switch (mode) {
        case ShaderMode::Flat:    return std::make_unique<FlatShader>(model, light);
        case ShaderMode::Gouraud: return std::make_unique<GouraudShader>(model, light);
        case ShaderMode::Normal:  return std::make_unique<DebugShader>(model, DebugShader::Kind::Normal);
        case ShaderMode::UV:      return std::make_unique<DebugShader>(model, DebugShader::Kind::UV);
        case ShaderMode::Depth:   // 深度模式只要把深度缓冲填出来，用最便宜的着色器即可
        case ShaderMode::Phong:
        case ShaderMode::Wire:
        default:                  return std::make_unique<PhongShader>(model, light);
        }
    }

    // 把一个模型的所有三角形喂给光栅化器
    void draw_model(const Model& model, IShader& shader, RenderTarget& target, const bool cull) {
        for (int f = 0; f < model.nfaces(); f++) {
            vec4 clip[3];
            for (int v = 0; v < 3; v++)
                clip[v] = shader.vertex(f, v);   // 顶点着色器：模型属性 → 裁剪空间坐标
            // rasterize 负责裁剪、透视除法、重心插值、深度测试和调用 fragment。
            // 从 Scene 角度看，它就是"提交一个三角形"。
            rasterize(clip, shader, target, cull);
        }
    }

} // namespace

TGAImage Scene::render(const RenderOptions& opt, TGAImage* depth_out) const {
    // 超采样：先按放大后的分辨率渲染，最后再缩回去
    const int ss = std::clamp(opt.ssaa, 1, 4);
    const int w  = opt.width  * ss;
    const int h  = opt.height * ss;

    Lighting light;
    // 把命令行/默认配置里的渲染选项翻译成 shader 需要的光照状态。
    // 这一步集中做，后面每个 shader 都读同一个 Lighting，避免参数到处传。
    light.light_dir         = normalized(opt.light_dir);
    light.ambient           = opt.ambient;
    light.eye               = opt.eye;
    light.use_normal_map    = opt.normal_map;
    light.use_spec          = opt.spec;
    light.shininess_override = opt.shininess;

    // ── 第一趟：阴影贴图 ──────────────────────────────────────────────────
    // 用 unique_ptr 是因为 shadow_map 指针要在主趟期间保持有效，
    // 而它只在开启阴影时才存在。
    std::unique_ptr<RenderTarget> shadow_target;
    if (opt.shadow) {
        // 阴影贴图本质上是一张"从光源看过去的深度图"。
        // 主渲染时，只要某个像素比这张图里记录的最近深度更远，就说明它被遮住了。
        vec3 vmin, vmax;
        bbox(vmin, vmax);
        const vec3 center = (vmin + vmax) * 0.5;
        // 场景包围球半径：用对角线长度的一半，保证整个场景都装得下
        const double radius = std::max(1e-3, norm(vmax - vmin) * 0.5);

        // 把"相机"放到光源方向上足够远处，正对场景中心
        const double distance = radius * 3.0;
        const vec3 light_eye = center + light.light_dir * distance;

        // up 不能与视线方向平行，否则叉乘退化成零向量。
        // 光从正上方打下来时（light_dir ≈ (0,1,0)）就会撞上这种情况。
        const vec3 up = (std::abs(light.light_dir.y) > 0.99) ? vec3{ 0, 0, 1 } : vec3{ 0, 1, 0 };

        // 阴影图分辨率：至少 1024，且不低于主趟分辨率。
        // 分辨率太低时，一个阴影纹素覆盖太多屏幕像素，阴影边缘会呈现锯齿状台阶。
        const int sm_size = std::max(1024, std::max(w, h));

        shadow_target = std::make_unique<RenderTarget>(sm_size, sm_size);

        lookat(light_eye, center, up);
        // 平行光要用正交投影：光线本身互相平行，没有汇聚中心。
        // 用透视投影的话，阴影会随距离发散，看起来像点光源打出来的。
        orthographic(radius * 1.05, 1.0, std::max(1e-3, distance - radius * 1.5),
                                          distance + radius * 1.5);
        viewport(0, 0, sm_size, sm_size);

        // 记下"世界空间 → 阴影图屏幕空间"的完整变换链，片元着色时要用它做查询
        light.shadow_matrix = ViewportMatrix * ProjectionMatrix * ModelView;

        for (const Model& m : models_) {
            DepthShader depth_shader(m);
            // 阴影趟必须关闭背面剔除：物体的背面同样会挡光，
            // 剔掉它们会让薄壁物体（比如地板下方）漏光。
            draw_model(m, depth_shader, *shadow_target, false);
        }
        light.shadow_map = shadow_target.get();
    }

    // ── 第二趟：主渲染 ────────────────────────────────────────────────────
    setup_camera(opt, w, h);

    RenderTarget target(w, h, opt.background);
    for (const Model& m : models_) {
        // 主趟按用户选择创建不同 shader；但提交三角形的路径完全相同。
        // 这正是把 IShader 抽象出来的意义：管线不关心具体光照公式。
        std::unique_ptr<IShader> shader = make_shader(opt.mode, m, light);
        draw_model(m, *shader, target, opt.cull);
    }

    if (depth_out) *depth_out = downsample(depth_to_image(target), ss);

    // 深度模式下，我们要的就是深度图本身
    if (opt.mode == ShaderMode::Depth)
        return downsample(depth_to_image(target), ss);

    // ── 第三趟（纯图像处理）：降采样 ──────────────────────────────────────
    return downsample(target.color, ss);
}
