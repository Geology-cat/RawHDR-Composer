#include "hdrcore/tone_compress.hpp"

#include <algorithm>
#include <cmath>

#include "hdrcore/filters.hpp"
#include "hdrcore/parallel.hpp"

namespace hdr {

double auto_tone_strength(double span) {
    // 実写の 2 場面（幅 14.3 段・20.8 段）で、手で選んだ 50% 前後になるように合わせた。
    if (span <= 8.0) return 0.15;
    if (span <= 14.0) return 0.15 + (span - 8.0) / 6.0 * 0.35;
    return std::min(0.6, 0.5 + (span - 14.0) / 8.0 * 0.1);
}

ToneCompressResult compress_tone(MergeResult& m, const double neutral[3], const ToneCompressOptions& opt) {
    ToneCompressResult res;
    m.gain.clear();
    m.gain_guide.clear();
    m.gain_base.clear();
    m.max_gain = 1.0;
    m.opening_ev = 0.0;
    m.tone_strength = 0.0;
    double strength = std::min(1.0, std::max(0.0, opt.strength));
    if ((!opt.auto_strength && strength <= 0.0) || m.data.empty()) return res;
    const int b = m.block, gw = m.grid_w, gh = m.grid_h;
    const std::size_t cells = static_cast<std::size_t>(gw) * gh;
    // 基準フレームの白（合成の値の単位）。明るさはこれに対する段で扱う。
    const double wref = 1.0 / m.white_scale;
    double wb[3];
    for (int c = 0; c < 3; ++c) wb[c] = neutral[c] > 0.0 ? 1.0 / neutral[c] : 1.0;
    const double coef[3] = {0.25, 0.5, 0.25};

    // ---- ブロックの明るさ（ホワイトバランスを掛けた色の平均）の対数 ----
    std::vector<float> guide(cells);
    const double floor_l = wref * std::ldexp(1.0, -18);
    parallel_for(gh, [&](int by0, int by1) {
        for (int by = by0; by < by1; ++by) {
            for (int bx = 0; bx < gw; ++bx) {
                double sum[3] = {};
                int n[3] = {};
                for (int y = by * b; y < std::min(m.height, (by + 1) * b); ++y) {
                    for (int x = bx * b; x < std::min(m.width, (bx + 1) * b); ++x) {
                        const int c = m.cfa.at(x, y);
                        sum[c] += m.data[static_cast<std::size_t>(y) * m.width + x];
                        ++n[c];
                    }
                }
                double l = 0.0, wsum = 0.0;
                for (int c = 0; c < 3; ++c) {
                    if (!n[c]) continue;
                    l += coef[c] * wb[c] * sum[c] / n[c];
                    wsum += coef[c];
                }
                l = wsum > 0.0 ? l / wsum : 0.0;
                guide[static_cast<std::size_t>(by) * gw + bx] = static_cast<float>(std::log2(std::max(l, floor_l) / wref));
            }
        }
    });
    // 暗い所のノイズで倍率が画素ごとに揺れないよう、案内の値を軽く（3×3 ブロック）ならす。
    // 明るさが range_sigma 段以上違う隣（月と空など）は混ぜない。混ぜると輪郭のブロックが中間の明るさになり、
    // 月の縁にだけ空の倍率が掛かって白い縁取りが出る。
    const float edge = static_cast<float>(opt.range_sigma);
    const auto smooth_within = [&](std::vector<float>& v, const std::vector<float>& ref) {
        std::vector<float> tmp(cells);
        parallel_for(gh, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < gw; ++x) {
                    const float c = ref[static_cast<std::size_t>(y) * gw + x];
                    double acc = 0.0;
                    int n = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        const int yy = y + dy;
                        if (yy < 0 || yy >= gh) continue;
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int xx = x + dx;
                            if (xx < 0 || xx >= gw) continue;
                            const std::size_t j = static_cast<std::size_t>(yy) * gw + xx;
                            if (std::fabs(ref[j] - c) > edge) continue;
                            acc += v[j];
                            ++n;
                        }
                    }
                    tmp[static_cast<std::size_t>(y) * gw + x] = static_cast<float>(acc / n);
                }
            }
        });
        v.swap(tmp);
    };
    {
        const std::vector<float> ref(guide);
        smooth_within(guide, ref);
    }

    // ---- 大まかな明るさ（輪郭を残して滑らかに） ----
    const float space = std::max(4.0f, std::max(gw, gh) / 80.0f);
    const std::vector<float> base = bilateral_smooth(guide, gw, gh, space, static_cast<float>(opt.range_sigma));
    float hi = -1e9f;
    for (float v : base) hi = std::max(hi, v);
    std::vector<float> sorted(base);
    const std::size_t k_lo = cells * 5 / 1000;
    std::nth_element(sorted.begin(), sorted.begin() + k_lo, sorted.end());
    float lo = sorted[k_lo];
    std::nth_element(sorted.begin(), sorted.begin() + cells / 2, sorted.end());
    const float median = sorted[cells / 2];
    res.before_span = hi - lo;
    if (opt.auto_strength) strength = auto_tone_strength(res.before_span);
    // 0〜50%: 下の目標（knee・top・floor）に向けて倍率を 0 から満額まで強める。
    // 50〜100%: 倍率は満額のまま、目標そのものを厳しくする（明るい所をさらに下げ、暗い所をさらに持ち上げる）。
    // 倍率を満額より大きくすると明るさの順序が入れ替わる（月が空より暗くなる）ので、目標の側で強める。
    // 明るい側・暗い側は、それぞれの倍率（highlight_amount・shadow_amount）を掛けた強さで別々に決める。
    const double str_h = std::min(1.0, std::max(0.0, strength * opt.highlight_amount));
    const double str_s = std::min(1.0, std::max(0.0, strength * opt.shadow_amount));
    const double s_h = std::min(1.0, 2.0 * str_h), s_s = std::min(1.0, 2.0 * str_s);
    const double extra_h = std::max(0.0, 2.0 * str_h - 1.0), extra_s = std::max(0.0, 2.0 * str_s - 1.0);
    const double hk = opt.highlight_knee - 0.5 * extra_h;
    const double highlight_top = opt.highlight_top - 1.0 * extra_h;
    const double sk = opt.shadow_knee + 0.5 * extra_s;
    const double shadow_floor = opt.shadow_floor + 2.0 * extra_s;
    const double s = std::max(s_h, s_s);
    res.strength = strength;
    // 開いたときの明るさを整える（白を shift 段だけ下げたことにする）。
    double shift = 0.0;
    if (opt.auto_brightness) {
        shift = std::min(opt.max_brightness_change, std::max(-opt.max_brightness_change, opt.target_median - median));
    }
    hi += static_cast<float>(shift);
    lo += static_cast<float>(shift);

    // 明るい所の細かい模様は大まかな明るさより上に振れるので、その分だけ行き先を下げ、
    // 模様の明るい部分（月の明るい縁など）まで白の手前に収める。
    double overshoot = 0.0;
    {
        std::vector<float> over;
        for (std::size_t i = 0; i < cells; ++i) {
            if (base[i] + shift > hk) over.push_back(guide[i] - base[i]);
        }
        if (over.size() > 20) {
            const std::size_t k = over.size() * 995 / 1000;
            std::nth_element(over.begin(), over.begin() + k, over.end());
            overshoot = std::min(2.0, std::max(0.0, static_cast<double>(over[k])));
        }
    }
    // ---- 大まかな明るさの行き先 ----
    // 最も明るい所（月）は、場面の中央の明るさより 1.5 段は上に残す。下げすぎると、月のすぐ外の光のにじみ
    // （大まかには空の一部として扱われ、空より明るい模様として残る）が月より明るくなり、明るさの順序が逆転する。
    const double top = std::max({hk + 0.1, highlight_top - overshoot, median + shift + 1.5});
    const KneeCurve high(top - hk, hi - hk);
    const KneeCurve low(sk - shadow_floor, sk - lo);
    m.gain.assign(cells, 1.0f);
    double gmin = 1e9, gmax = 0.0;
    for (std::size_t i = 0; i < cells; ++i) {
        const double b0 = base[i] + shift;
        double nb = b0, si = 0.0;
        if (b0 > hk) {
            nb = hk + high(b0 - hk);
            si = s_h;
        } else if (b0 < sk) {
            nb = sk - low(sk - b0);
            si = s_s;
        }
        const double g = std::exp2(si * (nb - b0));
        m.gain[i] = static_cast<float>(g);
        gmin = std::min(gmin, g);
        gmax = std::max(gmax, g);
    }
    // 倍率のノイズによる細かい揺れをならす（対数で 3×3 ブロック、2 回）。輪郭（明るさが range_sigma 段以上違う隣）
    // はまたがない。輪郭での色の縞は、書き出しで倍率を外して色補間してから掛け直すことで防いでいる。
    {
        std::vector<float> lg(cells);
        for (std::size_t i = 0; i < cells; ++i) lg[i] = std::log2(m.gain[i]);
        for (int pass = 0; pass < 2; ++pass) smooth_within(lg, guide);
        gmin = 1e9;
        gmax = 0.0;
        for (std::size_t i = 0; i < cells; ++i) {
            m.gain[i] = std::exp2(lg[i]);
            gmin = std::min(gmin, static_cast<double>(m.gain[i]));
            gmax = std::max(gmax, static_cast<double>(m.gain[i]));
        }
    }
    res.after_span = res.before_span + std::log2(std::max(1e-9, gmin) / std::max(1e-9, gmax));

    // ---- 倍率を掛ける（ブロックの全画素・全色に同じ値） ----
    parallel_for(m.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* row = m.data.data() + static_cast<std::size_t>(y) * m.width;
            const float* g = m.gain.data() + static_cast<std::size_t>(y / b) * gw;
            for (int x = 0; x < m.width; ++x) row[x] = std::min(1.0f, row[x] * g[x / b]);
        }
    });
    m.max_gain = gmax;
    m.gain_guide = guide;
    m.gain_base = base;
    m.detail_compress = s;
    {
        double wsum = 0.0;
        for (int c = 0; c < 3; ++c) wsum += coef[c];
        for (int c = 0; c < 3; ++c) m.gain_coef[c] = coef[c] * wb[c] / wsum;
    }
    m.gain_wref = wref;
    m.gain_range = 1.0;
    m.opening_ev = shift;
    m.tone_strength = strength;
    res.opening_ev = shift;
    res.applied = true;
    res.min_gain = gmin;
    res.max_gain = gmax;
    return res;
}

}  // namespace hdr
