#include "hdrcore/exposure.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numeric>

#include "hdrcore/parallel.hpp"

namespace hdr {

// ---- 飽和レベル --------------------------------------------------------------------
//
// 色ごとに値のヒストグラムを取り、最大値のすぐ下に画素が固まっていれば（飽和の山）
// そこを飽和レベルとする。最大値の画素がわずかしかなければ、そのフレームは飽和していない。
ClipLevels detect_clip_levels(const RawFrame& f) {
    ClipLevels out;
    const int kBins = 1 << 16;
    std::vector<uint32_t> hist[3];
    for (auto& h : hist) h.assign(kBins, 0);
    std::mutex mu;
    parallel_for(f.height, [&](int y0, int y1) {
        std::vector<uint32_t> local[3];
        for (auto& h : local) h.assign(kBins, 0);
        for (int y = y0; y < y1; ++y) {
            const float* row = f.data.data() + static_cast<std::size_t>(y) * f.width;
            for (int x = 0; x < f.width; ++x) {
                const float v = row[x];
                if (v <= 0.0f) continue;
                const int b = std::min(kBins - 1, static_cast<int>(v + 0.5f));
                ++local[f.cfa.at(x, y)][b];
            }
        }
        std::lock_guard<std::mutex> lock(mu);
        for (int c = 0; c < 3; ++c) {
            for (int i = 0; i < kBins; ++i) hist[c][i] += local[c][i];
        }
    });
    for (int c = 0; c < 3; ++c) {
        uint64_t total = 0;
        int vmax = -1;
        for (int i = 0; i < kBins; ++i) {
            total += hist[c][i];
            if (hist[c][i]) vmax = i;
        }
        out.level[c] = f.white[c];
        if (vmax <= 0) continue;
        const int tol = std::max(2, static_cast<int>(vmax * 0.001));
        uint64_t near = 0;
        for (int i = std::max(0, vmax - tol); i <= vmax; ++i) near += hist[c][i];
        const uint64_t need = std::max<uint64_t>(16, total / 50000);
        if (near >= need) {
            out.level[c] = static_cast<float>(vmax - tol);
            out.detected[c] = true;
        }
    }
    return out;
}

namespace {

struct Sample {
    float b, d;  // 明るいフレーム・暗いフレームのブロック平均（同じ色）
    uint8_t c;
};

// D = s·B + o を最小二乗で当てはめる。相対残差の大きいものを外して繰り返す。
void robust_fit(const std::vector<Sample>& samples, int channel, double& slope, double& offset, int& used) {
    std::vector<const Sample*> s;
    s.reserve(samples.size());
    for (const Sample& x : samples) {
        if (channel < 0 || x.c == channel) s.push_back(&x);
    }
    slope = 0.0;
    offset = 0.0;
    used = static_cast<int>(s.size());
    if (s.size() < 50) return;
    for (int iter = 0; iter < 4; ++iter) {
        double sb = 0, sd = 0, sbb = 0, sbd = 0;
        const double n = static_cast<double>(s.size());
        for (const Sample* x : s) {
            sb += x->b;
            sd += x->d;
            sbb += static_cast<double>(x->b) * x->b;
            sbd += static_cast<double>(x->b) * x->d;
        }
        const double den = n * sbb - sb * sb;
        if (!(std::fabs(den) > 0.0)) return;
        slope = (n * sbd - sb * sd) / den;
        offset = (sd - slope * sb) / n;
        if (iter == 3) break;
        // 相対残差の中央絶対偏差で外れ値を外す。
        std::vector<double> rel(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            const double pred = slope * s[i]->b + offset;
            rel[i] = (s[i]->d - pred) / std::max(1.0, std::fabs(pred));
        }
        std::vector<double> tmp(rel.size());
        for (std::size_t i = 0; i < rel.size(); ++i) tmp[i] = std::fabs(rel[i]);
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        const double mad = std::max(1e-6, tmp[tmp.size() / 2]);
        std::vector<const Sample*> kept;
        kept.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (std::fabs(rel[i]) <= 5.0 * mad) kept.push_back(s[i]);
        }
        if (kept.size() < 50) break;
        s.swap(kept);
    }
    used = static_cast<int>(s.size());
}

// min_dark: 暗い方の値の下限（飽和レベルに対する比）。離れた組では暗い方がノイズと黒のずれに埋もれやすいので上げる。
PairFit fit_pair(const RawFrame& dark, const ClipLevels& clip_d, const RawFrame& bright, const ClipLevels& clip_b,
                 double nominal, const ExposureOptions& opt, double min_dark = 0.0) {
    PairFit fit;
    fit.nominal_ratio = nominal;
    // ブロック（CFA の周期の4倍四方）ごとに、色ごとの平均と最大を取る。
    const int bs = dark.cfa.w * 2;
    const int bw = dark.width / bs, bh = dark.height / bs;
    std::vector<Sample> fit_samples, diag_samples;
    std::mutex mu;
    parallel_for(bh, [&](int by0, int by1) {
        std::vector<Sample> lf, ld;
        for (int by = by0; by < by1; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                double sum_b[3] = {}, sum_d[3] = {};
                float max_b[3] = {}, max_d[3] = {};
                float min_b[3] = {1e30f, 1e30f, 1e30f};
                int n[3] = {};
                for (int y = by * bs; y < (by + 1) * bs; ++y) {
                    for (int x = bx * bs; x < (bx + 1) * bs; ++x) {
                        const int c = dark.cfa.at(x, y);
                        const float vb = bright.value(x, y), vd = dark.value(x, y);
                        sum_b[c] += vb;
                        sum_d[c] += vd;
                        max_b[c] = std::max(max_b[c], vb);
                        min_b[c] = std::min(min_b[c], vb);
                        max_d[c] = std::max(max_d[c], vd);
                        ++n[c];
                    }
                }
                for (int c = 0; c < 3; ++c) {
                    if (!n[c]) continue;
                    if (max_b[c] >= clip_b.level[c] || max_d[c] >= clip_d.level[c]) continue;
                    const Sample s{static_cast<float>(sum_b[c] / n[c]), static_cast<float>(sum_d[c] / n[c]),
                                   static_cast<uint8_t>(c)};
                    if (s.b < opt.fit_lower * clip_b.level[c]) continue;
                    if (s.d < min_dark * clip_d.level[c]) continue;
                    ld.push_back(s);
                    // 輪郭をまたぐブロックは、わずかなずれや動体で比が狂うので当てはめには使わない。
                    const bool flat = max_b[c] - min_b[c] <= 0.3f * s.b + 0.01f * clip_b.level[c];
                    if (flat && max_b[c] < opt.fit_upper * clip_b.level[c]) lf.push_back(s);
                }
            }
        }
        std::lock_guard<std::mutex> lock(mu);
        fit_samples.insert(fit_samples.end(), lf.begin(), lf.end());
        diag_samples.insert(diag_samples.end(), ld.begin(), ld.end());
    });

    // 黒のわずかなずれ・長秒の暗電流・かぶりなどの「足し算の誤差」は、値が小さいほど比を狂わせる。
    // そのため、明るいフレームの値が大きい所だけで当てはめ、数が足りないときだけ下限を下げる。
    // 合成で比が効くのは切り替わる明るさ（明るいフレームの飽和の手前）なので、この選び方で合う。
    struct Tier {
        double lower;
        std::size_t need;
    };
    static const Tier kTiers[] = {{0.10, 300}, {0.03, 1000}, {0.01, 3000}};
    bool enough = false;
    std::vector<Sample> use;
    for (const Tier& t : kTiers) {
        use.clear();
        for (const Sample& s : fit_samples) {
            if (s.b >= t.lower * clip_b.level[s.c]) use.push_back(s);
        }
        if (use.size() >= t.need) {
            enough = true;
            break;
        }
    }
    fit_samples.swap(use);
    double slope = 0, offset = 0;
    int used = 0;
    if (enough) robust_fit(fit_samples, -1, slope, offset, used);
    fit.samples = used;
    if (enough && slope > 0.0) {
        fit.ratio = 1.0 / slope;
        fit.offset = offset;
        fit.measured = true;
        // 名目値から大きく外れたら（1/3段以上）、動体や光源の変化を疑って名目値に戻す。
        if (std::fabs(std::log2(fit.ratio / nominal)) > 0.34) fit.measured = false;
    }
    if (!fit.measured) {
        fit.ratio = nominal;
        fit.offset = 0.0;
    }
    for (int c = 0; c < 3; ++c) {
        double s = 0, o = 0;
        int u = 0;
        robust_fit(fit_samples, c, s, o, u);
        fit.ratio_channel[c] = s > 0.0 && u >= 200 ? 1.0 / s : 0.0;
    }

    // 明るさの帯ごとの比（非線形性の診断）。
    static const double kEdges[] = {0.01, 0.02, 0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.85, 0.9, 0.95, 0.98, 1.0};
    const int nb = static_cast<int>(sizeof(kEdges) / sizeof(kEdges[0])) - 1;
    std::vector<double> sb(nb, 0.0), sd(nb, 0.0);
    std::vector<int> cnt(nb, 0);
    for (const Sample& s : diag_samples) {
        const double frac = s.b / clip_b.level[s.c];
        for (int i = 0; i < nb; ++i) {
            if (frac >= kEdges[i] && frac < kEdges[i + 1]) {
                sb[i] += s.b;
                sd[i] += s.d - fit.offset;
                ++cnt[i];
                break;
            }
        }
    }
    for (int i = 0; i < nb; ++i) {
        RatioBin b;
        b.lo = kEdges[i];
        b.hi = kEdges[i + 1];
        b.samples = cnt[i];
        b.ratio = cnt[i] > 0 && sd[i] > 0.0 ? sb[i] / sd[i] : 0.0;
        fit.bins.push_back(b);
    }
    return fit;
}

}  // namespace

ExposurePlan estimate_exposures(const std::vector<RawFrame>& frames, const ExposureOptions& options) {
    ExposurePlan plan;
    const int n = static_cast<int>(frames.size());
    plan.order.resize(n);
    std::iota(plan.order.begin(), plan.order.end(), 0);
    std::stable_sort(plan.order.begin(), plan.order.end(),
                     [&](int a, int b) { return frames[a].nominal_ev() < frames[b].nominal_ev(); });

    // 飽和レベル。飽和の山が見つからないフレームは、同じ ISO のフレームで見つかった値を借りる
    // （同じ ISO なら飽和値は同じ）。どれにも無ければ機種表の白。
    plan.clip.resize(n);
    for (int i = 0; i < n; ++i) plan.clip[i] = detect_clip_levels(frames[i]);
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            if (plan.clip[i].detected[c]) continue;
            float best = 0.0f;
            for (int j = 0; j < n; ++j) {
                if (j == i || !plan.clip[j].detected[c] || frames[j].iso != frames[i].iso) continue;
                if (best == 0.0f || plan.clip[j].level[c] < best) best = plan.clip[j].level[c];
            }
            if (best > 0.0f) plan.clip[i].level[c] = best;
        }
    }

    // 名目の露光量。EXIF のシャッター速度は「1/320」のように丸めた表示値なので、
    // 1/3段または1/2段の刻み（合うほう）に寄せてから比を取る。
    std::vector<double> ev(n);
    for (int i = 0; i < n; ++i) ev[i] = frames[i].nominal_ev();
    double best_err = 1e9, best_step = 0.0;
    for (double step : {1.0 / 3.0, 0.5}) {
        double err = 0.0;
        for (double e : ev) err += std::fabs(e - std::round(e / step) * step);
        if (err < best_err - 1e-9) {
            best_err = err;
            best_step = step;
        }
    }
    if (n > 0 && best_err / n < 0.06) {
        for (double& e : ev) e = std::round(e / best_step) * best_step;
    }

    plan.rel_exposure.assign(n, 1.0);
    for (int k = 0; k + 1 < n; ++k) {
        const int d = plan.order[k], b = plan.order[k + 1];
        PairFit fit = fit_pair(frames[d], plan.clip[d], frames[b], plan.clip[b], std::exp2(ev[b] - ev[d]), options);
        fit.dark = d;
        fit.bright = b;
        plan.rel_exposure[k + 1] = plan.rel_exposure[k] * fit.ratio;
        plan.fits.push_back(std::move(fit));
    }
    // ---- 全体の最適化 ----
    // 隣どうしの比をつなぐと、それぞれのわずかな偏り（同じ向きに出やすい）が積み重なる。
    // 2 枚以上離れた組の比も測り、すべての組の「対数の露光量の差」を同時に最小二乗で満たす値を求める。
    // 実測できなかった組（名目値）は、ほかに手がかりが無いときだけ効くよう、ごく小さい重みにする。
    for (int g = 2; g <= options.global_span; ++g) {
        for (int k = 0; k + g < n; ++k) {
            const int d = plan.order[k], b = plan.order[k + g];
            PairFit fit = fit_pair(frames[d], plan.clip[d], frames[b], plan.clip[b], std::exp2(ev[b] - ev[d]), options, 0.01);
            fit.dark = d;
            fit.bright = b;
            if (fit.measured) plan.wide_fits.push_back(std::move(fit));
        }
    }
    if (!plan.wide_fits.empty()) {
        // 未知数は e[1..n-1]（e[0] = 0、対数 2 の露光量）。式 e[hi] − e[lo] = log2(比) を重みつき最小二乗で解く。
        // 動く物（波・光の筋）のある組は比が大きく外れるので、残差の大きい組の重みを下げて解き直す（IRLS）。
        std::vector<int> pos(n, -1);
        for (int k = 0; k < n; ++k) pos[plan.order[k]] = k;
        struct Eq {
            int lo, hi;
            double r, w0;
        };
        std::vector<Eq> eqs;
        for (const PairFit& f : plan.fits) eqs.push_back({pos[f.dark], pos[f.bright], std::log2(f.ratio), f.measured ? 1.0 : 1e-6});
        for (const PairFit& f : plan.wide_fits) eqs.push_back({pos[f.dark], pos[f.bright], std::log2(f.ratio), 1.0});
        const int m = n - 1;
        std::vector<double> e(n, 0.0);
        for (int k = 1; k < n; ++k) e[k] = std::log2(plan.rel_exposure[k]);  // 始めはつないだ値
        bool ok = true;
        for (int iter = 0; iter < 6 && ok; ++iter) {
            std::vector<double> ata(static_cast<std::size_t>(m) * m, 0.0), atr(m, 0.0);
            for (const Eq& q : eqs) {
                // 残差が 1% を超える組は、超えた分だけ重みを下げる（Cauchy）。
                const double res = e[q.hi] - e[q.lo] - q.r;
                const double t = res / 0.015;
                const double w = q.w0 / (1.0 + t * t);
                const int a = q.hi - 1, c = q.lo - 1;  // 未知数の番号（-1 は e[0] = 0）
                if (a >= 0) {
                    ata[static_cast<std::size_t>(a) * m + a] += w;
                    atr[a] += w * q.r;
                }
                if (c >= 0) {
                    ata[static_cast<std::size_t>(c) * m + c] += w;
                    atr[c] -= w * q.r;
                }
                if (a >= 0 && c >= 0) {
                    ata[static_cast<std::size_t>(a) * m + c] -= w;
                    ata[static_cast<std::size_t>(c) * m + a] -= w;
                }
            }
            // ガウス・ジョルダンの消去法（m は高々十数）。
            std::vector<double> x(atr);
            for (int i = 0; i < m && ok; ++i) {
                int piv = i;
                for (int r = i + 1; r < m; ++r) {
                    if (std::fabs(ata[static_cast<std::size_t>(r) * m + i]) > std::fabs(ata[static_cast<std::size_t>(piv) * m + i])) piv = r;
                }
                if (std::fabs(ata[static_cast<std::size_t>(piv) * m + i]) < 1e-15) {
                    ok = false;
                    break;
                }
                if (piv != i) {
                    for (int c = 0; c < m; ++c) std::swap(ata[static_cast<std::size_t>(i) * m + c], ata[static_cast<std::size_t>(piv) * m + c]);
                    std::swap(x[i], x[piv]);
                }
                for (int r = 0; r < m; ++r) {
                    if (r == i) continue;
                    const double k = ata[static_cast<std::size_t>(r) * m + i] / ata[static_cast<std::size_t>(i) * m + i];
                    if (k == 0.0) continue;
                    for (int c = i; c < m; ++c) ata[static_cast<std::size_t>(r) * m + c] -= k * ata[static_cast<std::size_t>(i) * m + c];
                    x[r] -= k * x[i];
                }
            }
            if (ok) {
                for (int i = 0; i < m; ++i) e[i + 1] = x[i] / ata[static_cast<std::size_t>(i) * m + i];
            }
        }
        if (ok) {
            for (int k = 1; k < n; ++k) plan.rel_exposure[k] = std::exp2(e[k]);
        }
    }
    for (int k = 0; k + 1 < n; ++k) plan.fits[k].solved_ratio = plan.rel_exposure[k + 1] / plan.rel_exposure[k];
    return plan;
}

}  // namespace hdr
