#include "hdrcore/merged_rgb.hpp"

#include <algorithm>
#include <cmath>

#include "hdrcore/demosaic.hpp"
#include "hdrcore/noise.hpp"
#include "hdrcore/parallel.hpp"

namespace hdr {

float preview_pedestal(const MergeResult& m) {
    const NoiseModel& ndn = m.brightest_noise_dn;
    double read_sd = 0.0;
    for (int c = 0; c < 3; ++c) read_sd = std::max(read_sd, std::sqrt(ndn.O[c]));
    if (!ndn.valid || !(read_sd > 0.0)) read_sd = 8.0;
    return static_cast<float>(16.0 * read_sd * std::max(1.0, m.max_gain) / (m.brightest_rel_exposure * m.darkest_clip));
}

std::vector<float> merged_camera_rgb(const MergeResult& m, float pedestal) {
    // 明暗差の圧縮の倍率は、色補間の前に外してから（倍率を掛ける前の値で色補間し）、後で画素ごとに
    // 滑らかに補間した倍率を掛け直す。倍率が CFA の上で急に変わると、隣り合う色の画素の関係が崩れ、
    // 縁に色の縞が出るため（月の縁で確認）。
    const bool has_gain = !m.gain.empty();
    std::vector<float> lifted(m.data);
    parallel_for(m.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* row = lifted.data() + static_cast<std::size_t>(y) * m.width;
            const float* g = has_gain ? m.gain.data() + static_cast<std::size_t>(y / m.block) * m.grid_w : nullptr;
            for (int x = 0; x < m.width; ++x) row[x] = (g ? row[x] / g[x / m.block] : row[x]) + pedestal;
        }
    });
    // 色補間の判断にはデータから見積もったノイズ（合成の値の単位）を使う。
    const NoiseModel nout = scale_noise(m.brightest_noise_dn, 1.0 / (m.brightest_rel_exposure * m.darkest_clip));
    std::vector<float> rgb = demosaic(lifted.data(), m.width, m.height, m.cfa, nout, pedestal);
    std::vector<float>().swap(lifted);
    if (has_gain) {
        // 倍率を画素に配る。周りの 2×2 ブロックについて「ブロックの明るさ → 倍率（対数）」の関係を距離（双一次）の
        // 重みで直線に当てはめ、画素自身の明るさで読む（局所線形モデル）。月の縁のように何段も明るさが違うブロックが
        // 隣り合う所でも、空の画素には空の倍率、月の画素には月の倍率、中間の明るさの縁の画素には中間の倍率が掛かり、
        // 明るさの順序が保たれる（距離だけで補間すると白い縁取りが、近いブロックの倍率を選ぶと黒い縁取りが出る）。
        // 底上げの分は掛けない。
        const int b = m.block, gw = m.grid_w, gh = m.grid_h;
        const bool guided = m.gain_guide.size() == m.gain.size();
        std::vector<float> lgain(m.gain.size());
        for (std::size_t i = 0; i < lgain.size(); ++i) lgain[i] = std::log2(m.gain[i]);
        const double floor_l = m.gain_wref * std::ldexp(1.0, -18);
        const double reg = m.gain_range * m.gain_range * 0.25;
        const bool has_base = m.gain_base.size() == m.gain.size();  // 明るさがほぼ同じブロックだけのときは平均になる
        parallel_for(m.height, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const float fy = (y - 0.5f * (b - 1)) / b;
                const int iy = static_cast<int>(std::floor(fy));
                for (int x = 0; x < m.width; ++x) {
                    const float fx = (x - 0.5f * (b - 1)) / b;
                    const int ix = static_cast<int>(std::floor(fx));
                    float* p = rgb.data() + (static_cast<std::size_t>(y) * m.width + x) * 3;
                    double sw = 0.0, sg = 0.0, sl = 0.0, sgg = 0.0, sgl = 0.0, sb = 0.0, sgb = 0.0;
                    float lmin = 1e30f, lmax = -1e30f;
                    for (int dy = -1; dy <= 2; ++dy) {
                        const int yy = std::min(gh - 1, std::max(0, iy + dy));
                        const float wy = std::max(0.0f, 1.0f - std::fabs(fy - (iy + dy)) * 0.5f);
                        if (wy <= 0.0f) continue;
                        for (int dx = -1; dx <= 2; ++dx) {
                            const int xx = std::min(gw - 1, std::max(0, ix + dx));
                            const float wx = std::max(0.0f, 1.0f - std::fabs(fx - (ix + dx)) * 0.5f);
                            if (wx <= 0.0f) continue;
                            const std::size_t j = static_cast<std::size_t>(yy) * gw + xx;
                            const double w = wx * wy;
                            const double gj = guided ? m.gain_guide[j] : 0.0;
                            sw += w;
                            sg += w * gj;
                            sl += w * lgain[j];
                            sgg += w * gj * gj;
                            sgl += w * gj * lgain[j];
                            if (has_base) {
                                sb += w * m.gain_base[j];
                                sgb += w * gj * m.gain_base[j];
                            }
                            lmin = std::min(lmin, lgain[j]);
                            lmax = std::max(lmax, lgain[j]);
                        }
                    }
                    const double mg = sg / sw, ml = sl / sw;
                    double lg = ml;
                    if (guided) {
                        double l = 0.0;
                        for (int c = 0; c < 3; ++c) l += m.gain_coef[c] * (p[c] - pedestal);
                        const double lp = std::log2(std::max(l, floor_l) / m.gain_wref);
                        const double var = std::max(0.0, sgg / sw - mg * mg);
                        // 傾きは -0.9 まで（明るい画素ほど出力も明るい、という順序を崩さない）。
                        const double slope = std::max(-0.9, std::min(0.9, (sgl / sw - mg * ml) / (var + reg)));
                        lg = std::min(static_cast<double>(lmax), std::max(static_cast<double>(lmin), ml + slope * (lp - mg)));
                        // 大きな模様を縮める。画素の明るさと大まかな明るさの差（模様）が 1 段を超える分を、強さに応じて
                        // 0.35 倍まで縮める。月のすぐ外の光のにじみ（月より 6 段暗いが、空の一部として大まかな明るさは
                        // 空と同じ）は、空の倍率がそのまま掛かると月より明るくなり、月の周りの白い輪と、その内側の
                        // 縁の画素が暗い点に見える。月の海などの細かい模様（1 段未満）は変えない。
                        if (has_base && m.detail_compress > 0.0) {
                            const double mb = sb / sw;
                            const double sl_b = std::max(-1.0, std::min(1.0, (sgb / sw - mg * mb) / (var + reg)));
                            const double bp = mb + sl_b * (lp - mg);
                            const double dt = lp - bp;
                            const double over = std::fabs(dt) - 1.0;
                            if (over > 0.0) {
                                const double k = 1.0 - 0.65 * m.detail_compress;
                                lg -= (dt > 0 ? 1.0 : -1.0) * over * (1.0 - k);
                            }
                        }
                    }
                    const float g = static_cast<float>(std::exp2(lg));
                    for (int c = 0; c < 3; ++c) p[c] = (p[c] - pedestal) * g + pedestal;
                }
            }
        });
    }
    return rgb;
}

}  // namespace hdr
