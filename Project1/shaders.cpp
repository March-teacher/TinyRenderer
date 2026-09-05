#include <cmath>
#include <algorithm>
#include "shaders.h"

namespace {

    // 把线性空间的 [0,1] RGB 打包成 TGAColor（内部为 BGRA 序），超范围的值做截断。
    // 注：这里没有做 gamma 校正，属于有意为之的简化，README 的"已知简化"一节有说明。
    TGAColor to_color(const vec3& rgb) {
        auto q = [](const double v) {
            return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255));
        };
        return TGAColor{ q(rgb.z), q(rgb.y), q(rgb.x), 255, 3 }; // B, G, R, A
    }

    // 顶点变换的公共部分：世界坐标 → 裁剪空间齐次坐标
    inline vec4 to_clip(const vec3& world) {
        return ProjectionMatrix * (ModelView * embed<4>(world, 1.0));
    }

    // ── 阴影查询 ──────────────────────────────────────────────────────────
    // 原理：从光源视角先渲染一张深度图。着色时把当前像素的世界坐标变换到
    // 光源视角的屏幕空间，如果它的深度明显大于深度图里记录的值，
    // 就说明在它和光源之间还夹着别的东西 —— 也就是处在阴影里。
    //
    // 返回值是"直接光的可见比例"：1 表示完全被照亮，0 表示完全在阴影中。
    double shadow_factor(const Lighting& light, const vec3& world, const double n_dot_l) {
        if (!light.shadow_map) return 1.0;

        const vec4 sc = light.shadow_matrix * embed<4>(world, 1.0);
        if (std::abs(sc.w) < 1e-12) return 1.0;
        const vec3 sp = proj<3>(sc / sc.w);   // 阴影图的屏幕坐标 (x像素, y像素, [0,1]深度)

        const RenderTarget& sm = *light.shadow_map;
        const int cx = static_cast<int>(sp.x);
        const int cy = static_cast<int>(sp.y);
        // 落在阴影图之外的区域没有信息可用，按"被照亮"处理，
        // 否则模型周围会凭空多出一圈方形的暗块。
        if (cx < 1 || cy < 1 || cx >= sm.w - 1 || cy >= sm.h - 1) return 1.0;

        // ── 深度偏移（bias）──
        // 阴影图的分辨率有限，一个阴影纹素覆盖了表面上一小片区域，
        // 该区域内所有像素都拿同一个深度值去比较。表面越倾斜，这片区域在深度上
        // 的跨度越大，于是同一个面上就会有一部分"自己挡住自己"，
        // 渲染出成片的斑马纹（shadow acne）。
        // 解法是把比较阈值往光源方向挪一点，且倾斜得越厉害挪得越多。
        const double slope = std::clamp(1.0 - n_dot_l, 0.0, 1.0);
        const double bias  = light.shadow_bias * (1.0 + 8.0 * slope);

        // ── PCF（Percentage Closer Filtering）──
        // 不是只查一个纹素，而是查 3×3 邻域再取平均。
        // 这样阴影边缘会得到 0/1 之间的过渡值，硬边被柔化成几像素宽的软边。
        int lit = 0, total = 0;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                const double stored = sm.depth[static_cast<std::size_t>(cx + dx) +
                                               static_cast<std::size_t>(cy + dy) * sm.w];
                total++;
                // 深度图里是 +∞ 说明光源方向上这里根本没有遮挡物
                if (stored >= RenderTarget::kFarDepth || sp.z <= stored + bias) lit++;
            }
        }
        const double visible = static_cast<double>(lit) / total;

        // shadow_darkness 控制阴影有多黑：1 表示直接光被完全挡住
        return 1.0 - light.shadow_darkness * (1.0 - visible);
    }

    // ── 切线空间标架（Darboux 标架）──────────────────────────────────────
    //
    // 法线贴图里存的是"相对于表面局部坐标系的扰动"，要用它就得先把这个局部坐标系
    // 在世界空间里构造出来：法线 n 已知，还缺切线 t 和副切线 b。
    //
    // t 的定义是"沿着 u 增长最快的方向"。于是问题变成解一个线性方程组：
    // 设三角形两条边为 e1、e2，对应的 uv 差为 (du1,dv1)、(du2,dv2)，则
    //     e1 · t = du1,  e2 · t = du2,  n · t = 0
    // 写成矩阵形式 A·t = (du1, du2, 0)，其中 A 的三行分别是 e1、e2、n。
    // 求逆即得 t；同理把右端换成 dv 就得到 b。
    // 这样算出来的标架与顶点数据无关，不需要预计算逐顶点切线。
    mat<3, 3> darboux_frame(const mat<3, 3>& world, const mat<2, 3>& uv, const vec3& n) {
        mat<3, 3> A;
        A[0] = world.col(1) - world.col(0);    // 边 e1
        A[1] = world.col(2) - world.col(0);    // 边 e2
        A[2] = n;

        const mat<3, 3> AI = A.invert();
        const vec3 t = AI * vec3{ uv[0][1] - uv[0][0], uv[0][2] - uv[0][0], 0 }; // du
        const vec3 b = AI * vec3{ uv[1][1] - uv[1][0], uv[1][2] - uv[1][0], 0 }; // dv

        // 按列组装成变换矩阵：把切线空间向量 (x,y,z) 变换到世界空间
        mat<3, 3> B;
        const double lt = norm(t), lb = norm(b);
        B.set_col(0, lt > 1e-12 ? t / lt : vec3{ 1, 0, 0 });
        B.set_col(1, lb > 1e-12 ? b / lb : vec3{ 0, 1, 0 });
        B.set_col(2, n);
        return B;
    }

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// DepthShader
// ─────────────────────────────────────────────────────────────────────────────
vec4 DepthShader::vertex(const int iface, const int nthvert) {
    return to_clip(model.vert(iface, nthvert));
}

bool DepthShader::fragment(const vec3&, TGAColor& color) {
    // 颜色无意义，写白色只是为了让阴影图本身也能被可视化查看
    color = TGAColor{ 255, 255, 255, 255, 3 };
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FlatShader
// ─────────────────────────────────────────────────────────────────────────────
vec4 FlatShader::vertex(const int iface, const int nthvert) {
    cur_face = iface;
    const vec3 world = model.vert(iface, nthvert);
    varying_world.set_col(nthvert, world);
    varying_uv.set_col(nthvert, model.uv(iface, nthvert));
    return to_clip(world);
}

bool FlatShader::fragment(const vec3& bar, TGAColor& color) {
    // 面法线由三角形自身的两条边叉乘得到 —— 整个面共用一个法线，
    // 这正是平直着色能看出"棱面"的原因。
    const vec3 e1 = varying_world.col(1) - varying_world.col(0);
    const vec3 e2 = varying_world.col(2) - varying_world.col(0);
    vec3 n = cross(e1, e2);
    const double len = norm(n);
    if (len < 1e-12) return false;
    n = n / len;

    const double diff = std::max(0.0, n * light.light_dir);   // 兰伯特余弦项
    const vec2 uv = varying_uv * bar;
    const vec3 albedo = model.diffuse(cur_face, uv);

    color = to_color(albedo * (light.ambient + (1.0 - light.ambient) * diff));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// GouraudShader
// ─────────────────────────────────────────────────────────────────────────────
vec4 GouraudShader::vertex(const int iface, const int nthvert) {
    cur_face = iface;
    // 光照在 **顶点** 上就算完了，片元阶段只剩一次插值
    varying_intensity[nthvert] = std::max(0.0, model.normal(iface, nthvert) * light.light_dir);
    varying_uv.set_col(nthvert, model.uv(iface, nthvert));
    return to_clip(model.vert(iface, nthvert));
}

bool GouraudShader::fragment(const vec3& bar, TGAColor& color) {
    const double diff = varying_intensity * bar;              // 三个顶点光强的加权平均
    const vec2 uv = varying_uv * bar;
    const vec3 albedo = model.diffuse(cur_face, uv);

    color = to_color(albedo * (light.ambient + (1.0 - light.ambient) * diff));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// PhongShader
// ─────────────────────────────────────────────────────────────────────────────
vec4 PhongShader::vertex(const int iface, const int nthvert) {
    cur_face = iface;
    const vec3 world = model.vert(iface, nthvert);
    varying_world .set_col(nthvert, world);
    varying_normal.set_col(nthvert, model.normal(iface, nthvert));
    varying_uv    .set_col(nthvert, model.uv(iface, nthvert));
    return to_clip(world);
}

bool PhongShader::fragment(const vec3& bar, TGAColor& color) {
    const vec2 uv    = varying_uv * bar;
    const vec3 world = varying_world * bar;

    // ── 法线 ──
    // 插值三个顶点的法线。注意插值会让长度缩短（两个单位向量的中点长度 < 1），
    // 所以必须重新归一化，否则光强会莫名其妙地偏暗。
    vec3 n = varying_normal * bar;
    double len = norm(n);
    if (len < 1e-12) return false;
    n = n / len;

    // ── 法线贴图 ──
    // 用贴图记录的高频细节去扰动几何法线，让低面数模型也能有毛孔、皱纹、铆钉。
    // 几何本身没变，变的只是光照计算时用的法线方向。
    if (light.use_normal_map && model.has_normal_map(cur_face)) {
        const vec3 tex_n = model.sample_normal_map(cur_face, uv);
        if (model.normal_map_tangent_space()) {
            // 切线空间：贴图里的向量要先经由 TBN 标架转到世界空间
            const mat<3, 3> TBN = darboux_frame(varying_world, varying_uv, n);
            const vec3 wn = TBN * tex_n;
            const double l2 = norm(wn);
            if (l2 > 1e-12) n = wn / l2;
        } else {
            // 物体空间：贴图里存的已经是世界方向，直接用
            n = tex_n;
        }
    }

    const vec3& l = light.light_dir;                       // 表面 → 光源
    const double n_dot_l = n * l;
    const double diff = std::max(0.0, n_dot_l);            // 漫反射：兰伯特余弦定律

    // ── 高光（Blinn-Phong）──
    // 用"半程向量" h = normalize(l + v) 与法线的夹角来衡量高光强度，
    // 而不是原始 Phong 的"反射向量 r 与视线 v 的夹角"。
    // 两者视觉效果接近，但半程向量少算一次反射，且在掠射角下表现更稳定。
    double spec = 0.0;
    if (light.use_spec && diff > 0.0) {
        const vec3 v = normalized(light.eye - world);      // 表面 → 相机
        const vec3 h = normalized(l + v);
        const double shininess = (light.shininess_override > 0)
                               ? light.shininess_override
                               : model.shininess(cur_face);
        spec = std::pow(std::max(0.0, n * h), shininess)   // 指数越大，高光斑越小越锐
             * model.specular(cur_face, uv);               // 高光贴图逐像素调制强度
    }

    // ── 阴影 ──
    const double vis = shadow_factor(light, world, n_dot_l);

    // ── 合成 ──
    // 环境光不受阴影影响（它代表来自各个方向的间接光），
    // 漫反射和高光则要乘上可见度。
    const vec3 albedo = model.diffuse(cur_face, uv);
    const vec3 ks     = model.spec_color(cur_face);
    const vec3 lit    = albedo * (light.ambient + (1.0 - light.ambient) * diff * vis)
                      + ks * (spec * vis);

    color = to_color({ lit.x * light.light_color.x,
                       lit.y * light.light_color.y,
                       lit.z * light.light_color.z });
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// DebugShader
// ─────────────────────────────────────────────────────────────────────────────
vec4 DebugShader::vertex(const int iface, const int nthvert) {
    cur_face = iface;
    varying_normal.set_col(nthvert, model.normal(iface, nthvert));
    varying_uv    .set_col(nthvert, model.uv(iface, nthvert));
    return to_clip(model.vert(iface, nthvert));
}

bool DebugShader::fragment(const vec3& bar, TGAColor& color) {
    if (kind == Kind::Normal) {
        vec3 n = varying_normal * bar;
        const double len = norm(n);
        if (len < 1e-12) return false;
        n = n / len;
        // 法线分量的范围是 [-1,1]，颜色是 [0,1]，所以做 (n+1)/2 的映射。
        // 于是 +X 偏红、+Y 偏绿、+Z 偏蓝，朝向相机的面呈淡蓝紫色。
        color = to_color((n + vec3{ 1, 1, 1 }) * 0.5);
    } else {
        const vec2 uv = varying_uv * bar;
        // u → 红，v → 绿。贴图坐标越界时会看到颜色回绕，接缝一目了然。
        color = to_color({ uv.x, uv.y, 0.25 });
    }
    return true;
}
