#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// tgaimage —— 极简 TGA 图片读写与像素容器
//
// 这个文件来自 tinyrenderer 教学项目的风格：不用 stb_image、libpng 等第三方库，
// 自己保存一块连续的像素数组，并提供 get/set/read/write 这些最小接口。
//
// 在本项目里，TGAImage 不只负责 .tga 文件。它也是整个软件渲染器的帧缓冲：
// 光栅化器把每个像素写进 TGAImage，最后 image_io.cpp 再把它保存成 PNG 或 TGA。
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <fstream>
#include <vector>

#pragma pack(push,1)
// TGA 文件头必须严格按字节对齐，不能让编译器插入 padding。
// 因此这里用 #pragma pack(push,1)，保证结构体布局与磁盘格式一一对应。
struct TGAHeader {
    std::uint8_t  idlength = 0;
    std::uint8_t  colormaptype = 0;
    std::uint8_t  datatypecode = 0;
    std::uint16_t colormaporigin = 0;
    std::uint16_t colormaplength = 0;
    std::uint8_t  colormapdepth = 0;
    std::uint16_t x_origin = 0;
    std::uint16_t y_origin = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t  bitsperpixel = 0;
    std::uint8_t  imagedescriptor = 0;
};
#pragma pack(pop)

// 单个像素颜色。历史原因：TGA 常用 BGR/BGRA 存储顺序，
// 所以这里的数组名叫 bgra，访问时 c[0]=B、c[1]=G、c[2]=R、c[3]=A。
// 着色器内部通常使用 RGB 的 vec3，写入 TGAColor 时要手动调换顺序。
struct TGAColor {
    std::uint8_t bgra[4] = { 0,0,0,0 };
    std::uint8_t bytespp = 4;
    std::uint8_t& operator[](const int i) { return bgra[i]; }
    const std::uint8_t& operator[](const int i) const { return bgra[i]; }
};

// 简单图片对象：保存宽高、每像素字节数和连续像素数据。
// 坐标约定沿用 tinyrenderer：get/set 接收的 x,y 是图像坐标，
// 底层 data 按行连续存储，索引 = (x + y*w) * bpp。
struct TGAImage {
    enum Format { GRAYSCALE = 1, RGB = 3, RGBA = 4 };
    TGAImage() = default;

    // 创建一张 w*h 的空图，可选用颜色 c 初始化。
    TGAImage(const int w, const int h, const int bpp, TGAColor c = {});

    // 读写 TGA 文件。write_tga_file 默认做垂直翻转，是为了让输出文件在常见看图软件中
    // 与渲染器内部坐标显示一致；rle=true 时会使用 TGA 的简单游程压缩。
    bool  read_tga_file(const std::string filename);
    bool write_tga_file(const std::string filename, const bool vflip = true, const bool rle = true) const;

    // 整图翻转，主要用于处理不同图片文件的原点方向差异。
    void flip_horizontally();
    void flip_vertically();

    // 像素访问。越界时实现文件里会做保护，返回/忽略无效访问。
    TGAColor get(const int x, const int y) const;
    void set(const int x, const int y, const TGAColor& c);

    int width()  const;
    int height() const;
    int bytespp() const; // 每像素字节数，取值同 Format 枚举（1/3/4）
private:
    // RLE 是 Run-Length Encoding（游程编码）：把连续相同像素压缩成"重复次数 + 像素"。
    // TGA 的 RLE 规则很简单，适合教学项目自己实现。
    bool   load_rle_data(std::ifstream& in);
    bool unload_rle_data(std::ofstream& out) const;
    int w = 0, h = 0;
    std::uint8_t bpp = 0;
    std::vector<std::uint8_t> data = {};
};
