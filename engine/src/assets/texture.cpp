#include "assets/texture.h"

#include <algorithm>
#include <array>
#include <string>

#include "core/zipfile.h"

namespace nova {

namespace {

constexpr uint32_t kFmtRGBA8888 = 0x12;
constexpr uint32_t kFmtRGB565 = 0x13;
constexpr uint32_t kFmtI8 = 0x16;
constexpr uint32_t kFmtPVRTC2 = 0x18;
constexpr uint32_t kFmtPVRTC4 = 0x19;
constexpr uint32_t kFmtETC1 = 0x36;
constexpr uint32_t kFlagTwiddled = 0x200;
constexpr uint32_t kFlagFlipped = 0x10000;

inline int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline int wrap(int a, int n) { return ((a % n) + n) % n; }
inline uint8_t clamp8(int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); }

// Word index for word (x, y) in PVRTC's Morton order (PVRTDecompress TwiddleUV).
uint32_t twiddle(uint32_t nx, uint32_t ny, uint32_t x, uint32_t y) {
    const uint32_t min_dim = std::min(nx, ny);
    uint32_t out = 0, shift = 0;
    for (uint32_t bit = 1; bit < min_dim; bit <<= 1, ++shift) {
        if (y & bit) out |= 1u << (2 * shift);
        if (x & bit) out |= 1u << (2 * shift + 1);
    }
    const uint32_t rest = (ny < nx ? x : y) >> shift;
    return out | (rest << (2 * shift));
}

// ------------------------------------------------------------------ ETC1

const int kEtcTables[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};

Image decode_etc1(const uint8_t* data, size_t size, int w, int h) {
    Image img{w, h, std::vector<uint8_t>(size_t(w) * h * 4, 255)};
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    check_range(size, 0, size_t(bw) * bh * 8);
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = data + (size_t(by) * bw + bx) * 8;
            const uint32_t hi = load_be<uint32_t>(blk), lo = load_be<uint32_t>(blk + 4);
            const bool diff = (hi >> 1) & 1, flip = hi & 1;
            const int cw[2] = {int((hi >> 5) & 7), int((hi >> 2) & 7)};
            int base[2][3];
            const int shifts[3] = {27, 19, 11};
            for (int c = 0; c < 3; ++c) {
                const int sh = shifts[c];
                if (diff) {
                    const int c5 = (hi >> sh) & 31;
                    int d = (hi >> (sh - 3)) & 7;
                    if (d >= 4) d -= 8;
                    const int c5b = (c5 + d) & 31;
                    base[0][c] = (c5 << 3) | (c5 >> 2);
                    base[1][c] = (c5b << 3) | (c5b >> 2);
                } else {
                    base[0][c] = int((hi >> (sh + 1)) & 15) * 17;
                    base[1][c] = int((hi >> (sh - 3)) & 15) * 17;
                }
            }
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const int px = bx * 4 + x, py = by * 4 + y;
                    if (px >= w || py >= h) continue;
                    const int bit = x * 4 + y;
                    const int idx = int(((lo >> (bit + 16)) & 1) << 1 | ((lo >> bit) & 1));
                    const int sub = flip ? (y >= 2) : (x >= 2);
                    const int mag = kEtcTables[cw[sub]][idx & 1];
                    const int delta = idx >= 2 ? -mag : mag;
                    uint8_t* out = &img.rgba[(size_t(py) * w + px) * 4];
                    for (int c = 0; c < 3; ++c) out[c] = clamp8(base[sub][c] + delta);
                }
            }
        }
    }
    return img;
}

// ------------------------------------------------------------------ PVRTC

struct Rgba {
    int r, g, b, a;
};

void pvrtc_colors(uint32_t cd, Rgba& a, Rgba& b) {
    if (cd & 0x8000) {
        a = {int((cd & 0x7C00) >> 10), int((cd & 0x3E0) >> 5), int((cd & 0x1E) | ((cd & 0x1E) >> 4)), 0xF};
    } else {
        a = {int(((cd & 0xF00) >> 7) | ((cd & 0xF00) >> 11)), int(((cd & 0xF0) >> 3) | ((cd & 0xF0) >> 7)),
             int(((cd & 0xE) << 1) | ((cd & 0xE) >> 2)), int((cd & 0x7000) >> 11)};
    }
    if (cd & 0x80000000u) {
        b = {int((cd & 0x7C000000) >> 26), int((cd & 0x3E00000) >> 21), int((cd & 0x1F0000) >> 16), 0xF};
    } else {
        b = {int(((cd & 0xF000000) >> 23) | ((cd & 0xF000000) >> 27)), int(((cd & 0xF00000) >> 19) | ((cd & 0xF00000) >> 23)),
             int(((cd & 0xF0000) >> 15) | ((cd & 0xF0000) >> 19)), int((cd & 0x70000000) >> 27)};
    }
}

Image decode_pvrtc(const uint8_t* data, size_t size, int w, int h, int bpp) {
    const int ww = bpp == 2 ? 8 : 4, wh = 4;
    const int nx = std::max(w / ww, 2), ny = std::max(h / wh, 2);
    const int W = nx * ww, H = ny * wh;
    check_range(size, 0, size_t(nx) * ny * 8);
    std::vector<uint32_t> mod(size_t(nx) * ny), col(size_t(nx) * ny);
    std::vector<Rgba> ca(mod.size()), cb(mod.size());
    for (int y = 0; y < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            const size_t i = size_t(y) * nx + x;
            const uint8_t* word = data + size_t(twiddle(nx, ny, x, y)) * 8;
            mod[i] = load_le<uint32_t>(word);
            col[i] = load_le<uint32_t>(word + 4);
            pvrtc_colors(col[i], ca[i], cb[i]);
        }
    }
    static const int rep[4] = {0, 3, 5, 8};

    // 2bpp: per-word sub-mode (0 direct, 1 H+V, 2 H only, 3 V only) and adjusted bits,
    // then every pixel's 2-bit modulation value.
    std::vector<int> value;
    std::vector<uint8_t> sub_mode;
    if (bpp == 2) {
        sub_mode.resize(mod.size());
        std::vector<uint32_t> mb(mod);
        for (size_t i = 0; i < mod.size(); ++i) {
            const bool interp = col[i] & 1;
            if (!interp) {
                sub_mode[i] = 0;
                continue;
            }
            if (mb[i] & 1) {
                sub_mode[i] = ((mb[i] >> 20) & 1) ? 3 : 2;
                mb[i] = ((mb[i] >> 21) & 1) ? (mb[i] | (1u << 20)) : (mb[i] & ~(1u << 20));
            } else {
                sub_mode[i] = 1;
            }
            mb[i] = (mb[i] & 2) ? (mb[i] | 1u) : (mb[i] & ~1u);
        }
        value.resize(size_t(W) * H);
        for (int py = 0; py < H; ++py) {
            for (int px = 0; px < W; ++px) {
                const size_t wi = size_t(py / wh) * nx + px / ww;
                const int lx = px % ww, ly = py % wh;
                value[size_t(py) * W + px] = (col[wi] & 1) ? int((mb[wi] >> (2 * ((ly * 8 + lx) / 2))) & 3)
                                                           : (((mod[wi] >> (ly * 8 + lx)) & 1) ? 3 : 0);
            }
        }
    }

    Image img{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            // Colours: bilinear between the four words whose centres surround the pixel.
            const int sx = px - ww / 2, sy = py - wh / 2;
            int x0 = floor_div(sx, ww), y0 = floor_div(sy, wh);
            const int fx = sx - x0 * ww, fy = sy - y0 * wh;
            x0 = wrap(x0, nx);
            y0 = wrap(y0, ny);
            const int x1 = (x0 + 1) % nx, y1 = (y0 + 1) % ny;
            const int wP = (wh - fy) * (ww - fx), wQ = (wh - fy) * fx, wR = fy * (ww - fx), wS = fy * fx;
            auto interp = [&](const std::vector<Rgba>& c) {
                const Rgba& P = c[size_t(y0) * nx + x0];
                const Rgba& Q = c[size_t(y0) * nx + x1];
                const Rgba& R = c[size_t(y1) * nx + x0];
                const Rgba& S = c[size_t(y1) * nx + x1];
                Rgba r{P.r * wP + Q.r * wQ + R.r * wR + S.r * wS, P.g * wP + Q.g * wQ + R.g * wR + S.g * wS,
                       P.b * wP + Q.b * wQ + R.b * wR + S.b * wS, P.a * wP + Q.a * wQ + R.a * wR + S.a * wS};
                if (bpp == 2) return Rgba{(r.r >> 7) + (r.r >> 2), (r.g >> 7) + (r.g >> 2), (r.b >> 7) + (r.b >> 2),
                                          (r.a >> 5) + (r.a >> 1)};
                return Rgba{(r.r >> 6) + (r.r >> 1), (r.g >> 6) + (r.g >> 1), (r.b >> 6) + (r.b >> 1), (r.a >> 4) + r.a};
            };
            const Rgba A = interp(ca), B = interp(cb);

            // Modulation, from the word containing the pixel.
            const size_t wi = size_t(py / wh) * nx + px / ww;
            const int lx = px % ww, ly = py % wh;
            int m;
            bool punch = false;
            if (bpp == 4) {
                const int v = int((mod[wi] >> (2 * (ly * 4 + lx))) & 3);
                if (col[wi] & 1) {
                    static const int rep1[4] = {0, 4, 4, 8};
                    m = rep1[v];
                    punch = v == 2;
                } else {
                    m = rep[v];
                }
            } else {
                const int smode = sub_mode[wi];
                auto nb = [&](int dx, int dy) { return rep[value[size_t(wrap(py + dy, H)) * W + wrap(px + dx, W)]]; };
                const int own = rep[value[size_t(py) * W + px]];
                if (smode == 0 || ((lx ^ ly) & 1) == 0) m = own;
                else if (smode == 1) m = (nb(0, -1) + nb(0, 1) + nb(-1, 0) + nb(1, 0) + 2) / 4;
                else if (smode == 2) m = (nb(-1, 0) + nb(1, 0) + 1) / 2;
                else m = (nb(0, -1) + nb(0, 1) + 1) / 2;
            }
            uint8_t* out = &img.rgba[(size_t(py) * w + px) * 4];
            out[0] = clamp8((A.r * (8 - m) + B.r * m) / 8);
            out[1] = clamp8((A.g * (8 - m) + B.g * m) / 8);
            out[2] = clamp8((A.b * (8 - m) + B.b * m) / 8);
            out[3] = punch ? 0 : clamp8((A.a * (8 - m) + B.a * m) / 8);
        }
    }
    (void)W;
    return img;
}

}  // namespace

bool Image::has_alpha() const {
    for (size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] < 250) return true;
    }
    return false;
}

Image decode_pvr2(const Bytes& buf) {
    if (buf.size() < 52 || std::memcmp(&buf[44], "PVR!", 4) != 0) throw FormatError("not a PVR v2 file");
    const uint32_t hsize = read_le<uint32_t>(buf, 0);
    const int h = int(read_le<uint32_t>(buf, 4)), w = int(read_le<uint32_t>(buf, 8));
    const uint32_t flags = read_le<uint32_t>(buf, 16);
    const uint32_t fmt = flags & 0xFF;
    check_range(buf.size(), hsize, 0);
    const uint8_t* data = buf.data() + hsize;
    const size_t size = buf.size() - hsize;
    Image img;
    if (fmt == kFmtETC1) {
        img = decode_etc1(data, size, w, h);
    } else if (fmt == kFmtPVRTC4 || fmt == kFmtPVRTC2) {
        img = decode_pvrtc(data, size, w, h, fmt == kFmtPVRTC4 ? 4 : 2);
    } else if (fmt == kFmtRGBA8888 || fmt == kFmtRGB565 || fmt == kFmtI8) {
        img = Image{w, h, std::vector<uint8_t>(size_t(w) * h * 4, 255)};
        const size_t px_size = fmt == kFmtRGBA8888 ? 4 : fmt == kFmtRGB565 ? 2 : 1;
        check_range(size, 0, size_t(w) * h * px_size);
        for (size_t i = 0; i < size_t(w) * h; ++i) {
            // Twiddled uncompressed data stores pixel i at its Morton index.
            const size_t src = (flags & kFlagTwiddled) ? twiddle(w, h, uint32_t(i % w), uint32_t(i / w)) : i;
            uint8_t* out = &img.rgba[i * 4];
            if (fmt == kFmtRGBA8888) {
                std::memcpy(out, data + src * 4, 4);
            } else if (fmt == kFmtRGB565) {
                const uint32_t v = load_le<uint16_t>(data + src * 2);
                const uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                out[0] = uint8_t(r << 3 | r >> 2);
                out[1] = uint8_t(g << 2 | g >> 4);
                out[2] = uint8_t(b << 3 | b >> 2);
            } else {
                out[0] = out[1] = out[2] = data[src];
            }
        }
    } else {
        throw FormatError("unsupported PVR pixel format " + std::to_string(fmt));
    }
    if (flags & kFlagFlipped) {
        const size_t row = size_t(w) * 4;
        for (int y = 0; y < h / 2; ++y) {
            std::swap_ranges(img.rgba.begin() + y * row, img.rgba.begin() + (y + 1) * row,
                             img.rgba.begin() + (h - 1 - y) * row);
        }
    }
    return img;
}

Image load_texture(const Bytes& data) {
    if (is_zip(data)) {
        const Bytes rgb = unzip_member(data, "rgb.pvr"), alpha = unzip_member(data, "alpha.pvr");
        if (rgb.empty()) throw FormatError("ETC archive without rgb.pvr");
        Image img = decode_pvr2(rgb);
        if (!alpha.empty()) {
            const Image a = decode_pvr2(alpha);
            if (a.width != img.width || a.height != img.height) throw FormatError("alpha size mismatch");
            for (size_t i = 0; i < size_t(img.width) * img.height; ++i) img.rgba[i * 4 + 3] = a.rgba[i * 4 + 1];
        }
        return img;
    }
    return decode_pvr2(data);
}

}  // namespace nova
