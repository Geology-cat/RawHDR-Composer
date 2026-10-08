#include "hdrcore/merge.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "hdrcore/noise.hpp"
#include "hdrcore/parallel.hpp"

namespace hdr {

namespace {

// 箱型のぼかし（半径 r、端は端の値を延ばす）を横・縦に1回ずつ掛ける。
// 1回の台（影響の届く範囲）は正方形の半径 r。
void box_blur(std::vector<float>& img, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<float> tmp(img.size());
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    // 横
    parallel_for(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* src = img.data() + static_cast<std::size_t>(y) * w;
            float* dst = tmp.data() + static_cast<std::size_t>(y) * w;
            double acc = 0.0;
            for (int k = -r; k <= r; ++k) acc += src[std::min(w - 1, std::max(0, k))];
            for (int x = 0; x < w; ++x) {
                dst[x] = static_cast<float>(acc) * inv;
                acc += src[std::min(w - 1, x + r + 1)] - src[std::max(0, x - r)];
            }
        }
    });
    // 縦
    parallel_for(w, [&](int x0, int x1) {
        std::vector<double> acc(static_cast<std::size_t>(x1 - x0), 0.0);
        for (int x = x0; x < x1; ++x) {
            for (int k = -r; k <= r; ++k) acc[x - x0] += tmp[static_cast<std::size_t>(std::min(h - 1, std::max(0, k))) * w + x];
        }
        for (int y = 0; y < h; ++y) {
            const std::size_t add = static_cast<std::size_t>(std::min(h - 1, y + r + 1)) * w;
            const std::size_t sub = static_cast<std::size_t>(std::max(0, y - r)) * w;
            float* dst = img.data() + static_cast<std::size_t>(y) * w;
            for (int x = x0; x < x1; ++x) {
                dst[x] = static_cast<float>(acc[x - x0]) * inv;
                acc[x - x0] += tmp[add + x] - tmp[sub + x];
            }
        }
    }, 64);
}

// ---- ゴースト（動いた物）対策 ----
//
// level[o]: ブロックの明るさ（飽和の閾値に対する比、色をまたいだ最大）。order の順。
// 結果は res.weights を書き換え、res.ghost_* に残す。
void deghost(MergeResult& res, const std::vector<RawFrame>& frames, const ExposurePlan& plan,
             const std::vector<std::vector<float>>& level, const MergeOptions& opt, int ref_o) {
    const int n = static_cast<int>(frames.size());
    const int b = res.block, gw = res.grid_w, gh = res.grid_h;
    const int G = 4;  // 区画の大きさ（ブロック）。Bayer で 8 画素四方
    const int cw = (gw + G - 1) / G, ch = (gh + G - 1) / G;
    const std::size_t ncell = static_cast<std::size_t>(cw) * ch;
    const double sens = std::min(1.0, std::max(0.0, opt.ghost_sensitivity));
    // 違いの判定: ノイズの標準偏差の k 倍を超え、かつ明るさの比で tol 以上違う。
    // 比の条件は、露出比のわずかな誤差・飽和の手前の非線形さで、明るい静止物が引っかからないようにするため。
    const double k = 6.0 - 4.0 * sens;      // 0 → 6σ・20%、0.5 → 4σ・12%、1 → 2σ・4%
    const double tol = 0.20 - 0.16 * sens;
    // 区画ごとの平均（DN を相対露光量で割ったもの）・平均のノイズの分散・飽和しているか。
    std::vector<std::vector<float>> mean(n), var(n);
    std::vector<std::vector<uint8_t>> sat(n);
    for (int o = 0; o < n; ++o) {
        const int fi = plan.order[o];
        const RawFrame& f = frames[fi];
        const NoiseModel nm = estimate_noise(f, plan.clip[fi]);
        double S = 0.0, O = 0.0;
        for (int c = 0; c < 3; ++c) {
            S += nm.S[c] / 3.0;
            O += nm.O[c] / 3.0;
        }
        if (!nm.valid) {
            S = 1.0;
            O = 4.0;
        }
        const double e = plan.rel_exposure[o];
        mean[o].assign(ncell, 0.0f);
        var[o].assign(ncell, 0.0f);
        sat[o].assign(ncell, 0);
        parallel_for(ch, [&](int cy0, int cy1) {
            for (int cy = cy0; cy < cy1; ++cy) {
                for (int cx = 0; cx < cw; ++cx) {
                    const std::size_t ci = static_cast<std::size_t>(cy) * cw + cx;
                    double sum = 0.0;
                    long cnt = 0;
                    float mx = 0.0f;
                    for (int by = cy * G; by < std::min(gh, (cy + 1) * G); ++by) {
                        for (int bx = cx * G; bx < std::min(gw, (cx + 1) * G); ++bx) {
                            mx = std::max(mx, level[o][static_cast<std::size_t>(by) * gw + bx]);
                        }
                    }
                    for (int y = cy * G * b; y < std::min(res.height, (cy + 1) * G * b); ++y) {
                        const float* row = f.data.data() + static_cast<std::size_t>(y) * f.width;
                        for (int x = cx * G * b; x < std::min(res.width, (cx + 1) * G * b); ++x) {
                            sum += row[x];
                            ++cnt;
                        }
                    }
                    const double m = cnt ? sum / cnt : 0.0;
                    mean[o][ci] = static_cast<float>(m / e);
                    var[o][ci] = static_cast<float>((S * std::max(0.0, m) + O) / std::max<long>(1, cnt) / (e * e));
                    sat[o][ci] = mx >= 1.0f ? 1 : 0;
                }
            }
        });
    }
    // 隣り合う露出どうしで比べ、違う区画に印をつける。
    std::vector<uint8_t> flag(ncell, 0);
    for (int o = 0; o + 1 < n; ++o) {
        for (std::size_t i = 0; i < ncell; ++i) {
            if (sat[o][i] || sat[o + 1][i]) continue;
            const double a = mean[o][i], c = mean[o + 1][i];
            const double d = std::fabs(a - c);
            if (d > k * std::sqrt(static_cast<double>(var[o][i]) + var[o + 1][i]) && d > tol * std::max(std::fabs(a), std::fabs(c))) flag[i] = 1;
        }
    }
    // つながった印の塊を求める（4 近傍）。小さすぎる塊（3 区画未満）はノイズの外れとみなして捨てる。
    std::vector<int> label(ncell, -1);
    std::vector<int> queue;
    std::vector<uint8_t> keep(ncell, 0);
    for (std::size_t s0 = 0; s0 < ncell; ++s0) {
        if (!flag[s0] || label[s0] >= 0) continue;
        queue.assign(1, static_cast<int>(s0));
        label[s0] = 0;
        for (std::size_t qi = 0; qi < queue.size(); ++qi) {
            const int i = queue[qi];
            const int x = i % cw, y = i / cw;
            const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (const auto& q : nb) {
                if (q[0] < 0 || q[0] >= cw || q[1] < 0 || q[1] >= ch) continue;
                const int j = q[1] * cw + q[0];
                if (flag[j] && label[j] < 0) {
                    label[j] = 0;
                    queue.push_back(j);
                }
            }
        }
        if (queue.size() >= 3) {
            for (int i : queue) keep[i] = 1;
        }
    }
    // 少し広げる（2 区画）: 動いた物の縁・にじみも同じフレームから取る。
    std::vector<uint8_t> grown(keep);
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<uint8_t> next(grown);
        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                if (grown[static_cast<std::size_t>(y) * cw + x]) continue;
                bool any = false;
                for (int dy = -1; dy <= 1 && !any; ++dy) {
                    for (int dx = -1; dx <= 1 && !any; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx >= 0 && xx < cw && yy >= 0 && yy < ch && grown[static_cast<std::size_t>(yy) * cw + xx]) any = true;
                    }
                }
                if (any) next[static_cast<std::size_t>(y) * cw + x] = 1;
            }
        }
        grown.swap(next);
    }
    // 動いた所では「手本」のフレームと矛盾しないフレームだけを使う。手本は基準フレーム（ref_o）。基準フレームが
    // 飽和している区画では、それより暗い中で最も明るい飽和していないフレーム。手本がその区画を受け持てない
    // フレームでも、違いがノイズで説明できる（＝動いていない）なら使ってよいので、暗い所は明るいフレームの
    // 少ないノイズのまま、動いた物だけが手本の位置・形にそろう。
    std::vector<uint8_t> region(ncell, 0);
    for (std::size_t i = 0; i < ncell; ++i) region[i] = grown[i];
    std::vector<int8_t> anchor(ncell, -1);
    std::vector<std::vector<float>> deny(n, std::vector<float>(ncell, 0.0f));
    std::size_t restricted = 0;
    int regions = 0;
    {
        // 塊の数（表示用）
        std::vector<int> lab(ncell, -1);
        for (std::size_t s0 = 0; s0 < ncell; ++s0) {
            if (!region[s0] || lab[s0] >= 0) continue;
            queue.assign(1, static_cast<int>(s0));
            lab[s0] = regions;
            for (std::size_t qi = 0; qi < queue.size(); ++qi) {
                const int i = queue[qi];
                const int x = i % cw, y = i / cw;
                const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& q : nb) {
                    if (q[0] < 0 || q[0] >= cw || q[1] < 0 || q[1] >= ch) continue;
                    const int j = q[1] * cw + q[0];
                    if (region[j] && lab[j] < 0) {
                        lab[j] = regions;
                        queue.push_back(j);
                    }
                }
            }
            ++regions;
        }
    }
    for (std::size_t i = 0; i < ncell; ++i) {
        if (!region[i]) continue;
        int an = ref_o;
        while (an > 0 && sat[an][i]) --an;
        anchor[i] = static_cast<int8_t>(an);
        bool any = false;
        for (int o = 0; o < n; ++o) {
            if (o == an || sat[o][i]) continue;
            const double a = mean[an][i], c = mean[o][i];
            const double d = std::fabs(a - c);
            if (d > k * std::sqrt(static_cast<double>(var[an][i]) + var[o][i]) && d > tol * std::max(std::fabs(a), std::fabs(c))) {
                deny[o][i] = 1.0f;
                any = true;
            }
        }
        if (any) ++restricted;
    }
    res.ghost_regions = regions;
    res.ghost_mask.assign(static_cast<std::size_t>(gw) * gh, 0.0f);
    res.ghost_frame.assign(res.ghost_mask.size(), -1);
    if (restricted == 0) return;
    // 外す判定をなだらかに: 1 区画広げてから 3×3 でならす（区画ごとに使うフレームが入れ替わって、ノイズの
    // まだらになるのを防ぐ）。
    for (int o = 0; o < n; ++o) {
        std::vector<float>& d = deny[o];
        std::vector<float> g(ncell, 0.0f);
        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                float m = 0.0f;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx >= 0 && xx < cw && yy >= 0 && yy < ch) m = std::max(m, d[static_cast<std::size_t>(yy) * cw + xx]);
                    }
                }
                g[static_cast<std::size_t>(y) * cw + x] = region[static_cast<std::size_t>(y) * cw + x] ? m : 0.0f;
            }
        }
        box_blur(g, cw, ch, 1);
        d.swap(g);
    }
    const float ra = static_cast<float>(std::min(0.95, std::max(0.0, opt.ramp_start)));
    std::vector<float> fo(n), wnew(n);
    for (int by = 0; by < gh; ++by) {
        for (int bx = 0; bx < gw; ++bx) {
            const std::size_t bi = static_cast<std::size_t>(by) * gw + bx;
            // 区画の中心からの位置で、まわりの区画の値を双線形に補間する。
            const float fx = (bx + 0.5f) / G - 0.5f, fy = (by + 0.5f) / G - 0.5f;
            const int x0 = std::max(0, std::min(cw - 1, static_cast<int>(std::floor(fx))));
            const int y0 = std::max(0, std::min(ch - 1, static_cast<int>(std::floor(fy))));
            const int x1 = std::min(cw - 1, x0 + 1), y1 = std::min(ch - 1, y0 + 1);
            const float tx = std::min(1.0f, std::max(0.0f, fx - x0)), ty = std::min(1.0f, std::max(0.0f, fy - y0));
            float denied = 0.0f;
            for (int o = 0; o < n; ++o) {
                const std::vector<float>& d = deny[o];
                const float v = (1 - ty) * ((1 - tx) * d[static_cast<std::size_t>(y0) * cw + x0] + tx * d[static_cast<std::size_t>(y0) * cw + x1]) +
                                ty * ((1 - tx) * d[static_cast<std::size_t>(y1) * cw + x0] + tx * d[static_cast<std::size_t>(y1) * cw + x1]);
                fo[o] = 1.0f - std::min(1.0f, v);
                denied = std::max(denied, 1.0f - fo[o]);
            }
            if (denied <= 0.0f) continue;
            const int an = anchor[static_cast<std::size_t>(by / G) * cw + bx / G];
            if (an < 0) continue;  // 塊の外（なだらかにした裾）は手本が無いので変えない
            fo[an] = 1.0f;
            // 使ってよい度合い fo を掛けて、明るい方から重みを配り直す（合成の本体と同じ切り替え）。残りは手本へ。
            float remaining = 1.0f;
            for (int o = 0; o < n; ++o) wnew[o] = 0.0f;
            for (int o = n - 1; o >= 0; --o) {
                if (o == an) continue;
                float mx = 0.0f;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int yy = std::min(gh - 1, std::max(0, by + dy));
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = std::min(gw - 1, std::max(0, bx + dx));
                        mx = std::max(mx, level[o][static_cast<std::size_t>(yy) * gw + xx]);
                    }
                }
                const float t = std::min(1.0f, std::max(0.0f, (mx - ra) / (1.0f - ra)));
                const float hi = mx >= 1.0f ? 0.0f : 1.0f - t * t * (3.0f - 2.0f * t);
                // 手本より暗いフレームは、手本が受け持てる所では要らない（ノイズが多い）。
                const float use = o > an ? hi * fo[o] : 0.0f;
                wnew[o] = remaining * use;
                remaining -= wnew[o];
            }
            wnew[an] += remaining;
            res.ghost_mask[bi] = denied;
            res.ghost_frame[bi] = static_cast<int8_t>(an);
            for (int o = 0; o < n; ++o) {
                float& w = res.weights[o][bi];
                w = (1.0f - denied) * w + denied * wnew[o];
            }
        }
    }
    // 動いたとみなして、いずれかのフレームを外した区画の割合。
    res.ghost_fraction = static_cast<double>(restricted) / static_cast<double>(ncell);
}

}  // namespace

int auto_reference(const std::vector<RawFrame>& frames, const ExposurePlan& plan) {
    if (frames.empty() || plan.order.empty()) return -1;
    return plan.order[(plan.order.size() - 1) / 2];
}

MergeResult merge_frames(const std::vector<RawFrame>& frames, const ExposurePlan& plan, const MergeOptions& opt) {
    const int n = static_cast<int>(frames.size());
    if (n == 0) throw std::runtime_error("フレームがありません");
    const RawFrame& first = frames[0];
    for (const RawFrame& f : frames) {
        if (f.width != first.width || f.height != first.height) throw std::runtime_error("フレームの寸法が揃っていません");
        if (f.cfa.w != first.cfa.w || f.cfa.h != first.cfa.h || std::memcmp(f.cfa.color, first.cfa.color, sizeof(f.cfa.color)) != 0) {
            throw std::runtime_error("フレームのカラーフィルターの並びが揃っていません");
        }
    }

    MergeResult res;
    res.width = first.width;
    res.height = first.height;
    res.cfa = first.cfa;
    res.block = first.cfa.is_xtrans() ? 3 : 2;
    const int b = res.block;
    const int gw = (res.width + b - 1) / b, gh = (res.height + b - 1) / b;
    res.grid_w = gw;
    res.grid_h = gh;
    const std::size_t cells = static_cast<std::size_t>(gw) * gh;

    // ---- ブロックの明るさ ----
    // s = ブロックの中の各画素の「値 / 飽和の閾値」の最大（どの色でも）。s ≥ 1 なら飽和ブロック。
    // 1色だけ飽和した状態で使うと色かぶり（マゼンタのハイライトなど）になるので、色をまたいで最大を取る。
    std::vector<std::vector<float>> level(n);
    for (int o = 0; o < n; ++o) {
        const int fi = plan.order[o];
        const RawFrame& f = frames[fi];
        float inv[3];
        for (int c = 0; c < 3; ++c) inv[c] = static_cast<float>(1.0 / (plan.clip[fi].level[c] * opt.safety));
        std::vector<float>& s = level[o];
        s.assign(cells, 0.0f);
        parallel_for(gh, [&](int by0, int by1) {
            for (int by = by0; by < by1; ++by) {
                float* srow = s.data() + static_cast<std::size_t>(by) * gw;
                for (int y = by * b; y < std::min(res.height, (by + 1) * b); ++y) {
                    const float* row = f.data.data() + static_cast<std::size_t>(y) * f.width;
                    for (int x = 0; x < res.width; ++x) {
                        const float v = row[x] * inv[f.cfa.at(x, y)];
                        float& d = srow[x / b];
                        if (v > d) d = v;
                    }
                }
            }
        });
    }
    {
        std::size_t clipped = 0;
        for (float v : level[0]) clipped += v >= 1.0f ? 1 : 0;
        res.clipped_fraction = static_cast<double>(clipped) / static_cast<double>(cells);
    }

    // ---- 重み ----
    // 各フレームの重み h は「そのブロック自身の明るさ」で決める。s が ramp_start 以下なら 1、
    // 飽和の閾値（s = 1）に近づくにつれ滑らかに 0 へ下げる。明るいフレームから順に、残りの重みを配る:
    //   w[N-1] = h[N-1]、w[k] = (1 − Σ_{j>k} w[j]) · h[k]、最も暗いフレームは残り全部。
    //
    // 明るさで切り替えるので、なだらかなグラデーションでは切り替わりも空間的に滑らかになり、
    // くっきりした輪郭（月・光源）では輪郭そのものの位置で切り替わる（段差は輪郭に隠れる）。
    // 空間的な余白で切り替えると、小さな明るい物の周りが最も暗い（ノイズの多い）フレームで
    // 埋まってしまうので、余白は隣の1ブロック（にじみ・わずかなずれの分）だけにとどめる。
    //
    // 飽和ブロックとその隣では h = 0 を最後に掛け直すので、飽和した画素に重みが漏れることはない。
    const float a = static_cast<float>(std::min(0.95, std::max(0.0, opt.ramp_start)));
    const int r = std::max(1, opt.feather_px / (4 * b));
    res.weights.assign(n, std::vector<float>());
    std::vector<float> remaining(cells, 1.0f);
    std::vector<float> dil(cells), h(cells), hb;
    for (int o = n - 1; o >= 1; --o) {
        const std::vector<float>& s = level[o];
        // 隣の1ブロックまで含めた最大（にじみ・わずかなずれへの余裕）。
        parallel_for(gh, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < gw; ++x) {
                    float m = 0.0f;
                    for (int dy = -1; dy <= 1; ++dy) {
                        const int yy = std::min(gh - 1, std::max(0, y + dy));
                        const float* row = s.data() + static_cast<std::size_t>(yy) * gw;
                        for (int dx = -1; dx <= 1; ++dx) m = std::max(m, row[std::min(gw - 1, std::max(0, x + dx))]);
                    }
                    dil[static_cast<std::size_t>(y) * gw + x] = m;
                }
            }
        });
        for (std::size_t i = 0; i < cells; ++i) {
            const float t = std::min(1.0f, std::max(0.0f, (dil[i] - a) / (1.0f - a)));
            h[i] = 1.0f - t * t * (3.0f - 2.0f * t);
        }
        // 雑音で重みがちらつかないよう軽くぼかす。ただし、ぼかしで重みを「下げる」ことはしない
        // （飽和ブロックの 0 が周りへ広がると、余裕のある明るいフレームを使える所まで暗いフレームに
        // 回ってしまう）。飽和ブロック（とその隣）は最後に 0 に戻す。
        hb = h;
        box_blur(hb, gw, gh, r);
        box_blur(hb, gw, gh, r);
        std::vector<float>& w = res.weights[o];
        w.resize(cells);
        for (std::size_t i = 0; i < cells; ++i) {
            const float hi = dil[i] >= 1.0f ? 0.0f : std::min(1.0f, std::max(h[i], hb[i]));
            w[i] = remaining[i] * hi;
            remaining[i] -= w[i];
        }
    }
    res.weights[0] = remaining;
    res.reference = opt.reference >= 0 && opt.reference < n ? opt.reference : auto_reference(frames, plan);
    int ref_o = 0;
    for (int o = 0; o < n; ++o) {
        if (plan.order[o] == res.reference) ref_o = o;
    }
    if (opt.deghost && n >= 2) deghost(res, frames, plan, level, opt, ref_o);

    // ---- 合成 ----
    // 出力 = Σ w·v / (相対露光量 · 最も暗いフレームの飽和レベル)。
    const int darkest = plan.order[0];
    const float l0 = std::min({plan.clip[darkest].level[0], plan.clip[darkest].level[1], plan.clip[darkest].level[2]});
    if (!(l0 > 0.0f)) throw std::runtime_error("飽和レベルが分かりません");
    std::vector<float> scale(n);
    for (int o = 0; o < n; ++o) scale[o] = static_cast<float>(1.0 / (plan.rel_exposure[o] * l0));
    res.data.assign(static_cast<std::size_t>(res.width) * res.height, 0.0f);
    parallel_for(res.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* dst = res.data.data() + static_cast<std::size_t>(y) * res.width;
            const std::size_t grow = static_cast<std::size_t>(y / b) * gw;
            for (int o = 0; o < n; ++o) {
                const float* src = frames[plan.order[o]].data.data() + static_cast<std::size_t>(y) * res.width;
                const float* w = res.weights[o].data() + grow;
                const float s = scale[o];
                for (int x = 0; x < res.width; ++x) {
                    const float wx = w[x / b];
                    if (wx != 0.0f) dst[x] += wx * src[x] * s;
                }
            }
            // 上は最も暗いフレームの飽和点（1.0）で切る。下は切らない: 暗い所のノイズは黒より下にも振れるので、
            // 0 で切ると平均が持ち上がり、信号の弱い色ほど持ち上がって色がかぶる（灯台の野原で確認）。
            for (int x = 0; x < res.width; ++x) dst[x] = std::min(1.0f, dst[x]);
        }
    });

    // ---- 基準フレームとの明るさの関係 ----
    const float lref = plan.clip[res.reference].level[1];
    // 全体の明るさを基準フレームに直接合わせる。隣どうしの露出比は「切り替わる明るさ」で測っているので
    // 継ぎ目には正しいが、つなぐと誤差が積み重なり、基準フレームとの関係が数 % ずれることがある
    // （柱状節理の 7 枚で、1/10 秒と 3.2 秒の比が直接 33.65、つないで 34.95）。
    // 基準フレームがよく写っている画素（飽和レベルの 5〜85%）が十分あれば、そこで合成の値と比べて補正する。
    double eref = plan.rel_exposure[ref_o];
    {
        const RawFrame& rf = frames[res.reference];
        std::vector<float> ratios;
        const double inv = 1.0 / (eref * l0);
        for (int y = 0; y < res.height; y += 3) {
            for (int x = 0; x < res.width; x += 3) {
                const int c = rf.cfa.at(x, y);
                const float v = rf.value(x, y);
                const float lv = plan.clip[res.reference].level[c];
                if (v < 0.05f * lv || v > 0.85f * lv) continue;
                const float o = res.data[static_cast<std::size_t>(y) * res.width + x];
                if (o > 0.0f) ratios.push_back(static_cast<float>(o / (v * inv)));
            }
        }
        res.anchor_samples = static_cast<int>(ratios.size());
        if (ratios.size() >= 5000) {
            std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
            const double k = ratios[ratios.size() / 2];
            if (k > 0.5 && k < 2.0) {
                res.anchor_correction = k;
                eref /= k;
            }
        }
    }
    res.white_scale = eref * l0 / lref;
    res.reference_rel_exposure = eref;
    // 最も明るいフレーム（シャドウ・中間調はこれから来る）のノイズ。NoiseProfile と色補間に使う。
    res.brightest_rel_exposure = plan.rel_exposure[n - 1];
    res.brightest_noise_dn = estimate_noise(frames[plan.order[n - 1]], plan.clip[plan.order[n - 1]]);
    res.darkest_clip = l0;
    res.reference_ev_offset = std::log2(res.white_scale);
    return res;
}

}  // namespace hdr
