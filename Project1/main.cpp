// ─────────────────────────────────────────────────────────────────────────────
// main —— 命令行入口与线框渲染
//
// 这个文件保留了整个项目最早期的那部分代码：从"用参数方程画线"一路优化到
// "全整数 Bresenham"的四个版本。它们不是死代码，线框模式仍然在用最终版的
// line()，前三个版本作为推导过程留在这里。
//
// 除此之外，本文件只负责两件事：解析命令行参数，以及把工作分派给 Scene。
// 真正的渲染逻辑在 our_gl.cpp（光栅化）、shaders.cpp（着色）、scene.cpp（流程）。
// ─────────────────────────────────────────────────────────────────────────────
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#ifdef _WIN32
#include <windows.h>   // 仅用于把控制台输出代码页切到 UTF-8
#endif

#include "tgaimage.h"
#include "geometry.h"
#include "model.h"
#include "our_gl.h"
#include "shaders.h"
#include "scene.h"
#include "image_io.h"

constexpr TGAColor white = { 255, 255, 255, 255 }; // attention, BGRA order
constexpr TGAColor green = { 0, 255,   0, 255 };
constexpr TGAColor red = { 0,   0, 255, 255 };
constexpr TGAColor blue = { 255, 128,  64, 255 };
constexpr TGAColor yellow = { 0, 200, 255, 255 };

void line1(int ax, int ay, int bx, int by, TGAImage& framebuffer, TGAColor color) {
    // 直接用参数方程方法画 两个问题 参数细致度须于像素点数匹配 点的传入顺序会影响着色顺序（造成覆盖问题）
	for (float t = 0; t < 1; t+=.02) {
		int x = std::round(ax + (bx - ax) * t);
		int y = std::round(ay + (by - ay) * t);
		framebuffer.set(x, y, color);
	}
}

void line2(int ax, int ay, int bx, int by, TGAImage& framebuffer, TGAColor color) {
	// 以x为参数进行优化 像素数量会随着长度动态调整
	// 需要修复 只有按x值从小到大传的线段才能绘制且对于x变化率低y变化率高（斜率大）的线段会出现断点
	for (int x = ax; x <= bx; x++) {
		float t = (x - ax) / static_cast<float>(bx - ax);
		int y = std::round(ay + (by - ay) * t);
		framebuffer.set(x, y, color);
	}
}

void line3(int ax, int ay, int bx, int by, TGAImage& framebuffer, TGAColor color) {
	// 针对line2的缺陷进行修复
	// 通过交换x,y值保证采样轴x变化率大于y变化率（避免断点）
	bool steep = std::abs(ax - bx) < std::abs(ay - by);
	if (steep) {
		std::swap(ax, ay);
		std::swap(bx, by);
	}

	// 通过交换点位置保证从左到右绘制
	if (ax > bx) {
		std::swap(ax, bx);
		std::swap(ay, by);
	}
	for (int x = ax; x <= bx; x++) {
		float t = (x - ax) / static_cast<float>(bx - ax);
		int y = std::round(ay + (by - ay) * t);
		if (steep) framebuffer.set(y, x, color);
		else framebuffer.set(x, y, color);
	}
}

// 最终优化的线条绘制 全整数光栅化绘制（计算机内的浮点数计算要更复杂）将line2的两个浮点数计算合并（消除参数t）并转化为整数计算
void line(int ax, int ay, int bx, int by, TGAImage& framebuffer, TGAColor color) {
	bool steep = std::abs(ax - bx) < std::abs(ay - by);
	if (steep) {
		std::swap(ax, ay);
		std::swap(bx, by);
	}
	if (ax > bx) {
		std::swap(ax, bx);
		std::swap(ay, by);
	}
	int y = ay;
	int ierror = 0;
	for (int x = ax; x <= bx; x++) {
		if (steep) // if transposed, de−transpose
			framebuffer.set(y, x, color);
		else
			framebuffer.set(x, y, color);
		ierror += 2 * std::abs(by - ay);
		y += (by > ay ? 1 : -1) * (ierror > bx - ax);
		ierror -= 2 * (bx - ax) * (ierror > bx - ax);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// 顶点投影
//
// 项目最初这里是一个写死的正交投影：直接把 [-1,1] 的 x,y 线性拉到屏幕上，
// 相机既不能移动也不会有近大远小。现在改成走和实体渲染完全相同的变换链
//（ModelView → Projection → 透视除法 → Viewport），线框因此也能自由转视角。
//
// 返回 false 表示该顶点落在近平面之外（w <= 0，即跑到相机背后了），
// 此时它的投影坐标没有意义，调用方应当直接丢弃相关线段。
// ─────────────────────────────────────────────────────────────────────────────
static bool project(const vec3& world, int& out_x, int& out_y) {
    const vec4 clip = ProjectionMatrix * (ModelView * embed<4>(world, 1.0));
    if (clip.w <= 1e-6) return false;                  // 在相机平面上或背后
    const vec3 screen = proj<3>(ViewportMatrix * (clip / clip.w));
    out_x = static_cast<int>(std::lround(screen.x));
    out_y = static_cast<int>(std::lround(screen.y));
    return true;
}

// 线框渲染：把每个三角形的三条边和 OBJ 里的 "l" 线段都画出来。
//
// 线框没有深度测试，所有边一视同仁地叠在一起，因此背面的边也会透过来——
// 这恰恰是它作为"检查拓扑"工具的价值所在，也是对比之下最能说明
// z-buffer 到底解决了什么问题的一张图。
static TGAImage render_wireframe(const Scene& scene, const RenderOptions& opt) {
    const int ss = (opt.ssaa < 1 ? 1 : (opt.ssaa > 4 ? 4 : opt.ssaa));
    const int w = opt.width * ss, h = opt.height * ss;

    scene.setup_camera(opt, w, h);

    TGAImage framebuffer(w, h, TGAImage::RGB);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            framebuffer.set(x, y, opt.background);

    for (const Model& model : scene.models()) {
        for (int i = 0; i < model.nfaces(); i++) {
            int x[3], y[3];
            bool ok = true;
            for (int v = 0; v < 3; v++)
                ok = project(model.vert(i, v), x[v], y[v]) && ok;
            if (!ok) continue;                          // 有顶点在相机背后，整个三角形跳过
            line(x[0], y[0], x[1], y[1], framebuffer, red);
            line(x[1], y[1], x[2], y[2], framebuffer, red);
            line(x[2], y[2], x[0], y[0], framebuffer, red);
        }

        for (int i = 0; i < model.nlines(); i++) {
            int x[2], y[2];
            bool ok = true;
            for (int v = 0; v < 2; v++)
                ok = project(model.linevert(i, v), x[v], y[v]) && ok;
            if (ok) line(x[0], y[0], x[1], y[1], framebuffer, yellow);
        }
    }

    return downsample(framebuffer, ss);
}

// ─────────────────────────────────────────────────────────────────────────────
// 命令行解析
// ─────────────────────────────────────────────────────────────────────────────
namespace {

    // 用固定的程序名而不是 argv[0]：argv[0] 是操作系统按本地代码页给的字节串，
    // 而我们已经把控制台切到 UTF-8，直接打印含中文路径的 argv[0] 会显示成乱码。
    void print_usage() {
        const char* exe = "renderer";
        std::cout <<
            "TinyRenderer —— 从零实现的软件光栅化渲染器\n"
            "\n"
            "用法:\n"
            "  " << exe << " [选项] <模型.obj> [更多模型.obj ...]\n"
            "\n"
            "输出:\n"
            "  -o, --output <文件>     输出图片，按后缀选择 .png 或 .tga（默认 output.png）\n"
            "  -w, --width  <像素>     图像宽度（默认 800）\n"
            "  -H, --height <像素>     图像高度（默认 800）\n"
            "      --ssaa <1..4>       超采样抗锯齿倍率（默认 2，设为 1 即关闭）\n"
            "      --dump-depth <文件> 额外输出一张深度缓冲可视化图\n"
            "\n"
            "着色:\n"
            "  -s, --shader <模式>     wire | flat | gouraud | phong | normal | uv | depth\n"
            "                          （默认 phong）\n"
            "      --no-normal-map     关闭法线贴图（用于对比法线贴图带来的细节）\n"
            "      --no-spec           关闭高光\n"
            "      --shininess <值>    覆盖材质的高光指数 Ns\n"
            "      --checker           强制使用程序化棋盘格作为漫反射贴图\n"
            "\n"
            "相机:\n"
            "      --eye    x,y,z      相机位置（默认 1,0.55,3.2）\n"
            "      --center x,y,z      观察目标（默认 0,0,0）\n"
            "      --up     x,y,z      上方向（默认 0,1,0）\n"
            "      --fov <角度>        垂直视场角（默认 40）\n"
            "\n"
            "光照与阴影:\n"
            "      --light x,y,z       平行光方向，由表面指向光源（默认 1,1,1）\n"
            "      --ambient <0..1>    环境光强度（默认 0.18）\n"
            "      --no-shadow         关闭阴影贴图\n"
            "\n"
            "场景:\n"
            "      --floor             添加一块程序化棋盘格地板（便于展示阴影）\n"
            "      --no-cull           关闭背面剔除\n"
            "      --no-fit            不把场景归一化到单位立方体\n"
            "\n"
            "示例:\n"
            "  " << exe << " obj/african_head/african_head.obj --floor -o head.png\n"
            "  " << exe << " obj/african_head/african_head.obj \\\n"
            "      obj/african_head/african_head_eye_inner.obj \\\n"
            "      obj/african_head/african_head_eye_outer.obj --floor -o head_full.png\n"
            "  " << exe << " obj/diablo3_pose/diablo3_pose.obj -s wire -o wire.png\n"
            << std::endl;
    }

    // 解析 "x,y,z" 形式的向量；也接受用空格分隔。失败时返回 false 并保持 out 不变。
    bool parse_vec3(const std::string& s, vec3& out) {
        std::string tmp = s;
        for (char& c : tmp) if (c == ',' || c == ';') c = ' ';
        std::istringstream iss(tmp);
        vec3 v;
        if (!(iss >> v.x >> v.y >> v.z)) return false;
        out = v;
        return true;
    }

    bool parse_shader_mode(const std::string& s, ShaderMode& out) {
        if (s == "wire")    { out = ShaderMode::Wire;    return true; }
        if (s == "flat")    { out = ShaderMode::Flat;    return true; }
        if (s == "gouraud") { out = ShaderMode::Gouraud; return true; }
        if (s == "phong")   { out = ShaderMode::Phong;   return true; }
        if (s == "normal")  { out = ShaderMode::Normal;  return true; }
        if (s == "uv")      { out = ShaderMode::UV;      return true; }
        if (s == "depth")   { out = ShaderMode::Depth;   return true; }
        return false;
    }

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // 源码和字符串常量都是 UTF-8（编译时带 /utf-8），而 Windows 控制台默认是
    // 本地代码页（简体中文环境下是 GBK），不改的话所有中文提示都会变成乱码。
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (argc < 2) {
        print_usage();
        return 1;
    }

    RenderOptions opt;
    std::string output = "output.png";
    std::string depth_output;
    std::vector<std::string> model_paths;
    bool force_checker = false;

    // 取下一个参数作为选项的值；缺失时报错退出
    auto need_value = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) {
            std::cerr << "[错误] 选项 " << flag << " 缺少参数" << std::endl;
            std::exit(1);
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];

        if (a == "-h" || a == "--help") { print_usage(); return 0; }
        else if (a == "-o" || a == "--output")   output = need_value(i, "--output");
        else if (a == "-w" || a == "--width")    opt.width  = std::atoi(need_value(i, "--width"));
        else if (a == "-H" || a == "--height")   opt.height = std::atoi(need_value(i, "--height"));
        else if (a == "--ssaa")                  opt.ssaa   = std::atoi(need_value(i, "--ssaa"));
        else if (a == "--dump-depth")            depth_output = need_value(i, "--dump-depth");
        else if (a == "-s" || a == "--shader") {
            const std::string v = need_value(i, "--shader");
            if (!parse_shader_mode(v, opt.mode)) {
                std::cerr << "[错误] 未知的着色模式: " << v << std::endl;
                return 1;
            }
        }
        else if (a == "--no-normal-map") opt.normal_map = false;
        else if (a == "--no-spec")       opt.spec = false;
        else if (a == "--shininess")     opt.shininess = std::atof(need_value(i, "--shininess"));
        else if (a == "--checker")       force_checker = true;
        else if (a == "--eye")    { if (!parse_vec3(need_value(i, "--eye"),    opt.eye))    { std::cerr << "[错误] --eye 格式应为 x,y,z" << std::endl; return 1; } }
        else if (a == "--center") { if (!parse_vec3(need_value(i, "--center"), opt.center)) { std::cerr << "[错误] --center 格式应为 x,y,z" << std::endl; return 1; } }
        else if (a == "--up")     { if (!parse_vec3(need_value(i, "--up"),     opt.up))     { std::cerr << "[错误] --up 格式应为 x,y,z" << std::endl; return 1; } }
        else if (a == "--light")  { if (!parse_vec3(need_value(i, "--light"),  opt.light_dir)) { std::cerr << "[错误] --light 格式应为 x,y,z" << std::endl; return 1; } }
        else if (a == "--fov")     opt.fov     = std::atof(need_value(i, "--fov"));
        else if (a == "--ambient") opt.ambient = std::atof(need_value(i, "--ambient"));
        else if (a == "--no-shadow") opt.shadow = false;
        else if (a == "--floor")     opt.floor  = true;
        else if (a == "--no-cull")   opt.cull   = false;
        else if (a == "--no-fit")    opt.fit    = false;
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "[错误] 未知选项: " << a << "（用 --help 查看用法）" << std::endl;
            return 1;
        }
        else model_paths.push_back(a);
    }

    // ── 参数合法性检查 ────────────────────────────────────────────────────
    if (model_paths.empty()) {
        std::cerr << "[错误] 没有指定任何模型文件" << std::endl;
        print_usage();
        return 1;
    }
    if (opt.width < 1 || opt.height < 1) {
        std::cerr << "[错误] 图像尺寸必须为正数" << std::endl;
        return 1;
    }
    if (opt.ssaa < 1 || opt.ssaa > 4) {
        std::cerr << "[警告] --ssaa 超出 1..4，已截断" << std::endl;
        opt.ssaa = (opt.ssaa < 1) ? 1 : 4;
    }
    if (norm(opt.light_dir) < 1e-9) {
        std::cerr << "[错误] 光照方向不能是零向量" << std::endl;
        return 1;
    }
    opt.light_dir = normalized(opt.light_dir);

    // ── 加载场景 ──────────────────────────────────────────────────────────
    std::cerr << "正在加载模型..." << std::endl;
    Scene scene;
    for (const std::string& p : model_paths)
        if (!scene.add_model(p, force_checker))
            std::cerr << "[警告] 跳过无法加载的模型: " << p << std::endl;

    if (scene.empty()) {
        std::cerr << "[错误] 没有成功加载任何模型，无法渲染" << std::endl;
        return 1;
    }

    scene.prepare(opt);

    // ── 渲染 ──────────────────────────────────────────────────────────────
    std::cerr << "正在渲染 " << opt.width << "x" << opt.height
              << "（超采样 " << opt.ssaa << "x）..." << std::endl;

    TGAImage result;
    TGAImage depth_image;
    if (opt.mode == ShaderMode::Wire) {
        // 线框走独立路径：它不需要 z-buffer，也不需要着色器
        if (opt.shadow) std::cerr << "[提示] 线框模式不参与光照，已忽略阴影设置" << std::endl;
        result = render_wireframe(scene, opt);
    } else {
        result = scene.render(opt, depth_output.empty() ? nullptr : &depth_image);
    }

    // ── 写出 ──────────────────────────────────────────────────────────────
    if (!save_image(result, output)) {
        std::cerr << "[错误] 保存失败: " << output << std::endl;
        return 1;
    }
    std::cerr << "已输出: " << output << std::endl;

    if (!depth_output.empty()) {
        if (opt.mode == ShaderMode::Wire)
            std::cerr << "[提示] 线框模式没有深度缓冲，跳过 --dump-depth" << std::endl;
        else if (save_image(depth_image, depth_output))
            std::cerr << "已输出深度图: " << depth_output << std::endl;
    }

    return 0;
}
