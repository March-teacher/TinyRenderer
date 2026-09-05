#include <fstream>
#include <iostream>
#include <vector>
#include <cstdint>
#include <cctype>
#include <algorithm>
#include "image_io.h"

namespace {

    // ── CRC-32（PNG 每个 chunk 的校验和，多项式 0xEDB88320 的反射形式）───────
    std::uint32_t crc32_of(const std::uint8_t* data, const std::size_t n) {
        // 首次调用时构建 256 项查表，之后复用
        static std::uint32_t table[256];
        static bool ready = false;
        if (!ready) {
            for (std::uint32_t i = 0; i < 256; i++) {
                std::uint32_t c = i;
                for (int k = 0; k < 8; k++)
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            ready = true;
        }
        std::uint32_t c = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < n; i++)
            c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    // ── Adler-32（zlib 流尾部的校验和，对"解压后"的原始字节计算）────────────
    std::uint32_t adler32_of(const std::uint8_t* data, const std::size_t n) {
        std::uint32_t a = 1, b = 0;
        for (std::size_t i = 0; i < n; i++) {
            a = (a + data[i]) % 65521;
            b = (b + a) % 65521;
        }
        return (b << 16) | a;
    }

    // 大端写入 32 位整数：PNG 的所有多字节整数都是大端（网络字节序）
    void push_be32(std::vector<std::uint8_t>& out, const std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v >> 24));
        out.push_back(static_cast<std::uint8_t>(v >> 16));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
        out.push_back(static_cast<std::uint8_t>(v));
    }

    // 写一个完整的 PNG chunk：长度(4) + 类型(4) + 数据 + CRC(4)
    // CRC 覆盖"类型 + 数据"，不含长度字段。
    void push_chunk(std::vector<std::uint8_t>& out,
                    const char type[4],
                    const std::vector<std::uint8_t>& payload) {
        push_be32(out, static_cast<std::uint32_t>(payload.size()));

        std::vector<std::uint8_t> crc_input;
        crc_input.reserve(4 + payload.size());
        for (int i = 0; i < 4; i++) crc_input.push_back(static_cast<std::uint8_t>(type[i]));
        crc_input.insert(crc_input.end(), payload.begin(), payload.end());

        out.insert(out.end(), crc_input.begin(), crc_input.end());
        push_be32(out, crc32_of(crc_input.data(), crc_input.size()));
    }

    // ── 把原始字节封装成 zlib 流，全部使用 deflate 的 stored（非压缩）块 ─────
    // stored 块的格式（见 RFC 1951 §3.2.4）：
    //   1 字节：bit0 = BFINAL（是否最后一块），bit1~2 = BTYPE = 00 表示 stored，
    //           其余 bit 填 0 以对齐到字节边界；
    //   2 字节：LEN（小端），本块的数据长度，最大 65535；
    //   2 字节：NLEN，LEN 的按位取反（冗余校验）；
    //   LEN 字节：原样数据。
    std::vector<std::uint8_t> zlib_store(const std::vector<std::uint8_t>& raw) {
        std::vector<std::uint8_t> out;
        // zlib 头：0x78 = CM(8, deflate) + CINFO(7, 32K 窗口)；0x01 使 (0x78<<8|0x01) % 31 == 0
        out.push_back(0x78);
        out.push_back(0x01);

        constexpr std::size_t kMaxBlock = 65535;
        std::size_t offset = 0;
        do {
            const std::size_t len = std::min(kMaxBlock, raw.size() - offset);
            const bool final_block = (offset + len >= raw.size());
            out.push_back(final_block ? 1 : 0);
            out.push_back(static_cast<std::uint8_t>(len & 0xFF));
            out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
            out.push_back(static_cast<std::uint8_t>((~len) & 0xFF));
            out.push_back(static_cast<std::uint8_t>(((~len) >> 8) & 0xFF));
            out.insert(out.end(), raw.begin() + offset, raw.begin() + offset + len);
            offset += len;
        } while (offset < raw.size()); // 用 do-while 保证空图也会产出一个 final 空块

        push_be32(out, adler32_of(raw.data(), raw.size()));
        return out;
    }

    // 大小写无关的后缀判断
    bool ends_with_ci(const std::string& s, const std::string& suffix) {
        if (s.size() < suffix.size()) return false;
        for (std::size_t i = 0; i < suffix.size(); i++) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
            if (a != b) return false;
        }
        return true;
    }

} // namespace

bool write_png_file(const TGAImage& image, const std::string& filename) {
    const int w = image.width();
    const int h = image.height();
    if (w <= 0 || h <= 0) {
        std::cerr << "write_png_file: 空图像，无法写出 " << filename << std::endl;
        return false;
    }

    // 灰度图（如深度图）保留单通道，其余统一写成 24 位真彩
    const bool grayscale = (image.bytespp() == TGAImage::GRAYSCALE);
    const int channels   = grayscale ? 1 : 3;

    // ── 组装 PNG 的"原始扫描线数据"────────────────────────────────────────
    // 每行前面要加一个滤波器类型字节；0 表示 None（不做行间预测）。
    // 真正的编码器会逐行挑选最优滤波器来提升压缩率，我们既然用 stored 块，
    // 滤波就毫无收益，直接全用 None。
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(h) * (1 + static_cast<std::size_t>(w) * channels));
    for (int y = h - 1; y >= 0; y--) {          // TGAImage 第 0 行在底部，PNG 第 0 行在顶部，故倒着遍历
        raw.push_back(0);                        // 滤波器类型：None
        for (int x = 0; x < w; x++) {
            const TGAColor c = image.get(x, y);  // TGAColor 内部是 BGRA 序
            if (grayscale) {
                raw.push_back(c[0]);
            } else {
                raw.push_back(c[2]);             // R
                raw.push_back(c[1]);             // G
                raw.push_back(c[0]);             // B
            }
        }
    }

    std::vector<std::uint8_t> png;
    // PNG 文件签名，固定 8 字节
    const std::uint8_t signature[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };
    png.insert(png.end(), signature, signature + 8);

    // IHDR：宽、高、位深、颜色类型、压缩方法、滤波方法、隔行扫描
    std::vector<std::uint8_t> ihdr;
    push_be32(ihdr, static_cast<std::uint32_t>(w));
    push_be32(ihdr, static_cast<std::uint32_t>(h));
    ihdr.push_back(8);                       // 每通道 8 位
    ihdr.push_back(grayscale ? 0 : 2);       // 颜色类型：0 = 灰度，2 = 真彩 RGB
    ihdr.push_back(0);                       // 压缩方法：仅定义了 0（deflate）
    ihdr.push_back(0);                       // 滤波方法：仅定义了 0
    ihdr.push_back(0);                       // 隔行扫描：0 = 不隔行
    push_chunk(png, "IHDR", ihdr);

    // IDAT：zlib 包装后的图像数据
    push_chunk(png, "IDAT", zlib_store(raw));

    // IEND：空的结束标记
    push_chunk(png, "IEND", {});

    std::ofstream out(filename, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "write_png_file: 无法打开输出文件 " << filename << std::endl;
        return false;
    }
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out.good()) {
        std::cerr << "write_png_file: 写入失败 " << filename << std::endl;
        return false;
    }
    return true;
}

bool save_image(const TGAImage& image, const std::string& filename) {
    if (ends_with_ci(filename, ".png"))
        return write_png_file(image, filename);
    return image.write_tga_file(filename);
}
