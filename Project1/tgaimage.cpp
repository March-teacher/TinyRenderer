#include <iostream>
#include <cstring>
#include "tgaimage.h"

TGAImage::TGAImage(const int w, const int h, const int bpp, TGAColor c) : w(w), h(h), bpp(bpp), data(w* h* bpp, 0) {
    // 当前调用方基本都传默认黑色，所以这里先只分配并清零 data。
    // 参数 c 保留为接口扩展点：以后如果想构造时填充背景色，可以在这里统一实现。
}

bool TGAImage::read_tga_file(const std::string filename) {
    std::ifstream in;
    in.open(filename, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "can't open file " << filename << "\n";
        return false;
    }
    TGAHeader header;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in.good()) {
        std::cerr << "an error occured while reading the header\n";
        return false;
    }
    w = header.width;
    h = header.height;
    // TGA 文件头记录的是 bits per pixel，这里换成每像素字节数，方便后续索引。
    bpp = header.bitsperpixel >> 3;
    if (w <= 0 || h <= 0 || (bpp != GRAYSCALE && bpp != RGB && bpp != RGBA)) {
        std::cerr << "bad bpp (or width/height) value\n";
        return false;
    }
    size_t nbytes = bpp * w * h;
    data = std::vector<std::uint8_t>(nbytes, 0);
    if (3 == header.datatypecode || 2 == header.datatypecode) {
        // 2 = 未压缩真彩，3 = 未压缩灰度；像素数据可直接整块读入。
        in.read(reinterpret_cast<char*>(data.data()), nbytes);
        if (!in.good()) {
            std::cerr << "an error occured while reading the data\n";
            return false;
        }
    }
    else if (10 == header.datatypecode || 11 == header.datatypecode) {
        // 10/11 = RLE 压缩版本，需要按包头逐段解码。
        if (!load_rle_data(in)) {
            std::cerr << "an error occured while reading the data\n";
            return false;
        }
    }
    else {
        std::cerr << "unknown file format " << (int)header.datatypecode << "\n";
        return false;
    }
    // TGA 的图像原点可以在左下或左上，也可以左右镜像。
    // 为了让后续渲染逻辑只面对一种内部行序，读取后统一到"第 0 行在顶部"。
    if (!(header.imagedescriptor & 0x20))
        flip_vertically();
    if (header.imagedescriptor & 0x10)
        flip_horizontally();
    std::cerr << w << "x" << h << "/" << bpp * 8 << "\n";
    return true;
}

bool TGAImage::load_rle_data(std::ifstream& in) {
    size_t pixelcount = w * h;
    size_t currentpixel = 0;
    size_t currentbyte = 0;
    TGAColor colorbuffer;
    do {
        std::uint8_t chunkheader = 0;
        chunkheader = in.get();
        if (!in.good()) {
            std::cerr << "an error occured while reading the data\n";
            return false;
        }
        if (chunkheader < 128) {
            // raw packet：接下来有 chunkheader+1 个不同像素，逐个读入。
            chunkheader++;
            for (int i = 0; i < chunkheader; i++) {
                in.read(reinterpret_cast<char*>(colorbuffer.bgra), bpp);
                if (!in.good()) {
                    std::cerr << "an error occured while reading the header\n";
                    return false;
                }
                for (int t = 0; t < bpp; t++)
                    data[currentbyte++] = colorbuffer.bgra[t];
                currentpixel++;
                if (currentpixel > pixelcount) {
                    std::cerr << "Too many pixels read\n";
                    return false;
                }
            }
        }
        else {
            // run-length packet：接下来只存一个像素，要重复 chunkheader-127 次。
            chunkheader -= 127;
            in.read(reinterpret_cast<char*>(colorbuffer.bgra), bpp);
            if (!in.good()) {
                std::cerr << "an error occured while reading the header\n";
                return false;
            }
            for (int i = 0; i < chunkheader; i++) {
                for (int t = 0; t < bpp; t++)
                    data[currentbyte++] = colorbuffer.bgra[t];
                currentpixel++;
                if (currentpixel > pixelcount) {
                    std::cerr << "Too many pixels read\n";
                    return false;
                }
            }
        }
    } while (currentpixel < pixelcount);
    return true;
}

bool TGAImage::write_tga_file(const std::string filename, const bool vflip, const bool rle) const {
    // 这三个区域是 TGA 2.0 的尾部信息。本项目不使用开发者区/扩展区，
    // 但写入标准 footer 能让部分查看器更稳妥地识别文件。
    constexpr std::uint8_t developer_area_ref[4] = { 0, 0, 0, 0 };
    constexpr std::uint8_t extension_area_ref[4] = { 0, 0, 0, 0 };
    constexpr std::uint8_t footer[18] = { 'T','R','U','E','V','I','S','I','O','N','-','X','F','I','L','E','.','\0' };
    std::ofstream out;
    out.open(filename, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "can't open file " << filename << "\n";
        return false;
    }
    TGAHeader header = {};
    header.bitsperpixel = bpp << 3;
    header.width = w;
    header.height = h;
    header.datatypecode = (bpp == GRAYSCALE ? (rle ? 11 : 3) : (rle ? 10 : 2));
    // vflip=true 时写成左下原点，兼容 tinyrenderer 原始习惯；
    // vflip=false 时写成左上原点，更接近常见 UI/图片坐标。
    header.imagedescriptor = vflip ? 0x00 : 0x20; // top-left or bottom-left origin
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!out.good()) goto err;
    if (!rle) {
        out.write(reinterpret_cast<const char*>(data.data()), w * h * bpp);
        if (!out.good()) goto err;
    }
    else if (!unload_rle_data(out)) goto err;
    out.write(reinterpret_cast<const char*>(developer_area_ref), sizeof(developer_area_ref));
    if (!out.good()) goto err;
    out.write(reinterpret_cast<const char*>(extension_area_ref), sizeof(extension_area_ref));
    if (!out.good()) goto err;
    out.write(reinterpret_cast<const char*>(footer), sizeof(footer));
    if (!out.good()) goto err;
    return true;
err:
    std::cerr << "can't dump the tga file\n";
    return false;
}

bool TGAImage::unload_rle_data(std::ofstream& out) const {
    const std::uint8_t max_chunk_length = 128;
    size_t npixels = w * h;
    size_t curpix = 0;
    while (curpix < npixels) {
        size_t chunkstart = curpix * bpp;
        size_t curbyte = curpix * bpp;
        std::uint8_t run_length = 1;
        bool raw = true;
        // 尝试从当前位置向后看，判断这一段更适合写成 raw packet 还是 run-length packet。
        // TGA RLE 单个 packet 最多 128 个像素，所以 run_length 也不能超过 128。
        while (curpix + run_length < npixels && run_length < max_chunk_length) {
            bool succ_eq = true;
            for (int t = 0; succ_eq && t < bpp; t++)
                succ_eq = (data[curbyte + t] == data[curbyte + t + bpp]);
            curbyte += bpp;
            if (1 == run_length)
                raw = !succ_eq;
            if (raw && succ_eq) {
                run_length--;
                break;
            }
            if (!raw && !succ_eq)
                break;
            run_length++;
        }
        curpix += run_length;
        // packet header 的最高位区分类型：
        //   0xxxxxxx 表示 raw，低 7 位 + 1 是像素数；
        //   1xxxxxxx 表示 run-length，低 7 位 + 1 是重复次数。
        out.put(raw ? run_length - 1 : run_length + 127);
        if (!out.good()) return false;
        out.write(reinterpret_cast<const char*>(data.data() + chunkstart), (raw ? run_length * bpp : bpp));
        if (!out.good()) return false;
    }
    return true;
}

TGAColor TGAImage::get(const int x, const int y) const {
    if (!data.size() || x < 0 || y < 0 || x >= w || y >= h) return {};
    TGAColor ret = { 0, 0, 0, 0, bpp };
    // 每个像素占 bpp 个字节，整张图按行连续存储。
    const std::uint8_t* p = data.data() + (x + y * w) * bpp;
    for (int i = bpp; i--; ret.bgra[i] = p[i]);
    return ret;
}

void TGAImage::set(int x, int y, const TGAColor& c) {
    if (!data.size() || x < 0 || y < 0 || x >= w || y >= h) return;
    // 只复制当前图像格式需要的 bpp 个字节；RGB 图不会写 alpha。
    memcpy(data.data() + (x + y * w) * bpp, c.bgra, bpp);
}

void TGAImage::flip_horizontally() {
    // 左右翻转：第 i 列与第 w-1-i 列交换，每个颜色通道都要交换。
    for (int i = 0; i < w / 2; i++)
        for (int j = 0; j < h; j++)
            for (int b = 0; b < bpp; b++)
                std::swap(data[(i + j * w) * bpp + b], data[(w - 1 - i + j * w) * bpp + b]);
}

void TGAImage::flip_vertically() {
    // 上下翻转：第 j 行与第 h-1-j 行交换。
    for (int i = 0; i < w; i++)
        for (int j = 0; j < h / 2; j++)
            for (int b = 0; b < bpp; b++)
                std::swap(data[(i + j * w) * bpp + b], data[(i + (h - 1 - j) * w) * bpp + b]);
}

int TGAImage::width() const {
    return w;
}

int TGAImage::height() const {
    return h;
}

int TGAImage::bytespp() const {
    return bpp;
}
