#pragma once

// HDR合成の本体（開発計画書 §5）。
//
// 原則: 各画素について「飽和していない（安全マージン込み）フレームのうち、最も明るいもの」を使う。
// 切り替わりは、明るいフレームの値が飽和の閾値に近づくにつれて重みを滑らかに下げて繋ぐ
// （明るさによる切り替え）。飽和したブロックとその隣には重みを与えない。
//
// 出力は CFA のまま（色補間しない）。値は「最も暗いフレームの飽和点 = 1.0」の線形の値で、
// すべて 0〜1 に収まる（開発計画書 §6.1 案2）。

#include <cstdint>
#include <vector>

#include "hdrcore/exposure.hpp"
#include "hdrcore/noise.hpp"
#include "hdrcore/raw_frame.hpp"

namespace hdr {

struct MergeOptions {
    // 飽和とみなす閾値 = 飽和レベル × safety。飽和の手前の非線形な所を避ける。
    double safety = 0.92;
    // 明るいフレームの重みを下げ始める明るさ（飽和の閾値に対する比）。ここから閾値まで滑らかに切り替える。
    // 小さいほど切り替わりが緩やかになるが、暗い（ノイズの多い）フレームを使う範囲が広がる。
    double ramp_start = 0.55;
    // 重みのちらつきを抑えるぼかしの幅（画素）。
    int feather_px = 16;
    // 基準フレーム（入力の番号）。-1 = 自動（名目の露光量が中央のもの）。
    int reference = -1;

    // ---- ゴースト（動いた物）対策 ----
    // 露出の隣り合うフレームを、露出をそろえて小さな区画（CFA の周期の 4 倍四方）ごとに比べ、ノイズで説明できない
    // 違いがある所を「動いた所」とする。つながった動いた所ごとに 1 枚のフレーム（そこ全体で白飛びしていない中で
    // 最も明るいもの）だけを使うので、二重像・縁の色の滲みが出ない。
    bool deghost = true;
    // 検出の感度（0〜1）。大きいほど小さな違いも動いたとみなす（そのぶん 1 枚だけを使う所が増え、ノイズが少し増える）。
    double ghost_sensitivity = 0.5;

    // ---- 平均型の合成（ノイズを減らす） ----
    // false: 各ブロックで白飛びしていない中で最も明るいフレームを使う（切り替え型）。
    // true: 白飛びしていないフレームを、ノイズの分散の逆数で重みづけして平均する。暗部・中間調のノイズが減るが、
    // フレームの位置がわずかにずれていると細部がぼける。動いた物はゴースト対策（deghost）で守る。
    bool average = true;
};

struct MergeResult {
    int width = 0, height = 0;
    CfaPattern cfa;
    std::vector<float> data;  // 合成した CFA（上は 1.0 で切る。黒より下のノイズは負の値のまま残す）

    int reference = -1;           // 基準フレーム（入力の番号）
    double white_scale = 1.0;     // 出力値 × white_scale = 基準フレームを白レベル 1.0 で見た値
    double reference_ev_offset = 0.0;  // = log2(white_scale)。DNG の BaselineExposure に足す
    double reference_rel_exposure = 1.0;  // 基準フレームの相対露光量（最も暗いフレーム = 1）
    double darkest_clip = 1.0;            // 出力の 1.0 にあたる DN（最も暗いフレームの飽和レベル）
    double anchor_correction = 1.0;       // 基準フレームと直接比べて補正した倍率（1 = 補正なし）
    int anchor_samples = 0;               // その比較に使えた画素の数
    double brightest_rel_exposure = 1.0;  // 最も明るいフレームの相対露光量
    NoiseModel brightest_noise_dn;        // 最も明るいフレームのノイズ（DN、データから見積もったもの）
    // 明暗差の圧縮（tone_compress.hpp）で掛けた倍率（ブロック単位。空なら掛けていない）。
    std::vector<float> gain;
    double max_gain = 1.0;
    // 倍率を画素に配るときの手がかり: ブロックの明るさ（白に対する段、log2）と、それを求めた色の重み。
    // 書き出しで、画素の明るさに近いブロックの倍率を優先して補間する（輪郭の画素に隣の倍率が掛からないように）。
    std::vector<float> gain_guide;
    double gain_coef[3] = {0.0, 0.0, 0.0};
    double gain_wref = 1.0;
    double gain_range = 1.0;
    // ブロックの大まかな明るさ（gain_guide と同じ単位）と、大きな模様を縮める度合い（0〜1）。
    // 大まかな明るさより 1 段以上明るい・暗い画素（月のすぐ外の光のにじみなど）は、その差を縮める。
    std::vector<float> gain_base;
    double detail_compress = 0.0;
    double opening_ev = 0.0;  // 開いたときの明るさを整えた量（段）。BaselineExposure・表示に足す
    double tone_strength = 0.0;  // 明暗差の圧縮の強さ（XMP に記録する）

    // 確認用: ブロック（CFA の周期）単位の重み。order の順（暗い→明るい）。
    int block = 2;
    int grid_w = 0, grid_h = 0;
    std::vector<std::vector<float>> weights;
    double clipped_fraction = 0.0;  // 最も暗いフレームでも飽和していたブロックの割合

    // ゴースト対策（ブロック単位。空なら対策していない）。ghost_mask = 1 枚のフレームに固定した度合い（0〜1、
    // 縁はなだらか）、ghost_frame = 固定したフレーム（order の番号、固定していない所は -1）。
    std::vector<float> ghost_mask;
    std::vector<int8_t> ghost_frame;
    int ghost_regions = 0;          // 動いた所（つながった領域）の数
    double ghost_fraction = 0.0;    // 1 枚に固定した面積の割合

    // 平均型の合成で、最も明るいフレームだけを使う場合に比べたノイズの分散の比（暗い所の代表値。1 = 減っていない）。
    // brightest_noise_dn にはこの比を掛けてある（NoiseProfile がノイズを多く見積もりすぎないように）。
    double average_noise_ratio = 1.0;
};

// frames と plan は estimate_exposures() に渡したもの・返ってきたもの。
MergeResult merge_frames(const std::vector<RawFrame>& frames, const ExposurePlan& plan, const MergeOptions& options);

// 自動で選ぶ基準フレーム（入力の番号）。
int auto_reference(const std::vector<RawFrame>& frames, const ExposurePlan& plan);

}  // namespace hdr
