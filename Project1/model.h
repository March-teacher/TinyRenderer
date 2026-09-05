#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// model —— 几何与材质数据源
//
// 管线位置：最前端。负责把磁盘上的 OBJ/MTL 变成渲染器能索引的顶点属性数组，
// 并把与之配套的贴图准备好。顶点着色器通过 (面序号, 面内顶点序号) 这一对下标
// 向它取数据，片元着色器通过 (面序号, uv) 向它取贴图颜色。
//
// 坐标空间：这里存放的一律是 **模型空间**（obj 文件里的原始坐标）。
//           场景组装时会调用 transform() 把它们烘焙到 **世界空间**。
// ─────────────────────────────────────────────────────────────────────────────
#include <vector>
#include <string>
#include "geometry.h"
#include "texture.h"

// OBJ 的伴生材质（MTL）。一个模型可以有多份材质，面通过下标引用它们。
struct Material {
    std::string name;                   // newmtl 名字，仅用于查找与调试输出
    vec3   Kd = { 0.75, 0.75, 0.75 };   // 漫反射基色，贴图缺失时的回落颜色
    vec3   Ks = { 0.30, 0.30, 0.30 };   // 高光颜色
    double Ns = 32.0;                   // 高光指数（越大高光越锐）
    Texture map_Kd;                     // 漫反射贴图
    Texture map_Ks;                     // 高光贴图
    Texture map_bump;                   // 法线/凹凸贴图（按切线空间解释）
};

class Model {
    // ── 几何数据 ──────────────────────────────────────────────────────────
    std::vector<vec3> verts      = {}; // OBJ "v"  : 顶点坐标 (x, y, z)
    std::vector<vec2> tex_coords = {}; // OBJ "vt" : 纹理坐标 (u, v)，第三分量 w 被忽略
    std::vector<vec3> normals    = {}; // OBJ "vn" : 顶点法线 (x, y, z)，未做归一化处理

    // ── 面索引（三个数组并行，每3个元素对应一个三角形的三个顶点）──────────
    // OBJ "f" : 每个元素是对应属性数组的下标，-1 表示该顶点没有此属性
    std::vector<int> face_vrt = {}; // 顶点坐标索引
    std::vector<int> face_tex = {}; // 纹理坐标索引，-1 表示不存在
    std::vector<int> face_nrm = {}; // 法线索引，-1 表示不存在
    std::vector<int> face_mtl = {}; // 每个三角形所属材质下标，-1 表示无材质

    // ── 折线段索引（每2个元素对应一条线段的两个端点）────────────────────
    std::vector<int> line_vrt = {}; // OBJ "l" : 顶点坐标索引

    // ── 材质与模型级贴图 ──────────────────────────────────────────────────
    std::vector<Material> materials = {};

    // 很多经典模型（tinyrenderer 素材集就是如此）不写 MTL，而是把贴图按
    // "<模型名>_diffuse.tga" 这样的后缀约定放在同目录。这三张是按约定找到的
    // 模型级贴图，优先级低于材质自带的 map_*。
    Texture map_diffuse;
    Texture map_normal;
    Texture map_spec;
    bool    normal_map_is_tangent = true; // "_nm_tangent" 为切线空间，"_nm" 为世界/物体空间

    std::string model_name;   // 便于日志区分多模型场景中的各个部件
    bool        ok = false;   // 是否成功加载到有效几何

    // 供构造函数使用的内部步骤
    void load_material_lib(const std::string& mtl_path, const std::string& base_dir);
    void discover_textures_by_convention(const std::string& obj_path);
    void generate_smooth_normals();

public:
    Model() = default;                                   // 空模型，供程序化构造使用
    explicit Model(const std::string& filename,
                   bool force_checker = false);          // force_checker: 强制使用棋盘格漫反射

    // 程序化生成一块朝上的矩形地板（用来接住阴影，并给场景一个空间参照）。
    // center 给出地板中心，half_size 是半边长，纹理坐标铺满 [0,1]²。
    static Model make_floor(vec3 center, double half_size, int checker_squares);

    bool valid() const { return ok; }
    const std::string& name() const { return model_name; }

    // 数量查询
    int nverts()     const; // 顶点坐标数量
    int ntexcoords() const; // 纹理坐标数量
    int nnormals()   const; // 法线数量
    int nfaces()     const; // 三角形面数量
    int nlines()     const; // 线段数量
    int nmaterials() const; // 材质数量

    // 按全局下标访问单个顶点坐标
    vec3 vert(const int i) const;

    // 按面序号 + 面内顶点序号（0/1/2）访问属性
    vec3 vert  (const int iface, const int nthvert) const;
    vec2 uv    (const int iface, const int nthvert) const;
    // 顶点法线；若该顶点没有法线数据，回落到该三角形的几何面法线
    vec3 normal(const int iface, const int nthvert) const;
    // 三角形的几何面法线（由三个顶点叉乘得到，已归一化）
    vec3 face_normal(const int iface) const;

    // 按线段序号 + 端点序号（0/1）访问顶点坐标
    vec3 linevert(const int iline, const int nthvert) const;

    // ── 材质与贴图查询 ────────────────────────────────────────────────────
    // 下面几个函数内部统一按 "材质贴图 → 模型级贴图 → 材质常量" 的优先级回落，
    // 着色器因此不必关心贴图到底从哪来、存不存在。
    vec3   diffuse  (const int iface, const vec2& uv) const; // 漫反射基色，[0,1] RGB
    double specular (const int iface, const vec2& uv) const; // 高光强度，[0,1]
    vec3   spec_color(const int iface) const;                // 高光颜色 Ks
    double shininess (const int iface) const;                // 高光指数 Ns

    bool has_normal_map (const int iface) const;             // 是否存在可用的法线贴图
    bool normal_map_tangent_space() const { return normal_map_is_tangent; }
    vec3 sample_normal_map(const int iface, const vec2& uv) const;

    // ── 空间变换 ──────────────────────────────────────────────────────────
    // 计算模型的轴对齐包围盒（AABB）。模型为空时返回全零。
    void bbox(vec3& vmin, vec3& vmax) const;
    // 用 4x4 矩阵变换整个模型（顶点按点变换，法线按逆转置矩阵变换）。
    void transform(const mat<4, 4>& M);
};
