#include "hdrcore/align.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "hdrcore/demosaic.hpp"
#include "hdrcore/noise.hpp"
#include "hdrcore/parallel.hpp"

namespace hdr {

namespace {

// 位置合わせに使う縮小画像。値は露出をそろえた明るさの対数、valid = 使ってよい画素か。
struct Proxy {
    int w = 0, h = 0;
    std::vector<float> v;
    std::vector<uint8_t> valid;
};

// ブロック（CFA の周期の大きさ）ごとの明るさの和を、相対露光量で割って対数にする。
// 飽和に近いブロックと、暗すぎる（ノイズに埋もれる）ブロックは使わない。
Proxy make_proxy(const RawFrame& f, const ClipLevels& clip, double rel_exposure, double dark_floor) {
    Proxy p;
    const int b = f.cfa.is_xtrans() ? 3 : 2;
    p.w = f.width / b;
    p.h = f.height / b;
    p.v.assign(static_cast<std::size_t>(p.w) * p.h, 0.0f);
    p.valid.assign(p.v.size(), 0);
    const float sat[3] = {clip.level[0] * 0.9f, clip.level[1] * 0.9f, clip.level[2] * 0.9f};
    parallel_for(p.h, [&](int y0, int y1) {
        for (int by = y0; by < y1; ++by) {
            for (int bx = 0; bx < p.w; ++bx) {
                double sum = 0.0;
                bool ok = true;
                for (int y = by * b; y < (by + 1) * b; ++y) {
                    for (int x = bx * b; x < (bx + 1) * b; ++x) {
                        const float v = f.value(x, y);
                        if (v >= sat[f.cfa.at(x, y)]) ok = false;
                        sum += v;
                    }
                }
                const std::size_t i = static_cast<std::size_t>(by) * p.w + bx;
                const double n = sum / (b * b);
                if (ok && n > dark_floor) {
                    p.v[i] = static_cast<float>(std::log(n / rel_exposure));
                    p.valid[i] = 1;
                }
            }
        }
    });
    return p;
}

Proxy downsample(const Proxy& s) {
    Proxy d;
    d.w = s.w / 2;
    d.h = s.h / 2;
    d.v.assign(static_cast<std::size_t>(d.w) * d.h, 0.0f);
    d.valid.assign(d.v.size(), 0);
    for (int y = 0; y < d.h; ++y) {
        for (int x = 0; x < d.w; ++x) {
            double sum = 0.0;
            int n = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const std::size_t i = static_cast<std::size_t>(2 * y + dy) * s.w + (2 * x + dx);
                    if (s.valid[i]) {
                        sum += s.v[i];
                        ++n;
                    }
                }
            }
            if (n == 4) {
                const std::size_t i = static_cast<std::size_t>(y) * d.w + x;
                d.v[i] = static_cast<float>(sum / 4.0);
                d.valid[i] = 1;
            }
        }
    }
    return d;
}

// a(x, y) と b(x + dx, y + dy) の違い（平均の差を引いた二乗平均）。両方で有効な画素の割合も返す。
double mismatch(const Proxy& a, const Proxy& b, int dx, int dy, double& overlap) {
    double s = 0.0, ss = 0.0;
    long n = 0;
    const int x0 = std::max(0, -dx), x1 = std::min(a.w, b.w - dx);
    const int y0 = std::max(0, -dy), y1 = std::min(a.h, b.h - dy);
    for (int y = y0; y < y1; ++y) {
        const std::size_t ra = static_cast<std::size_t>(y) * a.w, rb = static_cast<std::size_t>(y + dy) * b.w;
        for (int x = x0; x < x1; ++x) {
            if (!a.valid[ra + x] || !b.valid[rb + x + dx]) continue;
            const double d = a.v[ra + x] - b.v[rb + x + dx];
            s += d;
            ss += d * d;
            ++n;
        }
    }
    overlap = static_cast<double>(n) / std::max<long>(1, static_cast<long>(a.w) * a.h);
    if (n < 64) return std::numeric_limits<double>::infinity();
    const double mean = s / n;
    return ss / n - mean * mean;
}

// 2枚のずれ（ブロック単位）。粗い段から順に、前の段の答えの周りだけを探す。
FrameShift match_pair(const Proxy& a, const Proxy& b, int max_shift_blocks) {
    std::vector<Proxy> pa = {a}, pb = {b};
    while (pa.back().w > 320 && pa.back().h > 200) {
        pa.push_back(downsample(pa.back()));
        pb.push_back(downsample(pb.back()));
    }
    int dx = 0, dy = 0;
    double best = 0.0, overlap = 0.0;
    for (int lv = static_cast<int>(pa.size()) - 1; lv >= 0; --lv) {
        const bool coarsest = lv == static_cast<int>(pa.size()) - 1;
        const int r = coarsest ? std::max(2, (max_shift_blocks >> lv) + 1) : 1;
        const int cx = coarsest ? 0 : dx * 2, cy = coarsest ? 0 : dy * 2;
        best = std::numeric_limits<double>::infinity();
        // 候補ごとの計算は独立なので並列に。
        const int side = 2 * r + 1;
        std::vector<double> scores(static_cast<std::size_t>(side) * side), overlaps(scores.size());
        parallel_for(side * side, [&](int i0, int i1) {
            for (int i = i0; i < i1; ++i) {
                const int ox = cx + (i % side) - r, oy = cy + (i / side) - r;
                scores[i] = mismatch(pa[lv], pb[lv], ox, oy, overlaps[i]);
            }
        }, 1);
        for (int i = 0; i < side * side; ++i) {
            // 同じくらいなら移動の少ない方（0 に近い方）を選ぶ。
            const int ox = cx + (i % side) - r, oy = cy + (i / side) - r;
            const double penalty = 1e-9 * (ox * ox + oy * oy);
            if (scores[i] + penalty < best) {
                best = scores[i] + penalty;
                dx = ox;
                dy = oy;
                overlap = overlaps[i];
            }
        }
    }
    FrameShift s;
    s.dx = dx;
    s.dy = dy;
    s.score = best;
    s.reliable = std::isfinite(best) && overlap > 0.05;
    return s;
}

// ---- 相似変換（中心からの位置 q に [a −b; b a]·q + t） ----
struct Sim {
    double a = 1.0, b = 0.0, tx = 0.0, ty = 0.0;
};

Sim compose(const Sim& t, const Sim& s) {  // t(s(q))
    Sim r;
    r.a = t.a * s.a - t.b * s.b;
    r.b = t.b * s.a + t.a * s.b;
    r.tx = t.a * s.tx - t.b * s.ty + t.tx;
    r.ty = t.b * s.tx + t.a * s.ty + t.ty;
    return r;
}

Sim inverse(const Sim& t) {
    const double d = t.a * t.a + t.b * t.b;
    Sim r;
    r.a = t.a / d;
    r.b = -t.b / d;
    r.tx = -(r.a * t.tx - r.b * t.ty);
    r.ty = -(r.b * t.tx + r.a * t.ty);
    return r;
}

// 値と勾配を双線形に読む（4 点とも有効なときだけ）。
bool sample(const Proxy& p, const std::vector<float>& gx, const std::vector<float>& gy, double x, double y, double& v, double& dx,
            double& dy) {
    if (!(x >= 1.0 && y >= 1.0 && x < p.w - 2 && y < p.h - 2)) return false;
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const double fx = x - x0, fy = y - y0;
    const std::size_t i = static_cast<std::size_t>(y0) * p.w + x0;
    const std::size_t idx[4] = {i, i + 1, i + p.w, i + p.w + 1};
    for (std::size_t k : idx) {
        if (!p.valid[k]) return false;
    }
    const double w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    v = dx = dy = 0.0;
    for (int k = 0; k < 4; ++k) {
        v += w[k] * p.v[idx[k]];
        dx += w[k] * gx[idx[k]];
        dy += w[k] * gy[idx[k]];
    }
    return true;
}

void gradients(const Proxy& p, std::vector<float>& gx, std::vector<float>& gy) {
    gx.assign(p.v.size(), 0.0f);
    gy.assign(p.v.size(), 0.0f);
    for (int y = 1; y + 1 < p.h; ++y) {
        for (int x = 1; x + 1 < p.w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * p.w + x;
            if (p.valid[i - 1] && p.valid[i + 1]) gx[i] = 0.5f * (p.v[i + 1] - p.v[i - 1]);
            if (p.valid[i - p.w] && p.valid[i + p.w]) gy[i] = 0.5f * (p.v[i + p.w] - p.v[i - p.w]);
        }
    }
}

// 変換 t での食い違い（最も細かい段。明るさの違い c + k·e^(−(A−m)) は当てはめ直す）。外れに強い Huber の平均。
double pair_cost(const Proxy& A, const Proxy& B, const std::vector<float>& gx, const std::vector<float>& gy, const Sim& t) {
    const double ca = (A.w - 1) * 0.5, cha = (A.h - 1) * 0.5;
    const double cb = (B.w - 1) * 0.5, chb = (B.h - 1) * 0.5;
    const int stride = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(A.w) * A.h / 250000.0)));
    std::vector<float> rv, ev;
    double m = 0.0;
    for (int y = 0; y < A.h; y += stride) {
        for (int x = 0; x < A.w; x += stride) {
            const std::size_t i = static_cast<std::size_t>(y) * A.w + x;
            if (!A.valid[i]) continue;
            const double qx = x - ca, qy = y - cha;
            double v, dx, dy;
            if (!sample(B, gx, gy, cb + t.a * qx - t.b * qy + t.tx, chb + t.b * qx + t.a * qy + t.ty, v, dx, dy)) continue;
            rv.push_back(static_cast<float>(v - A.v[i]));
            ev.push_back(A.v[i]);
            m += A.v[i];
        }
    }
    if (rv.size() < 500) return std::numeric_limits<double>::infinity();
    m /= rv.size();
    for (float& e : ev) e = static_cast<float>(std::exp(std::min(20.0, -(e - m))));
    double c = 0.0, k = 0.0, cost = 0.0;
    for (int iter = 0; iter < 4; ++iter) {
        double s11 = 0, s12 = 0, s22 = 0, b1 = 0, b2 = 0;
        cost = 0.0;
        for (std::size_t i = 0; i < rv.size(); ++i) {
            const double r = rv[i] - c - k * ev[i];
            const double ar = std::fabs(r);
            const double w = ar < 0.1 ? 1.0 : 0.1 / ar;
            cost += ar < 0.1 ? 0.5 * r * r : 0.1 * (ar - 0.05);
            s11 += w;
            s12 += w * ev[i];
            s22 += w * ev[i] * ev[i];
            b1 += w * rv[i];
            b2 += w * rv[i] * ev[i];
        }
        const double det = s11 * s22 - s12 * s12;
        if (std::fabs(det) < 1e-12) break;
        c = (b1 * s22 - b2 * s12) / det;
        k = (s11 * b2 - s12 * b1) / det;
    }
    return cost / rv.size();
}

// B(T(q)) ≈ A(q) + c となる相似変換 T を、ガウス・ニュートン法で粗い段から詰める（縮小画像の単位、中心から）。
// t0 は平行移動の探索の答え（縮小画像の最も細かい段の単位）。収束しなければ false。
bool refine_pair(const Proxy& a, const Proxy& b, double t0x, double t0y, Sim& out) {
    std::vector<Proxy> pa = {a}, pb = {b};
    while (pa.back().w > 400 && pa.back().h > 260) {
        pa.push_back(downsample(pa.back()));
        pb.push_back(downsample(pb.back()));
    }
    const int levels = static_cast<int>(pa.size());
    Sim t;
    t.tx = t0x / std::ldexp(1.0, levels - 1);
    t.ty = t0y / std::ldexp(1.0, levels - 1);
    double c = 0.0, k = 0.0;
    bool ok = true;
    for (int lv = levels - 1; lv >= 0; --lv) {
        const Proxy& A = pa[lv];
        const Proxy& B = pb[lv];
        std::vector<float> gx, gy;
        gradients(B, gx, gy);
        const double ca = (A.w - 1) * 0.5, cha = (A.h - 1) * 0.5;
        const double cb = (B.w - 1) * 0.5, chb = (B.h - 1) * 0.5;
        const double norm = 1.0 / std::max(A.w, A.h);  // 回転の未知数の大きさをそろえる
        const int stride = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(A.w) * A.h / 250000.0)));
        const int rows = (A.h + stride - 1) / stride;
        // 明るさの違いのモデル: 対数で c（露出比の誤差）＋ k·e^(−(A − m))（黒のわずかなずれ。暗い所ほど比が狂う）。
        // 黒のずれを入れないと、空のなだらかな明るさの変化と重なって、ずれ・拡大と取り違える（三脚の柱状節理で、
        // 0.05% の拡大と 0.5〜0.8 画素のずれが出た）。
        double m = 0.0;
        {
            long cnt = 0;
            for (std::size_t i = 0; i < A.v.size(); i += 7) {
                if (A.valid[i]) {
                    m += A.v[i];
                    ++cnt;
                }
            }
            m = cnt ? m / cnt : 0.0;
        }
        for (int iter = 0; iter < 20; ++iter) {
            // 正規方程式（未知数: da, db, dtx, dty, dc）を並列に集める。
            struct Acc {
                double h[6][6] = {};
                double g[6] = {};
                long n = 0;
            };
            std::vector<Acc> part(static_cast<std::size_t>(std::max(1, rows)));
            parallel_for(rows, [&](int r0, int r1) {
                for (int r = r0; r < r1; ++r) {
                    Acc& acc = part[r];
                    const int y = r * stride;
                    for (int x = 0; x < A.w; x += stride) {
                        const std::size_t i = static_cast<std::size_t>(y) * A.w + x;
                        if (!A.valid[i]) continue;
                        const double qx = x - ca, qy = y - cha;
                        const double sx = cb + t.a * qx - t.b * qy + t.tx, sy = chb + t.b * qx + t.a * qy + t.ty;
                        double v, dx, dy;
                        if (!sample(B, gx, gy, sx, sy, v, dx, dy)) continue;
                        const double e = std::exp(std::min(20.0, -(A.v[i] - m)));
                        const double res = v - A.v[i] - c - k * e;
                        // 動いた物・光の変化は残差が大きいので重みを下げる（Huber）。
                        const double ar = std::fabs(res);
                        const double w = ar < 0.1 ? 1.0 : 0.1 / ar;
                        const double J[6] = {(dx * qx + dy * qy) * norm, (-dx * qy + dy * qx) * norm, dx, dy, -1.0, -e};
                        for (int p = 0; p < 6; ++p) {
                            acc.g[p] += w * J[p] * res;
                            for (int q = 0; q <= p; ++q) acc.h[p][q] += w * J[p] * J[q];
                        }
                        ++acc.n;
                    }
                }
            }, 8);
            Acc sum;
            for (const Acc& a2 : part) {
                for (int p = 0; p < 6; ++p) {
                    sum.g[p] += a2.g[p];
                    for (int q = 0; q <= p; ++q) sum.h[p][q] += a2.h[p][q];
                }
                sum.n += a2.n;
            }
            if (sum.n < 500) {
                ok = false;
                break;
            }
            double H[6][7];
            for (int p = 0; p < 6; ++p) {
                for (int q = 0; q < 6; ++q) H[p][q] = q <= p ? sum.h[p][q] : sum.h[q][p];
                H[p][p] = H[p][p] * (1.0 + 1e-6) + 1e-9;
                H[p][6] = -sum.g[p];
            }
            // ガウスの消去法
            bool singular = false;
            for (int p = 0; p < 6 && !singular; ++p) {
                int piv = p;
                for (int r = p + 1; r < 6; ++r) {
                    if (std::fabs(H[r][p]) > std::fabs(H[piv][p])) piv = r;
                }
                if (std::fabs(H[piv][p]) < 1e-12) {
                    singular = true;
                    break;
                }
                for (int q = 0; q < 7; ++q) std::swap(H[p][q], H[piv][q]);
                for (int r = 0; r < 6; ++r) {
                    if (r == p) continue;
                    const double f = H[r][p] / H[p][p];
                    for (int q = p; q < 7; ++q) H[r][q] -= f * H[p][q];
                }
            }
            if (singular) {
                ok = false;
                break;
            }
            double d[6];
            for (int p = 0; p < 6; ++p) d[p] = H[p][6] / H[p][p];
            t.a += d[0] * norm;
            t.b += d[1] * norm;
            t.tx += d[2];
            t.ty += d[3];
            c += d[4];
            k += d[5];
            if (std::fabs(d[2]) < 1e-3 && std::fabs(d[3]) < 1e-3 && std::fabs(d[0]) < 1e-3 && std::fabs(d[1]) < 1e-3) break;
        }
        if (!ok) break;
        if (lv > 0) {
            t.tx *= 2.0;
            t.ty *= 2.0;
        }
    }
    // ありえない答え（大きな回転・拡大）は捨てる。
    if (!ok || std::fabs(t.a - 1.0) > 0.05 || std::fabs(t.b) > 0.1) t = Sim();
    // 候補（ずれ 0・平行移動の探索の答え・詰めた答え）の食い違いを、最も細かい段で比べる。詰めた答えは
    // はっきり（3% 以上）良いときだけ、平行移動はずれ 0 よりはっきり良いときだけ使う。三脚の写真で、
    // 波・光の変化などに引かれて、ありもしない小さなずれ・拡大を答えにしないため。
    std::vector<float> gx, gy;
    gradients(b, gx, gy);
    Sim zero, shift;
    shift.tx = t0x;
    shift.ty = t0y;
    const double c0 = pair_cost(a, b, gx, gy, zero);
    const double c1 = (t0x == 0.0 && t0y == 0.0) ? c0 : pair_cost(a, b, gx, gy, shift);
    const double c2 = ok ? pair_cost(a, b, gx, gy, t) : std::numeric_limits<double>::infinity();
    Sim best = zero;
    double best_cost = c0;
    if (c1 < 0.97 * best_cost) {
        best = shift;
        best_cost = c1;
    }
    if (c2 < 0.97 * best_cost) best = t;
    out = best;
    return true;
}

int round_to_multiple(int v, int m) {
    return static_cast<int>(std::lround(static_cast<double>(v) / m)) * m;
}

}  // namespace

std::vector<FrameShift> estimate_shifts(const std::vector<RawFrame>& frames, const ExposurePlan& plan, int reference,
                                        const AlignOptions& opt) {
    const int n = static_cast<int>(frames.size());
    std::vector<FrameShift> out(n);
    if (n < 2 || reference < 0 || reference >= n) return out;
    const int block = frames[0].cfa.is_xtrans() ? 3 : 2;
    const int period = frames[0].cfa.is_xtrans() ? 6 : 2;
    // 縮小画像（並べた順に）。暗すぎるブロックの下限は、そのフレームの飽和レベルの 0.3%。
    std::vector<Proxy> proxies(n);
    for (int o = 0; o < n; ++o) {
        const int i = plan.order[o];
        const ClipLevels& c = plan.clip[i];
        proxies[o] = make_proxy(frames[i], c, plan.rel_exposure[o], 0.003 * c.level[1]);
    }
    // 隣どうしのずれ（order の順に、o と o+1）。
    std::vector<FrameShift> pair(n);
    const int max_blocks = std::max(1, opt.max_shift_px / block);
    for (int o = 0; o + 1 < n; ++o) {
        pair[o] = match_pair(proxies[o], proxies[o + 1], max_blocks);
        // 両方で有効な画素が足りない組（暗いフレームに光源しか写っていないなど）は当てにならないので、
        // 動かさない（三脚なら 0 が最も確からしい）。
        if (!pair[o].reliable) pair[o].dx = pair[o].dy = 0;
    }
    // 基準からたどってつなぐ。aligned_b(x) = b(x + d) で、d は「a に対する b のずれ」。
    int ref_o = 0;
    for (int o = 0; o < n; ++o) {
        if (plan.order[o] == reference) ref_o = o;
    }
    std::vector<int> bx(n, 0), by(n, 0);
    std::vector<bool> ok(n, true);
    for (int o = ref_o + 1; o < n; ++o) {
        bx[o] = bx[o - 1] + pair[o - 1].dx;
        by[o] = by[o - 1] + pair[o - 1].dy;
        ok[o] = ok[o - 1] && pair[o - 1].reliable;
    }
    for (int o = ref_o - 1; o >= 0; --o) {
        bx[o] = bx[o + 1] - pair[o].dx;
        by[o] = by[o + 1] - pair[o].dy;
        ok[o] = ok[o + 1] && pair[o].reliable;
    }
    for (int o = 0; o < n; ++o) {
        FrameShift& s = out[plan.order[o]];
        s.dx = round_to_multiple(bx[o] * block, period);
        s.dy = round_to_multiple(by[o] * block, period);
        s.warp[2] = s.dx;
        s.warp[3] = s.dy;
        s.reliable = ok[o];
        s.score = o == ref_o ? 0.0 : (o > ref_o ? pair[o - 1].score : pair[o].score);
    }
    out[reference] = FrameShift();
    if (!opt.subpixel) return out;

    // ---- 回転・1 画素未満の移動 ----
    // 隣どうしの相似変換を詰め、基準からつなぐ（画素の単位、画像の中心から）。
    // 縮小画像（ブロック単位）の中心からの位置を block 倍すると画素の中心からの位置になる。
    std::vector<Sim> pt(n);
    std::vector<bool> pok(n, false);
    for (int o = 0; o + 1 < n; ++o) {
        if (!pair[o].reliable) continue;
        Sim t;
        if (refine_pair(proxies[o], proxies[o + 1], pair[o].dx, pair[o].dy, t)) {
            t.tx *= block;
            t.ty *= block;
            pt[o] = t;
            pok[o] = true;
        }
    }
    std::vector<Sim> u(n);
    std::vector<bool> uok(n, true);
    for (int o = ref_o + 1; o < n; ++o) {
        u[o] = compose(pt[o - 1], u[o - 1]);
        uok[o] = uok[o - 1] && pok[o - 1];
    }
    for (int o = ref_o - 1; o >= 0; --o) {
        u[o] = compose(inverse(pt[o]), u[o + 1]);
        uok[o] = uok[o + 1] && pok[o];
    }
    const RawFrame& f0 = frames[0];
    const double hw = (f0.width - 1) * 0.5, hh = (f0.height - 1) * 0.5;
    for (int o = 0; o < n; ++o) {
        if (o == ref_o || !uok[o]) continue;
        FrameShift& s = out[plan.order[o]];
        const Sim& t = u[o];
        // 周期の倍数の平行移動だけにしたときとの違い（四隅で最大）。
        const int dx = round_to_multiple(static_cast<int>(std::lround(t.tx)), period);
        const int dy = round_to_multiple(static_cast<int>(std::lround(t.ty)), period);
        double worst = 0.0;
        for (double qx : {-hw, hw}) {
            for (double qy : {-hh, hh}) {
                const double ex = (t.a - 1.0) * qx - t.b * qy + t.tx - dx;
                const double ey = t.b * qx + (t.a - 1.0) * qy + t.ty - dy;
                worst = std::max(worst, std::hypot(ex, ey));
            }
        }
        s.dx = dx;
        s.dy = dy;
        s.warp[0] = t.a;
        s.warp[1] = t.b;
        s.warp[2] = t.tx;
        s.warp[3] = t.ty;
        s.angle_deg = std::atan2(t.b, t.a) * 180.0 / M_PI;
        s.needs_warp = worst > opt.warp_threshold_px;
        if (!s.needs_warp) {
            s.warp[0] = 1.0;
            s.warp[1] = 0.0;
            s.warp[2] = dx;
            s.warp[3] = dy;
        }
    }
    return out;
}

void apply_shift(RawFrame& f, int dx, int dy) {
    const int period = f.cfa.is_xtrans() ? 6 : 2;
    dx = round_to_multiple(dx, period);
    dy = round_to_multiple(dy, period);
    if (dx == f.shift_x && dy == f.shift_y && !f.warped) return;
    if (f.original.empty()) f.original = f.data;  // 元の画素は一度だけ取っておく
    f.shift_x = dx;
    f.shift_y = dy;
    f.warped = false;
    f.warp[0] = 1.0;
    f.warp[1] = 0.0;
    f.warp[2] = dx;
    f.warp[3] = dy;
    if (dx == 0 && dy == 0) {
        f.data = f.original;
        f.original.clear();
        f.original.shrink_to_fit();
        return;
    }
    const int W = f.width, H = f.height;
    const std::vector<float>& src = f.original;
    // 画像の外は、同じ色の一番近い画素で埋める（周期の倍数だけ内側に寄せる）。DefaultCrop で隠れる所。
    parallel_for(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            int sy = y + dy;
            while (sy < 0) sy += period;
            while (sy >= H) sy -= period;
            float* dst = f.data.data() + static_cast<std::size_t>(y) * W;
            const float* row = src.data() + static_cast<std::size_t>(sy) * W;
            for (int x = 0; x < W; ++x) {
                int sx = x + dx;
                while (sx < 0) sx += period;
                while (sx >= W) sx -= period;
                dst[x] = row[sx];
            }
        }
    });
}

void apply_warp(RawFrame& f, const double w[4], const ClipLevels& clip, double threshold_px) {
    const int period = f.cfa.is_xtrans() ? 6 : 2;
    const int W = f.width, H = f.height;
    const double cx = (W - 1) * 0.5, cy = (H - 1) * 0.5;
    const int dx = round_to_multiple(static_cast<int>(std::lround(w[2])), period);
    const int dy = round_to_multiple(static_cast<int>(std::lround(w[3])), period);
    double worst = 0.0;
    for (double qx : {-cx, cx}) {
        for (double qy : {-cy, cy}) {
            worst = std::max(worst, std::hypot((w[0] - 1.0) * qx - w[1] * qy + w[2] - dx, w[1] * qx + (w[0] - 1.0) * qy + w[3] - dy));
        }
    }
    if (worst <= threshold_px) {
        apply_shift(f, dx, dy);
        return;
    }
    if (f.warped && f.warp[0] == w[0] && f.warp[1] == w[1] && f.warp[2] == w[2] && f.warp[3] == w[3]) return;
    if (f.original.empty()) f.original = f.data;
    // 元の画素を色補間する（ノイズの見積もりも元の画素で）。
    std::swap(f.data, f.original);
    const NoiseModel noise = estimate_noise(f, clip);
    const std::vector<float> rgb = demosaic(f.data.data(), W, H, f.cfa, noise);
    // 飽和した画素（とその周り 2 画素）の印。動かした後もそこは飽和の値にする（補間で飽和の手前の値になると、
    // 合成が使える画素と取り違えて、色のついた縁が出る）。
    std::vector<uint8_t> sat(static_cast<std::size_t>(W) * H, 0);
    parallel_for(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < W; ++x) {
                const int c = f.cfa.at(x, y);
                if (clip.level[c] > 0.0f && f.data[static_cast<std::size_t>(y) * W + x] >= 0.98f * clip.level[c]) {
                    for (int yy = std::max(0, y - 2); yy <= std::min(H - 1, y + 2); ++yy) {
                        uint8_t* row = sat.data() + static_cast<std::size_t>(yy) * W;
                        for (int xx = std::max(0, x - 2); xx <= std::min(W - 1, x + 2); ++xx) row[xx] = 1;
                    }
                }
            }
        }
    });
    std::swap(f.data, f.original);
    float smax = 0.0f;
    for (int c = 0; c < 3; ++c) smax = std::max(smax, clip.level[c]);
    // Catmull-Rom の 3 次補間で、各画素の色（CFA の色）だけを読む。
    const auto cubic = [](double t, double k[4]) {
        const double t2 = t * t, t3 = t2 * t;
        k[0] = -0.5 * t3 + t2 - 0.5 * t;
        k[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
        k[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
        k[3] = 0.5 * t3 - 0.5 * t2;
    };
    parallel_for(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* dst = f.data.data() + static_cast<std::size_t>(y) * W;
            const double qy = y - cy;
            for (int x = 0; x < W; ++x) {
                const double qx = x - cx;
                double sx = cx + w[0] * qx - w[1] * qy + w[2];
                double sy = cy + w[1] * qx + w[0] * qy + w[3];
                sx = std::min(W - 3.001, std::max(1.0, sx));
                sy = std::min(H - 3.001, std::max(1.0, sy));
                const int ix = static_cast<int>(sx), iy = static_cast<int>(sy);
                const int c = f.cfa.at(x, y);
                const int nx = static_cast<int>(std::lround(sx)), ny = static_cast<int>(std::lround(sy));
                if (sat[static_cast<std::size_t>(ny) * W + nx]) {
                    dst[x] = clip.level[c] > 0.0f ? clip.level[c] : smax;
                    continue;
                }
                double kx[4], ky[4];
                cubic(sx - ix, kx);
                cubic(sy - iy, ky);
                double v = 0.0;
                for (int j = 0; j < 4; ++j) {
                    const float* row = rgb.data() + (static_cast<std::size_t>(iy - 1 + j) * W) * 3;
                    double h = 0.0;
                    for (int i = 0; i < 4; ++i) h += kx[i] * row[(ix - 1 + i) * 3 + c];
                    v += ky[j] * h;
                }
                dst[x] = static_cast<float>(v);
            }
        }
    });
    f.warped = true;
    for (int k = 0; k < 4; ++k) f.warp[k] = w[k];
    f.shift_x = dx;
    f.shift_y = dy;
}

void apply_frame_shift(RawFrame& f, const FrameShift& s, const ClipLevels& clip) {
    if (s.needs_warp) {
        apply_warp(f, s.warp, clip, 0.0);
    } else {
        apply_shift(f, s.dx, s.dy);
    }
}

void valid_area(const std::vector<RawFrame>& frames, int& x0, int& y0, int& x1, int& y1) {
    x0 = y0 = 0;
    x1 = frames.empty() ? 0 : frames[0].width;
    y1 = frames.empty() ? 0 : frames[0].height;
    for (const RawFrame& f : frames) {
        if (f.warped) {
            // 変換した元の位置が画像の内側（3 次補間の余白 2 画素）に収まる範囲。四隅で調べる（回転は小さい）。
            const double cx = (f.width - 1) * 0.5, cy = (f.height - 1) * 0.5;
            const double a = f.warp[0], b = f.warp[1], tx = f.warp[2], ty = f.warp[3];
            for (double qy : {-cy, cy}) {
                // 左の縁: cx + a·qx − b·qy + tx ≥ 2 → qx ≥ (2 − cx + b·qy − tx) / a
                x0 = std::max(x0, static_cast<int>(std::ceil(cx + (2.0 - cx + b * qy - tx) / a)));
                x1 = std::min(x1, static_cast<int>(std::floor(cx + (f.width - 3.0 - cx + b * qy - tx) / a)));
            }
            for (double qx : {-cx, cx}) {
                y0 = std::max(y0, static_cast<int>(std::ceil(cy + (2.0 - cy - b * qx - ty) / a)));
                y1 = std::min(y1, static_cast<int>(std::floor(cy + (f.height - 3.0 - cy - b * qx - ty) / a)));
            }
            continue;
        }
        // aligned(x) = original(x + dx) が有効なのは 0 ≤ x + dx < W のとき。
        x0 = std::max(x0, -f.shift_x);
        y0 = std::max(y0, -f.shift_y);
        x1 = std::min(x1, f.width - f.shift_x);
        y1 = std::min(y1, f.height - f.shift_y);
    }
}

}  // namespace hdr
