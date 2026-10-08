#pragma once

// 飽和レベルの検出と、フレーム間の露出比の推定。
//
// 継ぎ目の段差のいちばんの原因は「露出比の誤差」なので、EXIF の名目値（シャッター速度・
// 絞り・ISO）ではなく、隣り合うフレームで両方とも有効な画素から実測する（開発計画書 §5.3）。
// 飽和レベルも機種表の値ではなくデータから測る。6D Mark II では ISO によって実際の飽和値が
// 機種表の白（12510）より低かったり（ISO 640: 11319）高かったり（ISO 4000: 14336）する。

#include <string>
#include <vector>

#include "hdrcore/raw_frame.hpp"

namespace hdr {

// フレーム1枚の飽和レベル（黒を引いた後の DN、色ごと）。
struct ClipLevels {
    float level[3] = {};
    bool detected[3] = {};  // true = データの中に飽和の山が見つかった
};

ClipLevels detect_clip_levels(const RawFrame& frame);

// 明るさの帯ごとの露出比（非線形性の診断用）。
struct RatioBin {
    double lo = 0.0, hi = 0.0;  // 明るいフレームの値の範囲（飽和レベルに対する比）
    int samples = 0;
    double ratio = 0.0;         // この帯での 明るい/暗い の中央値
};

// 隣り合う2枚（暗い方 dark・明るい方 bright）の比較結果。
struct PairFit {
    int dark = -1, bright = -1;   // 入力の番号
    double nominal_ratio = 0.0;   // EXIF から求めた比
    double ratio = 0.0;           // 実測の比（明るい / 暗い）
    double ratio_channel[3] = {}; // 色ごとの実測の比
    double offset = 0.0;          // 暗い方の黒のずれ（DN）。0 に近いほど黒が正しい
    int samples = 0;
    bool measured = false;        // false = 有効な画素が足りず、名目値を使った
    std::vector<RatioBin> bins;
    // 全体の最適化（ExposureOptions::global_span）で決めた比。measured の比との違いが、つなぎ目に残るずれ。
    double solved_ratio = 0.0;
};

struct ExposureOptions {
    // 露出比を測るのに使う明るいフレームの値の上限（飽和レベルに対する比）。
    // 飽和の手前の非線形な領域を避ける。
    double fit_upper = 0.80;
    double fit_lower = 0.01;
    // 露出比の全体最適化: 並べた順で global_span 枚先までの組の比をすべて測り、まとめて最小二乗で解く
    // （対数で）。隣どうしの比をつなぐだけだと誤差が積み重なり、基準フレームとの関係が数 % ずれる。
    // 1 = 隣どうしをつなぐだけ（以前の動き）。
    int global_span = 2;
};

struct ExposurePlan {
    std::vector<int> order;             // 暗い→明るい の順に並べた入力の番号
    std::vector<double> rel_exposure;   // order の順。最も暗いフレームを 1 とした相対露光量
    std::vector<ClipLevels> clip;       // 入力の番号の順
    std::vector<PairFit> fits;          // order の隣り合う組ごと（N-1 個）
    std::vector<PairFit> wide_fits;     // 全体の最適化に使った、2 枚以上離れた組
};

ExposurePlan estimate_exposures(const std::vector<RawFrame>& frames, const ExposureOptions& options = {});

}  // namespace hdr
