#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cmath>
#include "model.h"

namespace {
    // 将 OBJ 索引字符串（可正可负，1-based）转换为 0-based 数组下标。
    // OBJ 规范：正数从 1 开始，负数表示从末尾倒数；0 为非法值。
    // 若字符串为空、非法或越界则返回 -1。
    int parse_index(const std::string& s, const int count) {
        if (s.empty()) return -1;
        int idx = 0;
        try { idx = std::stoi(s); } catch (...) { return -1; }
        if      (idx > 0) idx--;           // 转为 0-based
        else if (idx < 0) idx = count + idx; // 负数相对索引
        else return -1;                    // 0 在 OBJ 中非法
        return (idx >= 0 && idx < count) ? idx : -1;
    }

    // 解析 OBJ 面顶点 token，格式为 "v[/vt[/vn]]" 或 "v[//vn]"。
    // 将三个属性的 0-based 下标写入 out_v / out_vt / out_vn。
    // 缺失或非法的属性写 -1。
    void parse_face_token(const std::string& token,
                          const int nv, const int nvt, const int nvn,
                          int& out_v, int& out_vt, int& out_vn) {
        const std::size_t s1 = token.find('/');
        const std::size_t s2 = (s1 == std::string::npos) ? std::string::npos
                                                          : token.find('/', s1 + 1);

        // 第一段：顶点坐标索引（斜杠之前）
        out_v  = parse_index(token.substr(0, s1), nv);

        // 第二段：纹理坐标索引（两斜杠之间，可为空如 "v//vn"）
        if (s1 == std::string::npos) {
            out_vt = -1; // 没有斜杠，无纹理坐标
        } else {
            const std::size_t len = (s2 == std::string::npos) ? std::string::npos
                                                               : s2 - s1 - 1;
            out_vt = parse_index(token.substr(s1 + 1, len), nvt);
        }

        // 第三段：法线索引（第二个斜杠之后）
        out_vn = (s2 == std::string::npos) ? -1
                                           : parse_index(token.substr(s2 + 1), nvn);
    }

    // ── 路径小工具 ────────────────────────────────────────────────────────
    // 只做字符串处理，不依赖 <filesystem>，同时兼容 '/' 和 '\' 两种分隔符。

    // 取出目录部分，含末尾分隔符；没有目录时返回空串
    std::string dir_of(const std::string& path) {
        const std::size_t p = path.find_last_of("/\\");
        return (p == std::string::npos) ? std::string{} : path.substr(0, p + 1);
    }

    // 去掉扩展名（只去最后一个点之后的部分，且该点必须在最后一个目录分隔符之后）
    std::string strip_extension(const std::string& path) {
        const std::size_t dot = path.find_last_of('.');
        const std::size_t sep = path.find_last_of("/\\");
        if (dot == std::string::npos) return path;
        if (sep != std::string::npos && dot < sep) return path;
        return path.substr(0, dot);
    }

    // 取文件名（不含目录、不含扩展名），用作模型显示名
    std::string stem_of(const std::string& path) {
        const std::string no_ext = strip_extension(path);
        const std::size_t p = no_ext.find_last_of("/\\");
        return (p == std::string::npos) ? no_ext : no_ext.substr(p + 1);
    }

    bool file_exists(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        return f.good();
    }

    // 把 MTL 里的相对路径拼到 mtl 所在目录下；已经是绝对路径则原样返回
    std::string resolve_relative(const std::string& base_dir, const std::string& path) {
        if (path.empty()) return path;
        const bool absolute = (path[0] == '/' || path[0] == '\\') ||
                              (path.size() > 1 && path[1] == ':');
        return absolute ? path : base_dir + path;
    }

    // 去掉字符串首尾空白（MTL 的材质名常带尾随空格或 Windows 换行残留的 '\r'）
    std::string trim(const std::string& s) {
        std::size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
        return s.substr(a, b - a);
    }

    // map_Kd 这类语句允许带选项，例如 "map_Kd -s 1 1 1 brick.tga"。
    // 简单起见取最后一个 token 作为文件名（选项都排在前面）。
    std::string last_token(const std::string& s) {
        std::istringstream iss(s);
        std::string tok, last;
        while (iss >> tok) last = tok;
        return last;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MTL 材质库解析
// ─────────────────────────────────────────────────────────────────────────────
void Model::load_material_lib(const std::string& mtl_path, const std::string& base_dir) {
    std::ifstream in(mtl_path);
    if (in.fail()) {
        std::cerr << "  [警告] 打不开材质库 " << mtl_path << "，将使用默认材质" << std::endl;
        return;
    }

    Material* cur = nullptr;  // 指向当前正在填充的材质
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        std::istringstream iss(t);
        std::string key;
        iss >> key;

        if (key == "newmtl") {
            // 材质名可能含空格甚至中文，所以取 key 之后的整行而不是用 >> 读单词
            Material m;
            m.name = trim(t.substr(key.size()));
            materials.push_back(std::move(m));
            cur = &materials.back();
        } else if (!cur) {
            continue; // newmtl 之前出现的内容一律忽略
        } else if (key == "Kd") {
            iss >> cur->Kd.x >> cur->Kd.y >> cur->Kd.z;
        } else if (key == "Ks") {
            iss >> cur->Ks.x >> cur->Ks.y >> cur->Ks.z;
        } else if (key == "Ns") {
            iss >> cur->Ns;
        } else if (key == "map_Kd") {
            cur->map_Kd.load(resolve_relative(base_dir, last_token(t.substr(key.size()))));
        } else if (key == "map_Ks") {
            cur->map_Ks.load(resolve_relative(base_dir, last_token(t.substr(key.size()))));
        } else if (key == "map_Bump" || key == "map_bump" || key == "bump" || key == "norm") {
            cur->map_bump.load(resolve_relative(base_dir, last_token(t.substr(key.size()))));
        }
        // 其余字段（Ka / Ke / Ni / d / illum）本渲染器暂不使用
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 按命名约定寻找模型级贴图
//
// tinyrenderer 素材集（african_head / diablo3_pose）不带 MTL，而是把贴图放在
// 模型同目录、用固定后缀命名。这里按约定探测，找不到就安静跳过。
// ─────────────────────────────────────────────────────────────────────────────
void Model::discover_textures_by_convention(const std::string& obj_path) {
    const std::string base = strip_extension(obj_path);

    if (file_exists(base + "_diffuse.tga"))
        map_diffuse.load(base + "_diffuse.tga");

    // 优先切线空间法线贴图（_nm_tangent）：它记录的是相对于表面局部标架的扰动，
    // 与模型如何旋转、缩放无关，因此可以复用到形变后的模型上。
    // 退而求其次才用物体空间法线贴图（_nm），后者存的直接就是物体空间方向。
    if (file_exists(base + "_nm_tangent.tga")) {
        map_normal.load(base + "_nm_tangent.tga");
        normal_map_is_tangent = true;
    } else if (file_exists(base + "_nm.tga")) {
        map_normal.load(base + "_nm.tga");
        normal_map_is_tangent = false;
    }

    if (file_exists(base + "_spec.tga"))
        map_spec.load(base + "_spec.tga");
}

// ─────────────────────────────────────────────────────────────────────────────
// 缺失法线时生成平滑顶点法线
//
// 做法：把每个三角形的面法线累加到它的三个顶点上，最后归一化。
// 这里刻意 **不** 对叉乘结果做归一化——叉乘的模长恰好等于三角形面积的两倍，
// 于是大三角形自然获得更大权重，得到的是"面积加权平均法线"，
// 比等权平均更贴近真实曲面。
// ─────────────────────────────────────────────────────────────────────────────
void Model::generate_smooth_normals() {
    normals.assign(verts.size(), vec3{ 0, 0, 0 });

    for (int f = 0; f < nfaces(); f++) {
        const int ia = face_vrt[f * 3 + 0];
        const int ib = face_vrt[f * 3 + 1];
        const int ic = face_vrt[f * 3 + 2];
        const vec3 n = cross(verts[ib] - verts[ia], verts[ic] - verts[ia]); // 模长 = 2×面积
        normals[ia] = normals[ia] + n;
        normals[ib] = normals[ib] + n;
        normals[ic] = normals[ic] + n;
    }

    for (vec3& n : normals) {
        const double len = norm(n);
        n = (len > 1e-12) ? n / len : vec3{ 0, 0, 1 }; // 孤立顶点给个任意方向，避免 NaN
    }

    // 顶点法线与顶点一一对应，所以法线索引可以直接复用顶点索引
    face_nrm = face_vrt;
}

// ─────────────────────────────────────────────────────────────────────────────
// 构造函数：解析 OBJ
// ─────────────────────────────────────────────────────────────────────────────
Model::Model(const std::string& filename, const bool force_checker) {
    model_name = stem_of(filename);

    std::ifstream in;
    in.open(filename, std::ifstream::in);
    if (in.fail()) {
        std::cerr << "[错误] 打不开模型文件: " << filename << std::endl;
        return;
    }

    const std::string base_dir = dir_of(filename);
    int current_material = -1;   // usemtl 设定的"当前材质"，之后解析出的面都归它
    bool any_face_has_normal = false;

    std::string line;
    while (!in.eof()) {
        std::getline(in, line);
        std::istringstream iss(line.c_str());
        char trash;

        if (!line.compare(0, 2, "v ")) {
            // ── 顶点坐标：读取 x y z，忽略可选的第四分量 w ──
            iss >> trash;
            vec3 v;
            for (int i : {0, 1, 2}) iss >> v[i];
            verts.push_back(v);

        } else if (!line.compare(0, 3, "vt ")) {
            // ── 纹理坐标：读取 u v，忽略可选的第三分量 w ──
            iss >> trash >> trash; // 跳过 "vt"
            vec2 uv;
            for (int i : {0, 1}) iss >> uv[i];
            tex_coords.push_back(uv);

        } else if (!line.compare(0, 3, "vn ")) {
            // ── 顶点法线：读取 x y z；OBJ 不保证法线已归一化 ──
            iss >> trash >> trash; // 跳过 "vn"
            vec3 n;
            for (int i : {0, 1, 2}) iss >> n[i];
            normals.push_back(n);

        } else if (!line.compare(0, 2, "f ")) {
            // ── 面：每个 token 格式为 v[/vt[/vn]] ──
            // 三个属性索引分别存入并行数组，缺失时填 -1
            iss >> trash;
            std::string token;
            std::vector<int> fv, ft, fn;
            while (iss >> token) {
                if (token[0] == '#') break; // 行内注释
                int iv, ivt, ivn;
                parse_face_token(token, nverts(), ntexcoords(), nnormals(),
                                 iv, ivt, ivn);
                if (iv == -1) {
                    std::cerr << "[错误] 非法的 obj 面索引: " << token << std::endl;
                    return;
                }
                fv.push_back(iv);
                ft.push_back(ivt);
                fn.push_back(ivn);
            }
            if (fv.size() < 3) {
                std::cerr << "[警告] 跳过顶点数不足 3 的面" << std::endl;
                continue;
            }
            // ── 扇形三角化 ──
            // OBJ 允许任意多边形面（四边形尤其常见），而光栅化只认三角形。
            // 这里以第 0 个顶点为轴心拆成 (v0, vi, vi+1) 的三角形扇。
            // 对凸多边形完全正确；凹多边形会有轻微偏差，但导出器基本都输出凸面。
            for (std::size_t i = 1; i + 1 < fv.size(); i++) {
                const std::size_t idx[3] = { 0, i, i + 1 };
                for (const std::size_t k : idx) {
                    face_vrt.push_back(fv[k]);
                    face_tex.push_back(ft[k]);
                    face_nrm.push_back(fn[k]);
                    if (fn[k] >= 0) any_face_has_normal = true;
                }
                face_mtl.push_back(current_material);
            }

        } else if (!line.compare(0, 2, "l ")) {
            // ── 折线：将多段折线拆分为若干条独立线段 ──
            // "l" 的 token 同样可能带有 /vt//vn 后缀，只取顶点坐标索引
            iss >> trash;
            std::string token;
            std::vector<int> polyline;
            while (iss >> token) {
                if (token[0] == '#') break;
                int iv, ivt, ivn;
                parse_face_token(token, nverts(), ntexcoords(), nnormals(),
                                 iv, ivt, ivn);
                if (iv == -1) {
                    std::cerr << "[错误] 非法的 obj 线段索引: " << token << std::endl;
                    return;
                }
                polyline.push_back(iv);
            }
            // 将折线转换为相邻端点对
            for (int i = 1; i < (int)polyline.size(); i++) {
                line_vrt.push_back(polyline[i - 1]);
                line_vrt.push_back(polyline[i]);
            }

        } else if (!line.compare(0, 7, "mtllib ")) {
            // ── 材质库：路径相对于 obj 文件所在目录 ──
            const std::string mtl = trim(line.substr(7));
            if (mtl.empty()) continue;
            const std::string mtl_path = resolve_relative(base_dir, mtl);
            if (file_exists(mtl_path)) {
                load_material_lib(mtl_path, base_dir);
            } else {
                // 常见情况：导出后 obj 被改名，而 mtllib 里仍写着旧名字
                //（或者反过来）。这时按"同名 .mtl"再试一次，成功率相当高。
                const std::string guess = strip_extension(filename) + ".mtl";
                if (file_exists(guess)) {
                    std::cerr << "  [提示] 找不到 " << mtl << "，改用同名材质库 "
                              << guess << std::endl;
                    load_material_lib(guess, base_dir);
                } else {
                    std::cerr << "  [警告] 找不到材质库 " << mtl << "，将使用默认材质" << std::endl;
                }
            }

        } else if (!line.compare(0, 7, "usemtl ")) {
            // ── 切换当前材质：之后解析出的面都归属于它 ──
            const std::string want = trim(line.substr(7));
            current_material = -1;
            for (int i = 0; i < (int)materials.size(); i++)
                if (materials[i].name == want) { current_material = i; break; }
            if (current_material == -1 && !materials.empty())
                std::cerr << "  [警告] 材质库中找不到材质: " << want << std::endl;
        }
    }

    // ── 补齐缺失数据 ──────────────────────────────────────────────────────
    if (!any_face_has_normal && !verts.empty() && nfaces() > 0) {
        // 模型完全没提供法线，自己算一套平滑法线，否则光照无从谈起
        generate_smooth_normals();
    }

    discover_textures_by_convention(filename);

    // ── 漫反射兜底 ────────────────────────────────────────────────────────
    // 判断模型到底有没有提供"基色信息"：贴图算，非白色的 Kd 也算。
    // 很多从建模软件直接导出的模型两样都没有（所有材质的 Kd 都是纯白），
    // 这种模型渲染出来会是一片惨白，只剩轮廓；棋盘格能让表面起伏和 uv 走向
    // 一目了然，代价只是颜色不真实——而它本来也没有真实颜色可言。
    bool has_color_info = map_diffuse.valid();
    for (const Material& m : materials)
        if (m.map_Kd.valid() || norm(m.Kd - vec3{ 1, 1, 1 }) > 1e-3) {
            has_color_info = true;
            break;
        }
    // 棋盘格需要纹理坐标才能贴上去，没有 vt 的模型只能维持原样
    if (force_checker || (!has_color_info && ntexcoords() > 0))
        map_diffuse.make_checker(512, 16, vec3{ 0.85, 0.85, 0.85 }, vec3{ 0.30, 0.35, 0.45 });

    ok = (nfaces() > 0 || nlines() > 0);

    // ── 加载摘要 ──────────────────────────────────────────────────────────
    std::cerr << "  " << model_name
              << ": v=" << nverts()
              << " vt=" << ntexcoords()
              << " vn=" << nnormals()
              << " f="  << nfaces()
              << " l="  << nlines()
              << " mtl=" << nmaterials()
              << " [漫反射:" << (map_diffuse.valid() ? "贴图" : (materials.empty() ? "默认色" : "材质色"))
              << " 法线:"   << (map_normal.valid() ? (normal_map_is_tangent ? "切线空间" : "物体空间") : "无")
              << " 高光:"   << (map_spec.valid() ? "贴图" : "无")
              << "]" << std::endl;
}

// ─────────────────────────────────────────────────────────────────────────────
// 程序化地板
//
// 一块朝上（法线 +Y）的正方形，由两个三角形拼成。它有两个作用：
//   1. 承接阴影 —— 没有接受体的话，阴影贴图算得再准也看不见；
//   2. 提供空间参照 —— 棋盘格向远处的收缩能直观体现"这是透视投影不是正交投影"。
// ─────────────────────────────────────────────────────────────────────────────
Model Model::make_floor(const vec3 center, const double half_size, const int checker_squares) {
    Model m;
    m.model_name = "floor";

    // 四个角，从上往下看呈逆时针排列，使叉乘得到的面法线朝上（+Y）
    m.verts = {
        { center.x - half_size, center.y, center.z + half_size },
        { center.x + half_size, center.y, center.z + half_size },
        { center.x + half_size, center.y, center.z - half_size },
        { center.x - half_size, center.y, center.z - half_size },
    };
    m.tex_coords = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };
    m.normals    = { {0, 1, 0} };

    // 两个三角形：(0,1,2) 和 (0,2,3)
    const int tri[2][3] = { {0, 1, 2}, {0, 2, 3} };
    for (const auto& t : tri) {
        for (const int k : t) {
            m.face_vrt.push_back(k);
            m.face_tex.push_back(k);
            m.face_nrm.push_back(0);
        }
        m.face_mtl.push_back(-1);
    }

    // 地板用低饱和度的冷灰棋盘格，避免抢模型的视觉重点
    m.map_diffuse.make_checker(512, checker_squares,
                               vec3{ 0.62, 0.63, 0.66 }, vec3{ 0.38, 0.40, 0.44 });
    m.ok = true;
    return m;
}

// ─────────────────────────────────────────────────────────────────────────────
// 数量查询
// ─────────────────────────────────────────────────────────────────────────────
int Model::nverts()     const { return (int)verts.size(); }
int Model::ntexcoords() const { return (int)tex_coords.size(); }
int Model::nnormals()   const { return (int)normals.size(); }
int Model::nfaces()     const { return (int)face_vrt.size() / 3; }
int Model::nlines()     const { return (int)line_vrt.size() / 2; }
int Model::nmaterials() const { return (int)materials.size(); }

// ─────────────────────────────────────────────────────────────────────────────
// 顶点属性访问
// ─────────────────────────────────────────────────────────────────────────────

// 按全局下标获取顶点坐标
vec3 Model::vert(const int i) const {
    return verts[i];
}

// 按面序号 + 面内顶点序号获取顶点坐标
vec3 Model::vert(const int iface, const int nthvert) const {
    return verts[face_vrt[iface * 3 + nthvert]];
}

// 按面序号 + 面内顶点序号获取纹理坐标；若该顶点无纹理坐标则返回零向量
vec2 Model::uv(const int iface, const int nthvert) const {
    const int idx = face_tex[iface * 3 + nthvert];
    return (idx >= 0) ? tex_coords[idx] : vec2{};
}

// 三角形的几何面法线：两条边叉乘再归一化。
// 顶点绕序（OBJ 约定为逆时针）决定它朝向物体外侧还是内侧。
vec3 Model::face_normal(const int iface) const {
    const vec3 a = vert(iface, 0), b = vert(iface, 1), c = vert(iface, 2);
    const vec3 n = cross(b - a, c - a);
    const double len = norm(n);
    return (len > 1e-12) ? n / len : vec3{ 0, 0, 1 };
}

// 按面序号 + 面内顶点序号获取顶点法线。
// 该顶点没有法线数据时回落到面法线——这样至少能得到平直着色的效果，
// 而不是返回零向量导致光照全黑。
vec3 Model::normal(const int iface, const int nthvert) const {
    const int idx = face_nrm[iface * 3 + nthvert];
    if (idx < 0) return face_normal(iface);
    const vec3 n = normals[idx];
    const double len = norm(n);
    return (len > 1e-12) ? n / len : face_normal(iface);
}

// 按线段序号 + 端点序号（0/1）获取顶点坐标
vec3 Model::linevert(const int iline, const int nthvert) const {
    return verts[line_vrt[iline * 2 + nthvert]];
}

// ─────────────────────────────────────────────────────────────────────────────
// 材质与贴图查询
//
// 统一的回落顺序：材质自带贴图 → 模型级约定贴图 → 材质常量 → 内置默认值。
// 把这套优先级收敛在这里，着色器就只管"要颜色"，不必知道贴图究竟从哪来。
// ─────────────────────────────────────────────────────────────────────────────
vec3 Model::diffuse(const int iface, const vec2& uv) const {
    const int mi = face_mtl.empty() ? -1 : face_mtl[iface];
    if (mi >= 0) {
        const Material& m = materials[mi];
        if (m.map_Kd.valid())    return m.map_Kd.sample(uv);
        if (map_diffuse.valid()) return map_diffuse.sample(uv);
        return m.Kd;                        // 纯 MTL 模型走这条路径：用材质基色
    }
    if (map_diffuse.valid()) return map_diffuse.sample(uv);
    return { 0.75, 0.75, 0.75 };
}

double Model::specular(const int iface, const vec2& uv) const {
    const int mi = face_mtl.empty() ? -1 : face_mtl[iface];
    if (mi >= 0 && materials[mi].map_Ks.valid()) return materials[mi].map_Ks.sample_gray(uv);
    if (map_spec.valid()) return map_spec.sample_gray(uv);
    return 1.0; // 无高光贴图时不做逐像素调制，强度完全交给 Ks / Ns 控制
}

vec3 Model::spec_color(const int iface) const {
    const int mi = face_mtl.empty() ? -1 : face_mtl[iface];
    return (mi >= 0) ? materials[mi].Ks : vec3{ 0.30, 0.30, 0.30 };
}

double Model::shininess(const int iface) const {
    const int mi = face_mtl.empty() ? -1 : face_mtl[iface];
    return (mi >= 0) ? std::max(1.0, materials[mi].Ns) : 32.0;
}

bool Model::has_normal_map(const int iface) const {
    const int mi = face_mtl.empty() ? -1 : face_mtl[iface];
    if (mi >= 0 && materials[mi].map_bump.valid()) return true;
    return map_normal.valid();
}

vec3 Model::sample_normal_map(const int iface, const vec2& uv) const {
    const int mi = face_mtl.empty() ? -1 : face_mtl[iface];
    if (mi >= 0 && materials[mi].map_bump.valid()) return materials[mi].map_bump.sample_normal(uv);
    return map_normal.sample_normal(uv);
}

// ─────────────────────────────────────────────────────────────────────────────
// 空间变换
// ─────────────────────────────────────────────────────────────────────────────
void Model::bbox(vec3& vmin, vec3& vmax) const {
    if (verts.empty()) { vmin = vmax = vec3{}; return; }
    vmin = vmax = verts[0];
    for (const vec3& v : verts)
        for (const int i : {0, 1, 2}) {
            if (v[i] < vmin[i]) vmin[i] = v[i];
            if (v[i] > vmax[i]) vmax[i] = v[i];
        }
}

void Model::transform(const mat<4, 4>& M) {
    // 顶点是"点"，取 w=1，因此会参与平移
    for (vec3& v : verts)
        v = proj<3>(M * embed<4>(v, 1.0));

    // 法线不能直接乘 M！
    // 反例：把球体沿 x 方向压扁，表面变陡，法线应当朝 x 方向"张开"；
    // 但直接乘 M 会把法线也一起压扁，方向恰好错反。
    // 正确做法是乘 M 的逆转置矩阵，它保证变换后的法线仍垂直于变换后的切平面。
    // （只有当 M 是正交变换（纯旋转）时，两者才恰好相等。）
    const mat<4, 4> N = M.invert_transpose();
    for (vec3& n : normals) {
        // 取 w=0：法线是方向而非位置，不应受平移影响
        const vec3 t = proj<3>(N * embed<4>(n, 0.0));
        const double len = norm(t);
        if (len > 1e-12) n = t / len;
    }
}
