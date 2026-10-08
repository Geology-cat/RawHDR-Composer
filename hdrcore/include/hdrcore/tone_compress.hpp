#pragma once

// 明暗差の圧縮（任意）。合成したデータに、輪郭に沿った滑らかな「明るさの倍率」を掛けて、
// Lightroom の全体のスライダー（シャドウ・ハイライト）だけで仕上げられる幅に収める。
// 暗室の覆い焼き・焼き込みにあたる処理を、データに焼き込む。
//
// - 中間調（基準フレームの白から shadow_knee 段〜highlight_knee 段）は倍率 1 のまま。
//   BaselineExposure・明るさの基準合わせ・Adobe のテンプレートの明るさはそのまま使える
// - それより明るい所は、最も明るい所が highlight_top 段に来るよう滑らかに抑える（月・灯室）
// - それより暗い所は、shadow_floor 段より暗くならないよう滑らかに持ち上げる（岩・野原）
// - 倍率は CFA の 1 ブロックに 1 つで、全色に同じ値を掛ける（ホワイトバランス・色は変わらない）
// - 「大まかな明るさ」はバイラテラルグリッドで求めるので、月の縁や崖の輪郭で倍率が切り替わり、
//   にじみ（ハロー）が出にくい。細かい模様（大まかな明るさとの差）はそのまま残る
//
// 純粋な線形 HDR からは外れる（局所的な明るさの関係が変わる）。strength = 0 なら何もしない。

#include <vector>

#include "hdrcore/merge.hpp"

namespace hdr {

struct ToneCompressOptions {
    // 0〜1。0 = 何もしない、0.5 = 下の目標どおりに縮める、1 = 目標をさらに厳しくする
    // （明るい所の knee −0.5 段・top −1 段、暗い所の knee +0.5 段・floor +2 段）。
    double strength = 0.0;
    // 強さを場面の明暗差（大まかな明るさの幅）から自動で決める（strength は使わない）。
    // 幅 8 段で 15%、14 段で 50%、22 段以上で 60%（その間は直線）。決めた値は結果の strength に入る。
    bool auto_strength = false;
    // 明るい所を抑える側・暗い所を持ち上げる側の強さの倍率（0〜2、1 = strength のまま）。
    // 片側だけを強める・弱めるのに使う。どちらも 1 なら以前と同じ結果。
    double highlight_amount = 1.0;
    double shadow_amount = 1.0;
    // 白からの段。これより明るい所を縮める。Lightroom の標準のトーンカーブは白の 1 段下あたりから上を強く寝かせる
    // ので、最も明るい所（月の平均）はそれより下に置き、圧縮に使う幅も広めにとる（月と周りのにじみの差が残るように）。
    double highlight_knee = -3.0;
    double highlight_top = -1.0;   // 最も明るい所の行き先
    double shadow_knee = -4.0;     // これより暗い所を持ち上げる
    double shadow_floor = -8.0;    // 最も暗い所（下から 0.5%）の行き先
    // 輪郭とみなす明るさの差（段）。これより小さい濃淡（月の海と高地など）は模様として残り、縮められない。
    // 月と空（約 10 段）・崖と空（4〜5 段）の境は輪郭として扱われ、そこで倍率が切り替わる。
    double range_sigma = 2.0;
    // 開いたときの明るさを自動で整える: 大まかな明るさの中央値が、白から target_median 段に来るよう
    // BaselineExposure を変える（基準フレームが場面にとって暗い・明るいときに、露光量を動かさずに済む）。
    // 上の knee・top・floor は、この整えた後の白に対する段。
    bool auto_brightness = true;
    double target_median = -3.0;
    double max_brightness_change = 5.0;  // 整える量の上限（段）
};

struct ToneCompressResult {
    bool applied = false;
    double min_gain = 1.0, max_gain = 1.0;  // 掛けた倍率の範囲
    double before_span = 0.0, after_span = 0.0;  // 大まかな明るさの幅（段）
    double opening_ev = 0.0;  // 開いたときの明るさを整えた量（段。BaselineExposure に足す）
    double strength = 0.0;    // 使った強さ（自動のときは決めた値）
};

// 自動のときの強さ（大まかな明るさの幅、段から）。
double auto_tone_strength(double span_ev);

// m.data に倍率を掛ける。倍率（ブロック単位）は m.gain に残す（色補間のノイズの見積もりに使う）。
ToneCompressResult compress_tone(MergeResult& m, const double neutral[3], const ToneCompressOptions& options);

}  // namespace hdr
