#include "hdrcore/preview.hpp"

#include <algorithm>
#include <cmath>

#include "hdrcore/filters.hpp"
#include "hdrcore/parallel.hpp"

namespace hdr {

namespace {

bool invert3(const double m[3][3], double out[3][3]) {
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (!(std::fabs(det) > 1e-12)) return false;
    const double inv = 1.0 / det;
    out[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv;
    out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv;
    out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv;
    out[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv;
    out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv;
    out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv;
    out[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv;
    out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv;
    out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv;
    return true;
}

// カメラRGB（ホワイトバランス済み）→ 線形 sRGB の行列。dcraw と同じ求め方
// （カメラ←sRGB の行列の各行を和 1 に正規化してから逆行列を取る）。
void camera_to_srgb(const double cm[3][3], double out[3][3]) {
    static const double kXyzFromSrgb[3][3] = {
        {0.4124564, 0.3575761, 0.1804375}, {0.2126729, 0.7151522, 0.0721750}, {0.0193339, 0.1191920, 0.9503041}};
    double cam_rgb[3][3];
    for (int i = 0; i < 3; ++i) {
        double sum = 0.0;
        for (int j = 0; j < 3; ++j) {
            cam_rgb[i][j] = 0.0;
            for (int k = 0; k < 3; ++k) cam_rgb[i][j] += cm[i][k] * kXyzFromSrgb[k][j];
            sum += cam_rgb[i][j];
        }
        if (std::fabs(sum) > 1e-9) {
            for (int j = 0; j < 3; ++j) cam_rgb[i][j] /= sum;
        }
    }
    if (!invert3(cam_rgb, out)) {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) out[i][j] = i == j ? 1.0 : 0.0;
        }
    }
}

inline double shoulder(double x) {
    // 0.5 までは線形、その先は 1.0 に漸近する（0.5 で傾き 1 のまま繋がる）。
    if (x <= 0.5) return x;
    return 0.5 + 0.5 * (1.0 - std::exp(-(x - 0.5) / 0.5));
}

// Camera Raw の既定の見え方に近いトーンカーブ（線形 → 線形、白 = 1）。対数 2 の表で、間は直線で補う。
// 中間調（白の 2〜4 段下）を 1 段ほど明るくし、白の 4.5 段より下を沈め、白の手前をなだらかに寝かせる。
// 白の 6 段より下は場面によって違った（沈む／持ち上がる）ので、2 場面の中間にした。
double acr_tone(double v) {
    static const double kX[] = {-10, -9, -8, -7, -6.5, -6, -5.5, -5, -4.5, -4, -3.5, -3, -2.5, -2, -1.5, -1, -0.5, 0};
    static const double kY[] = {-13, -11.6, -10.4, -8.8, -7.85, -7.07, -6.27, -5.48, -4.51, -3.56, -2.70, -1.99, -1.44, -0.94, -0.55, -0.27, -0.06, 0};
    const int n = static_cast<int>(sizeof(kX) / sizeof(kX[0]));
    if (!(v > 0.0)) return 0.0;
    const double l = std::log2(v);
    if (l >= kX[n - 1]) return 1.0;
    if (l < kX[0]) return std::exp2(kY[0] + 1.4 * (l - kX[0]));
    int i = 0;
    while (i + 2 < n && l >= kX[i + 1]) ++i;
    const double t = (l - kX[i]) / (kX[i + 1] - kX[i]);
    return std::exp2(kY[i] + t * (kY[i + 1] - kY[i]));
}

inline uint8_t to_srgb8(double v) {
    v = std::min(1.0, std::max(0.0, v));
    const double s = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    return static_cast<uint8_t>(std::lround(s * 255.0));
}

// 局所トーンマッピング。明るさ（対数）を「大まかな分布」と「細かい模様」に分け、大まかな分布だけを縮める。
// - ハイライト: 白の半分（膝）より上を、最も明るい所が白の少し下に来るよう滑らかに縮める
//   （月のように小さく明るい所も含めるため、割合ではなく最大で決める）
// - 暗部: 白から range 段より暗い所だけを持ち上げる
// 中間調はそのまま。細かい模様（大まかな分布との差）はどこでも残す。
void local_tone_map(RgbFloatImage& img, double range) {
    const int w = img.width, h = img.height;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (n == 0) return;
    std::vector<float> l(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float* p = img.rgb.data() + i * 3;
        const float lum = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        l[i] = std::log2(std::max(lum, 1e-7f));
    }
    const float space = std::max(4.0f, std::max(w, h) / 80.0f);
    const std::vector<float> base = bilateral_smooth(l, w, h, space, 0.7f);
    float hi = -1e9f;
    for (float v : base) hi = std::max(hi, v);
    std::vector<float> sorted(base);
    const std::size_t k_lo = n * 5 / 1000;
    std::nth_element(sorted.begin(), sorted.begin() + k_lo, sorted.end());
    const float lo = sorted[k_lo];
    const double knee = -1.0;                  // 白の半分
    const double top = std::log2(0.7);         // 最も明るい所の行き先（白より下に置き、模様が寝ないように）
    const double floor = top - range;          // これより暗い所を持ち上げる
    const KneeCurve high(top - knee, hi - knee);
    const double shadow_knee = floor + 2.0;
    const KneeCurve low(shadow_knee - floor, shadow_knee - lo);
    parallel_for(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const double b0 = base[i];
                double nb = b0;
                if (b0 > knee) nb = knee + high(b0 - knee);
                else if (b0 < shadow_knee) nb = shadow_knee - low(shadow_knee - b0);
                const float gain = static_cast<float>(std::exp2(nb - b0));
                float* p = img.rgb.data() + i * 3;
                p[0] *= gain;
                p[1] *= gain;
                p[2] *= gain;
            }
        }
    });
}

}  // namespace

RgbFloatImage render_preview_linear(const float* cfa, int width, int height, const CfaPattern& pattern,
                                    const double color_matrix[3][3], const double neutral[3], const PreviewOptions& opt) {
    RgbFloatImage out;
    const int block = pattern.is_xtrans() ? 3 : 2;
    const int longest = std::max(width, height);
    int step = std::max(1, static_cast<int>(std::ceil(static_cast<double>(longest) / block / std::max(16, opt.max_size))));
    const int cell = step * block;
    out.width = width / cell;
    out.height = height / cell;
    if (out.width <= 0 || out.height <= 0) return out;
    out.rgb.assign(static_cast<std::size_t>(out.width) * out.height * 3, 0.0f);

    double m[3][3];
    double sum = 0.0;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) sum += std::fabs(color_matrix[i][j]);
    }
    if (sum > 0.0) {
        camera_to_srgb(color_matrix, m);
    } else {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) m[i][j] = i == j ? 1.0 : 0.0;
        }
    }
    const double gain = std::exp2(opt.exposure_ev);
    double wb[3];
    for (int c = 0; c < 3; ++c) wb[c] = neutral[c] > 0.0 ? 1.0 / neutral[c] : 1.0;

    parallel_for(out.height, [&](int y0, int y1) {
        for (int oy = y0; oy < y1; ++oy) {
            for (int ox = 0; ox < out.width; ++ox) {
                double s[3] = {};
                int n[3] = {};
                for (int y = oy * cell; y < (oy + 1) * cell; ++y) {
                    const float* row = cfa + static_cast<std::size_t>(y) * width;
                    for (int x = ox * cell; x < (ox + 1) * cell; ++x) {
                        const int c = pattern.at(x, y);
                        s[c] += row[x];
                        ++n[c];
                    }
                }
                double cam[3];
                for (int c = 0; c < 3; ++c) cam[c] = (n[c] ? s[c] / n[c] : 0.0) * wb[c] * gain;
                float* dst = out.rgb.data() + (static_cast<std::size_t>(oy) * out.width + ox) * 3;
                for (int i = 0; i < 3; ++i) {
                    dst[i] = static_cast<float>(std::max(0.0, m[i][0] * cam[0] + m[i][1] * cam[1] + m[i][2] * cam[2]));
                }
            }
        }
    });
    return out;
}

RgbFloatImage camera_rgb_preview(const float* rgb, int width, int height, float black, const CfaPattern& pattern,
                                 const double color_matrix[3][3], const double neutral[3], const PreviewOptions& opt) {
    RgbFloatImage out;
    const int block = pattern.is_xtrans() ? 3 : 2;
    const int longest = std::max(width, height);
    const int step = std::max(1, static_cast<int>(std::ceil(static_cast<double>(longest) / block / std::max(16, opt.max_size))));
    const int cell = step * block;
    out.width = width / cell;
    out.height = height / cell;
    if (out.width <= 0 || out.height <= 0) return out;
    out.rgb.assign(static_cast<std::size_t>(out.width) * out.height * 3, 0.0f);
    double m[3][3];
    double sum = 0.0;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) sum += std::fabs(color_matrix[i][j]);
    }
    if (sum > 0.0) {
        camera_to_srgb(color_matrix, m);
    } else {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) m[i][j] = i == j ? 1.0 : 0.0;
        }
    }
    const double gain = std::exp2(opt.exposure_ev);
    double wb[3];
    for (int c = 0; c < 3; ++c) wb[c] = neutral[c] > 0.0 ? 1.0 / neutral[c] : 1.0;
    const double inv_n = 1.0 / (static_cast<double>(cell) * cell);
    parallel_for(out.height, [&](int y0, int y1) {
        for (int oy = y0; oy < y1; ++oy) {
            for (int ox = 0; ox < out.width; ++ox) {
                double s[3] = {};
                for (int y = oy * cell; y < (oy + 1) * cell; ++y) {
                    const float* row = rgb + (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(ox) * cell) * 3;
                    for (int k = 0; k < cell; ++k) {
                        s[0] += row[k * 3];
                        s[1] += row[k * 3 + 1];
                        s[2] += row[k * 3 + 2];
                    }
                }
                double cam[3];
                for (int c = 0; c < 3; ++c) cam[c] = (s[c] * inv_n - black) * wb[c] * gain;
                float* dst = out.rgb.data() + (static_cast<std::size_t>(oy) * out.width + ox) * 3;
                for (int i = 0; i < 3; ++i) {
                    dst[i] = static_cast<float>(std::max(0.0, m[i][0] * cam[0] + m[i][1] * cam[1] + m[i][2] * cam[2]));
                }
            }
        }
    });
    return out;
}

Rgb8Image finish_preview(RgbFloatImage lin, const PreviewOptions& opt) {
    if (opt.local_tone) local_tone_map(lin, opt.tone_range);
    Rgb8Image out;
    out.width = lin.width;
    out.height = lin.height;
    out.rgb.resize(lin.rgb.size());
    parallel_for(lin.height, [&](int y0, int y1) {
        for (std::size_t i = static_cast<std::size_t>(y0) * lin.width * 3; i < static_cast<std::size_t>(y1) * lin.width * 3; ++i) {
            double v = lin.rgb[i];
            if (opt.acr_curve) v = acr_tone(v);
            else if (opt.tone_map) v = shoulder(v);
            out.rgb[i] = to_srgb8(v);
        }
    });
    return out;
}

Rgb8Image render_preview(const float* cfa, int width, int height, const CfaPattern& pattern,
                         const double color_matrix[3][3], const double neutral[3], const PreviewOptions& opt) {
    return finish_preview(render_preview_linear(cfa, width, height, pattern, color_matrix, neutral, opt), opt);
}

Rgb8Image render_frame_preview(const RawFrame& f, const PreviewOptions& options) {
    // 白レベルで割ってから渡す（1.0 = 白）。飽和した所が白く見えるよう、ホワイトバランスを掛けた後で
    // いちばん先に飽和する色の高さで各色を切る（切らないと、G だけが頭打ちになって飽和した所がピンクになる）。
    std::vector<float> norm(f.data.size());
    const float inv[3] = {1.0f / f.white[0], 1.0f / f.white[1], 1.0f / f.white[2]};
    double wb[3], wb_min = 1e30;
    for (int c = 0; c < 3; ++c) {
        wb[c] = f.as_shot_neutral[c] > 0.0 ? 1.0 / f.as_shot_neutral[c] : 1.0;
        wb_min = std::min(wb_min, wb[c]);
    }
    const float top[3] = {static_cast<float>(wb_min / wb[0]), static_cast<float>(wb_min / wb[1]), static_cast<float>(wb_min / wb[2])};
    parallel_for(f.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < f.width; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * f.width + x;
                const int c = f.cfa.at(x, y);
                norm[i] = std::min(top[c], f.data[i] * inv[c]);
            }
        }
    });
    return render_preview(norm.data(), f.width, f.height, f.cfa, f.color_matrix, f.as_shot_neutral, options);
}

Rgb8Image apply_orientation(const Rgb8Image& in, int orientation) {
    if (orientation <= 1 || orientation > 8) return in;
    const bool swap = orientation >= 5;
    Rgb8Image out;
    out.width = swap ? in.height : in.width;
    out.height = swap ? in.width : in.height;
    out.rgb.resize(in.rgb.size());
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            int sx = x, sy = y;
            const int W = out.width, H = out.height;
            switch (orientation) {
                case 2: sx = W - 1 - x; sy = y; break;
                case 3: sx = W - 1 - x; sy = H - 1 - y; break;
                case 4: sx = x; sy = H - 1 - y; break;
                case 5: sx = y; sy = x; break;
                case 6: sx = y; sy = W - 1 - x; break;
                case 7: sx = H - 1 - y; sy = W - 1 - x; break;
                case 8: sx = H - 1 - y; sy = x; break;
                default: break;
            }
            const uint8_t* s = in.rgb.data() + (static_cast<std::size_t>(sy) * in.width + sx) * 3;
            uint8_t* d = out.rgb.data() + (static_cast<std::size_t>(y) * out.width + x) * 3;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
    return out;
}

}  // namespace hdr
