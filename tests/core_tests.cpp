// エンジンのテスト（フレームワークを使わない小さなテスト）。
//
// 実写のRAWは使わず、既知の明るさの場面から露出違いのフレームを作って確かめる。

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

#include "hdrcore/align.hpp"
#include "hdrcore/demosaic.hpp"
#include "hdrcore/dng_writer.hpp"
#include "hdrcore/exposure.hpp"
#include "hdrcore/merge.hpp"
#include "hdrcore/raw_frame.hpp"
#include "libraw/libraw.h"

namespace {

int g_failed = 0, g_checks = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        ++g_checks;                                                   \
        if (!(cond)) {                                                \
            ++g_failed;                                               \
            std::printf("  失敗 %s:%d: %s\n    ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                 \
            std::printf("\n");                                        \
        }                                                             \
    } while (0)

// 場面の明るさ（任意の単位）。左→右のなだらかな勾配、明るい円（太陽）、細かい模様。
double scene(int x, int y, int w, int h) {
    const double gx = static_cast<double>(x) / w, gy = static_cast<double>(y) / h;
    double v = 0.002 + 0.3 * gx * gx + 0.05 * gy;
    v *= 1.0 + 0.2 * std::sin(x * 0.05) * std::sin(y * 0.07);
    const double dx = x - 0.7 * w, dy = y - 0.3 * h;
    const double r2 = (dx * dx + dy * dy) / (0.05 * w * 0.05 * w);
    v += 40.0 * std::exp(-r2);  // 明るい円
    return v;
}

hdr::RawFrame make_frame(int w, int h, double exposure, double gain_dn, float clip, double noise, unsigned seed,
                         double shutter, int ox = 0, int oy = 0) {
    hdr::RawFrame f;
    f.path = f.file_name = "synthetic_" + std::to_string(seed) + ".raw";
    f.width = w;
    f.height = h;
    f.cfa.w = f.cfa.h = 2;
    const uint8_t pat[2][2] = {{0, 1}, {1, 2}};
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) f.cfa.color[y][x] = pat[y][x];
    }
    f.white[0] = f.white[1] = f.white[2] = clip;
    f.crop_w = w;
    f.crop_h = h;
    f.exposure_time = shutter;
    f.fnumber = 8.0;
    f.iso = 100.0;
    f.make = "Test";
    f.model = "Synthetic";
    f.unique_camera_model = "Test Synthetic";
    f.has_color_matrix = true;
    const double cm[3][3] = {{0.7, -0.1, -0.1}, {-0.4, 1.2, 0.25}, {-0.1, 0.2, 0.6}};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) f.color_matrix[i][j] = cm[i][j];
    }
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    const double chan_gain[3] = {0.6, 1.0, 0.8};
    f.data.resize(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int c = f.cfa.at(x, y);
            // 中身を (ox, oy) だけずらした場面（カメラが動いた想定）。
            double v = scene(x + ox, y + oy, w, h) * chan_gain[c] * exposure * gain_dn;
            if (noise > 0.0) v += nd(rng) * std::sqrt(noise * noise + std::max(0.0, v) * 0.5);
            f.data[static_cast<std::size_t>(y) * w + x] = static_cast<float>(std::min<double>(v, clip));
        }
    }
    return f;
}

// ---- 浮動小数点プレディクタ（DNG を LibRaw で読み戻して確かめる） ----
void test_dng_roundtrip() {
    std::printf("DNG の書き出しと読み戻し\n");
    // 縦横ともタイル（256）の端をはみ出す寸法。
    // （高さが1タイルに満たない画像は LibRaw 0.22 が読めない。実写では起きないので試さない）
    const int w = 600, h = 400;
    std::vector<hdr::RawFrame> frames;
    frames.push_back(make_frame(w, h, 1.0, 1000.0, 16000.0f, 0.0, 1, 1.0 / 100));
    frames.push_back(make_frame(w, h, 4.0, 1000.0, 16000.0f, 0.0, 2, 4.0 / 100));
    const hdr::ExposurePlan plan = hdr::estimate_exposures(frames);
    const hdr::MergeResult m = hdr::merge_frames(frames, plan, hdr::MergeOptions());
    char tmpl[] = "/tmp/rbh_test_XXXXXX";
    const int fd = mkstemp(tmpl);
    CHECK(fd >= 0, "一時ファイルを作れない");
    close(fd);
    const std::string path = std::string(tmpl) + ".dng";
    rename(tmpl, path.c_str());
    double pedestal = -1.0;  // 書いた値 − 合成の値（黒の底上げ）。全画素で同じはず
    for (bool compress : {true, false}) {
        hdr::DngWriteOptions opt;
        opt.compress = compress;
        hdr::write_dng(path, m, frames, plan, opt);
        LibRaw raw;
        raw.imgdata.rawparams.options &= ~LIBRAW_RAWOPTIONS_CONVERTFLOAT_TO_INT;
        int rc = raw.open_file(path.c_str());
        CHECK(rc == LIBRAW_SUCCESS, "LibRaw で開けない: %s", libraw_strerror(rc));
        if (rc != LIBRAW_SUCCESS) continue;
        rc = raw.unpack();
        CHECK(rc == LIBRAW_SUCCESS, "展開できない: %s", libraw_strerror(rc));
        const float* fi = raw.imgdata.rawdata.float_image;
        CHECK(fi != nullptr, "float の画素が無い（圧縮=%d）", compress);
        if (!fi) continue;
        CHECK(raw.imgdata.sizes.raw_width == w && raw.imgdata.sizes.raw_height == h, "寸法が違う %dx%d",
              raw.imgdata.sizes.raw_width, raw.imgdata.sizes.raw_height);
        const int pitch = raw.imgdata.sizes.raw_pitch / 4;
        // 書いた値は「合成の値 + 底上げ」。底上げは全画素で同じ正の値（float の丸めの誤差まで）。
        // 書いた値は 65535 倍してある（32bit の CFA の既定）。
        const double p0 = static_cast<double>(fi[0]) / 65535.0 - m.data[0];
        double maxdiff = 0.0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const double d = static_cast<double>(fi[y * pitch + x]) / 65535.0 - m.data[static_cast<std::size_t>(y) * w + x];
                maxdiff = std::max(maxdiff, std::fabs(d - p0));
            }
        }
        CHECK(p0 > 0.0, "黒の底上げが無い（%g）", p0);
        CHECK(maxdiff < 1e-6, "読み戻した値が違う（圧縮=%d、底上げからのずれの最大 %g）", compress, maxdiff);
        pedestal = p0;
        CHECK(raw.COLOR(0, 0) == 0 && raw.COLOR(1, 1) == 2, "CFA の並びが違う");
        // 書き出しの決まり: 白 65535（32bit の CFA）、黒は底上げした値、黒の自動調整はさせない。
        CHECK(raw.imgdata.color.dng_levels.dng_whitelevel[0] == 65535, "WhiteLevel が 65535 でない（%u）",
              raw.imgdata.color.dng_levels.dng_whitelevel[0]);
        CHECK(std::fabs(raw.imgdata.color.dng_levels.dng_fblack - p0 * 65535.0) < 1e-3, "BlackLevel が底上げと違う（%g と %g）",
              raw.imgdata.color.dng_levels.dng_fblack, p0 * 65535.0);
    }
    // LinearRaw（半精度・値 ×32768・WhiteLevel 32768）。色補間した値と、半精度の誤差の範囲で一致すること。
    {
        hdr::DngWriteOptions opt;
        opt.linear_raw = true;
        hdr::write_dng(path, m, frames, plan, opt);
        // 書き出しと同じ手順で、期待する値を作る（底上げした値をノイズを考えて色補間）。
        std::vector<float> lifted(m.data);
        for (float& v : lifted) v += static_cast<float>(pedestal);
        const double el0 = m.brightest_rel_exposure * m.darkest_clip;
        const std::vector<float> rgb = hdr::demosaic(lifted.data(), m.width, m.height, m.cfa,
                                                     hdr::scale_noise(m.brightest_noise_dn, 1.0 / el0), static_cast<float>(pedestal));
        LibRaw raw;
        raw.imgdata.rawparams.options &= ~LIBRAW_RAWOPTIONS_CONVERTFLOAT_TO_INT;
        int rc = raw.open_file(path.c_str());
        if (rc == LIBRAW_SUCCESS) rc = raw.unpack();
        CHECK(rc == LIBRAW_SUCCESS, "LinearRaw を LibRaw で読めない: %s", libraw_strerror(rc));
        const float(*f3)[3] = raw.imgdata.rawdata.float3_image;
        CHECK(f3 != nullptr, "LinearRaw の float の画素が無い");
        if (rc == LIBRAW_SUCCESS && f3) {
            const int pitch = raw.imgdata.sizes.raw_pitch / 12;
            double worst = 0.0;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        const double want = rgb[(static_cast<std::size_t>(y) * w + x) * 3 + c] * 32768.0;
                        const double got = f3[y * pitch + x][c];
                        if (want > 1e-1) worst = std::max(worst, std::fabs(got - want) / want);
                    }
                }
            }
            CHECK(worst < 1e-3, "LinearRaw の値が違う（相対誤差の最大 %g）", worst);
        }
    }
    if (!std::getenv("RBH_KEEP")) std::remove(path.c_str()); else std::printf("  残した: %s\n", path.c_str());
}

// ---- 露出比の推定と合成の正しさ ----
void test_merge_accuracy() {
    std::printf("露出比の推定と合成\n");
    const int w = 800, h = 600;
    const float clip = 15000.0f;
    // 実際の露光は名目値（2段刻み）から少しずれている、という想定。
    const double actual[4] = {1.0, 4.12, 16.6, 65.3};
    const double shutter[4] = {1.0 / 1000, 1.0 / 250, 1.0 / 60, 1.0 / 15};
    std::vector<hdr::RawFrame> frames;
    // 入力の順はわざとばらばらにする。
    const int order_in[4] = {2, 0, 3, 1};
    for (int k : order_in) frames.push_back(make_frame(w, h, actual[k], 400.0, clip, 3.0, 10 + k, shutter[k]));
    const hdr::ExposurePlan plan = hdr::estimate_exposures(frames);
    CHECK(plan.order.size() == 4, "順序の数");
    for (int o = 0; o < 4; ++o) CHECK(plan.order[o] == (o == 0 ? 1 : o == 1 ? 3 : o == 2 ? 0 : 2), "暗い順の並び o=%d → %d", o, plan.order[o]);
    for (int o = 1; o < 4; ++o) {
        const double err = std::log2(plan.rel_exposure[o] / actual[o]);
        CHECK(std::fabs(err) < 0.01, "相対露光量の誤差 o=%d: %+.4f EV（推定 %.4f、正解 %.4f）", o, err, plan.rel_exposure[o], actual[o]);
    }
    for (int i = 0; i < 4; ++i) CHECK(plan.clip[i].detected[1] || i == 1 || i == 3, "飽和レベルの検出 i=%d", i);
    // 露出比の全体最適化: 離れた組も測られ、隣どうしの比（つなぎ目）は実測から大きく動かない。
    CHECK(!plan.wide_fits.empty(), "離れた組の比が測られていない");
    for (const hdr::PairFit& f : plan.fits) {
        CHECK(std::fabs(std::log2(f.solved_ratio / f.ratio)) < 0.01, "つなぎ目の比が実測から動きすぎ %d→%d: %.4f / %.4f", f.dark, f.bright,
              f.solved_ratio, f.ratio);
    }
    {
        // 隣どうしをつなぐだけのとき（global_span = 1）より、正解からの誤差が増えないこと。
        hdr::ExposureOptions one;
        one.global_span = 1;
        const hdr::ExposurePlan chain = hdr::estimate_exposures(frames, one);
        double worst_chain = 0.0, worst_global = 0.0;
        for (int o = 1; o < 4; ++o) {
            worst_chain = std::max(worst_chain, std::fabs(std::log2(chain.rel_exposure[o] / actual[o])));
            worst_global = std::max(worst_global, std::fabs(std::log2(plan.rel_exposure[o] / actual[o])));
        }
        CHECK(worst_global <= worst_chain + 0.005, "全体の最適化で誤差が増えた: %.4f > %.4f EV", worst_global, worst_chain);
    }

    hdr::MergeOptions mo;
    const hdr::MergeResult m = hdr::merge_frames(frames, plan, mo);
    // 正解: 場面の明るさ × 色ごとの利得 × 最も暗いフレームの露光 × 400 / 飽和レベル
    const double chan_gain[3] = {0.6, 1.0, 0.8};
    double worst = 0.0;
    int bad = 0, count = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int c = m.cfa.at(x, y);
            const double truth = scene(x, y, w, h) * chan_gain[c] * actual[0] * 400.0 / clip;
            if (truth >= 0.9 || truth < 1e-4) continue;  // 最も暗いフレームでも飽和する所・ほぼ黒は除く
            const double got = m.data[static_cast<std::size_t>(y) * w + x];
            const double rel = std::fabs(got - truth) / truth;
            ++count;
            // 雑音があるので1画素ずつは甘めに見て、外れの数で判定する。
            if (rel > 0.2) ++bad;
            worst = std::max(worst, rel);
        }
    }
    CHECK(bad < count / 1000, "正解から大きく外れた画素 %d / %d（最大 %.3f）", bad, count, worst);

    // 飽和したブロックに重みが入っていないこと。
    int leak = 0;
    for (int o = 1; o < 4; ++o) {
        const hdr::RawFrame& f = frames[plan.order[o]];
        for (int by = 0; by < m.grid_h; ++by) {
            for (int bx = 0; bx < m.grid_w; ++bx) {
                bool sat = false;
                for (int y = by * 2; y < by * 2 + 2 && y < h; ++y) {
                    for (int x = bx * 2; x < bx * 2 + 2 && x < w; ++x) sat |= f.value(x, y) >= plan.clip[plan.order[o]].level[f.cfa.at(x, y)] * mo.safety;
                }
                if (sat && m.weights[o][static_cast<std::size_t>(by) * m.grid_w + bx] != 0.0f) ++leak;
            }
        }
    }
    CHECK(leak == 0, "飽和したブロックに重みが入っている: %d", leak);

    // 「余裕のある」（隣も含めて s ≤ ramp_start）最も明るいフレームがあるのに、それより暗いフレームへ
    // 重みを回していないこと（暗いフレームのシャドウ・中間調は使わない、という原則）。
    {
        std::vector<std::vector<float>> lv(4, std::vector<float>(static_cast<std::size_t>(m.grid_w) * m.grid_h, 0.0f));
        for (int o = 0; o < 4; ++o) {
            const hdr::RawFrame& f = frames[plan.order[o]];
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const float v = f.value(x, y) / (plan.clip[plan.order[o]].level[f.cfa.at(x, y)] * mo.safety);
                    float& d = lv[o][static_cast<std::size_t>(y / 2) * m.grid_w + x / 2];
                    d = std::max(d, v);
                }
            }
        }
        int wrong = 0;
        for (int y = 0; y < m.grid_h; ++y) {
            for (int x = 0; x < m.grid_w; ++x) {
                int best = 0;
                for (int o = 3; o >= 1 && best == 0; --o) {
                    float mx = 0.0f;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int yy = std::min(m.grid_h - 1, std::max(0, y + dy)), xx = std::min(m.grid_w - 1, std::max(0, x + dx));
                            mx = std::max(mx, lv[o][static_cast<std::size_t>(yy) * m.grid_w + xx]);
                        }
                    }
                    if (mx <= mo.ramp_start) best = o;
                }
                double darker = 0.0;
                for (int o = 0; o < best; ++o) darker += m.weights[o][static_cast<std::size_t>(y) * m.grid_w + x];
                if (darker > 1e-4) ++wrong;
            }
        }
        CHECK(wrong == 0, "余裕のある明るいフレームより暗いフレームへ重みを回したブロック: %d", wrong);
    }

    // 重みの合計は 1。
    double maxdev = 0.0;
    for (std::size_t i = 0; i < m.weights[0].size(); ++i) {
        double s = 0.0;
        for (const auto& wv : m.weights) s += wv[i];
        maxdev = std::max(maxdev, std::fabs(s - 1.0));
    }
    CHECK(maxdev < 1e-5, "重みの合計が 1 でない（最大のずれ %g）", maxdev);

    // 明るさの平均は、どのフレームの範囲でも正解と揃う（継ぎ目の段差が無い）。
    // 横方向の勾配に沿って、列ごとの平均の比を調べる。
    double worst_col = 0.0;
    for (int x = 8; x < w - 8; x += 8) {
        double sg = 0.0, st = 0.0;
        for (int y = 0; y < h / 8; ++y) {  // 明るい円から離れた上の方の帯
            for (int xx = x; xx < x + 8; ++xx) {
                const int c = m.cfa.at(xx, y);
                st += scene(xx, y, w, h) * chan_gain[c] * actual[0] * 400.0 / clip;
                sg += m.data[static_cast<std::size_t>(y) * w + xx];
            }
        }
        worst_col = std::max(worst_col, std::fabs(sg / st - 1.0));
    }
    CHECK(worst_col < 0.01, "列ごとの平均の誤差が大きい（最大 %.4f）", worst_col);
}

// ---- 色補間（RCD） ----
void test_demosaic() {
    std::printf("色補間（RCD）\n");
    const int w = 600, h = 520;  // タイル（256）をまたぐ
    hdr::CfaPattern pat;
    pat.w = pat.h = 2;
    const uint8_t p[2][2] = {{0, 1}, {1, 2}};
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) pat.color[y][x] = p[y][x];
    }
    // 1) 一様な色はそのまま再現する
    const float flat[3] = {0.3f, 0.5f, 0.2f};
    std::vector<float> cfa(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) cfa[static_cast<std::size_t>(y) * w + x] = flat[pat.at(x, y)];
    }
    std::vector<float> rgb = hdr::demosaic(cfa.data(), w, h, pat);
    double worst = 0.0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        for (int c = 0; c < 3; ++c) worst = std::max(worst, static_cast<double>(std::fabs(rgb[i * 3 + c] - flat[c]) / flat[c]));
    }
    CHECK(worst < 1e-5, "一様な色の再現の誤差 %g", worst);

    // 2) 滑らかな模様と、値を 1e-6 倍したもの（HDR の暗部）で、比が保たれる
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int c = pat.at(x, y);
            const double v = (0.2 + 0.15 * std::sin(x * 0.07) * std::cos(y * 0.05)) * (c == 0 ? 0.6 : c == 1 ? 1.0 : 0.8);
            cfa[static_cast<std::size_t>(y) * w + x] = static_cast<float>(v);
        }
    }
    rgb = hdr::demosaic(cfa.data(), w, h, pat);
    std::vector<float> small(cfa);
    for (float& v : small) v *= 1e-6f;
    const std::vector<float> rgb_small = hdr::demosaic(small.data(), w, h, pat);
    double scale_err = 0.0, truth_err = 0.0, seam = 0.0, inner = 0.0;
    int bad = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            for (int c = 0; c < 3; ++c) {
                const float a = rgb[i * 3 + c], b = rgb_small[i * 3 + c] * 1e6f;
                if (!std::isfinite(a) || a < 0.0f) ++bad;
                scale_err = std::max(scale_err, static_cast<double>(std::fabs(a - b) / std::max(1e-6f, a)));
                const double truth = (0.2 + 0.15 * std::sin(x * 0.07) * std::cos(y * 0.05)) * (c == 0 ? 0.6 : c == 1 ? 1.0 : 0.8);
                if (x >= 4 && y >= 4 && x < w - 4 && y < h - 4) truth_err = std::max(truth_err, std::fabs(a - truth) / truth);
            }
            // 列ごとの「両隣の平均からの飛び」。タイルの境目（x = 256, 512）とそれ以外を分けて記録する。
            if (x >= 8 && x < w - 8 && y >= 8 && y < h - 8) {
                const bool edge = (x >= 255 && x <= 257) || (x >= 511 && x <= 513);
                for (int c = 0; c < 3; ++c) {
                    const double l = rgb[(i - 1) * 3 + c], m0 = rgb[i * 3 + c], r = rgb[(i + 1) * 3 + c];
                    const double j = std::fabs(m0 - 0.5 * (l + r)) / m0;
                    if (edge) {
                        seam = std::max(seam, j);
                    } else {
                        inner = std::max(inner, j);
                    }
                }
            }
        }
    }
    CHECK(bad == 0, "NaN・負の値 %d 個", bad);
    CHECK(scale_err < 1e-3, "1e-6 倍したときの比の崩れ %g", scale_err);
    CHECK(truth_err < 0.02, "滑らかな模様の再現の誤差 %g", truth_err);
    CHECK(seam <= inner * 1.2 + 1e-4, "タイルの境目の飛び %g（境目以外の最大 %g）", seam, inner);
}

// ---- 位置合わせ ----
void test_alignment() {
    std::printf("位置合わせ\n");
    const int w = 800, h = 600;
    const float clip = 15000.0f;
    // 基準（2枚目）に対して、1枚目は (+6, −4)、3枚目は (−10, +8) だけ中身がずれている。
    const int off[3][2] = {{6, -4}, {0, 0}, {-10, 8}};
    const double expo[3] = {1.0, 4.0, 16.0};
    const double shutter[3] = {1.0 / 1000, 1.0 / 250, 1.0 / 60};
    std::vector<hdr::RawFrame> frames;
    for (int k = 0; k < 3; ++k) frames.push_back(make_frame(w, h, expo[k], 400.0, clip, 3.0, 40 + k, shutter[k], off[k][0], off[k][1]));
    hdr::ExposurePlan plan = hdr::estimate_exposures(frames);
    const std::vector<hdr::FrameShift> s = hdr::estimate_shifts(frames, plan, 1);
    for (int k = 0; k < 3; ++k) {
        // aligned(x) = original(x + d) が基準と同じ場面になるのは d = −off のとき。
        CHECK(s[k].dx == -off[k][0] && s[k].dy == -off[k][1], "ずれの推定 %d: (%d, %d)、正解 (%d, %d)", k, s[k].dx, s[k].dy,
              -off[k][0], -off[k][1]);
        hdr::apply_shift(frames[k], s[k].dx, s[k].dy);
    }
    int x0, y0, x1, y1;
    hdr::valid_area(frames, x0, y0, x1, y1);
    CHECK(x0 == 6 && y0 == 8 && x1 == w - 10 && y1 == h - 4, "有効な範囲 (%d,%d)-(%d,%d)", x0, y0, x1, y1);
    // 合わせた後は、露出をそろえた値が基準とほぼ一致する（有効な範囲の中）。
    plan = hdr::estimate_exposures(frames);
    double worst = 0.0;
    for (int y = y0 + 4; y < y1 - 4; y += 3) {
        for (int x = x0 + 4; x < x1 - 4; x += 3) {
            const double a = frames[0].value(x, y) * 4.0, b = frames[1].value(x, y);
            if (b > 2000.0 && b < 12000.0 && a < 12000.0) worst = std::max(worst, std::fabs(a - b) / b);
        }
    }
    CHECK(worst < 0.15, "合わせた後の違いが大きい %g", worst);
    // 元に戻せる。
    hdr::apply_shift(frames[0], 0, 0);
    CHECK(frames[0].original.empty() && frames[0].shift_x == 0, "ずれを 0 に戻せない");
}

}  // namespace

int main() {
    test_dng_roundtrip();
    test_merge_accuracy();
    test_demosaic();
    test_alignment();
    std::printf("%d / %d 件成功\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
