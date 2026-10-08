#pragma once

// 位置合わせ（開発計画書 §4）。
//
// 1. 平行移動を CFA の周期の倍数（Bayer は 2 画素、X-Trans は 6 画素）で探す。この移動なら色の並びが崩れない。
// 2. 続けて、回転・拡大縮小・1 画素未満の移動（相似変換）を縮小画像の上で詰める（ガウス・ニュートン法）。
//    三脚のように 1 の結果で十分（どの隅でも 0.3 画素以内）なら、1 の移動だけを当てはめる（画素は元のまま）。
//    足りないフレームは、色補間 → 変換 → 元の色の並びに戻す（再モザイク）。合成・書き出しは CFA のまま進む。
//
// - 基準フレームは動かさない（レンズプロファイルが元の光学中心と寸法を前提にしているため）
// - 他のフレームを基準フレームに合わせる。隣り合う露出どうしで推定し、つないで基準まで届かせる
//   （露出の離れたフレームどうしは、両方で有効な画素が少なく合わせにくいため）
// - 動かした分だけ画像の端に無効な所ができるので、DNG の DefaultCrop で隠す

#include <string>
#include <vector>

#include "hdrcore/exposure.hpp"
#include "hdrcore/raw_frame.hpp"

namespace hdr {

struct FrameShift {
    int dx = 0, dy = 0;  // aligned(x, y) = original(x + dx, y + dy)。基準フレームは (0, 0)
    // 相似変換（RawFrame::warp と同じ意味）。needs_warp = 平行移動 (dx, dy) では足りない（再モザイクが要る）。
    double warp[4] = {1.0, 0.0, 0.0, 0.0};
    bool needs_warp = false;
    double angle_deg = 0.0;  // 回転（度、表示用）
    double score = 0.0;  // 合わせた後の違い（対数の二乗平均。小さいほどよく合っている）
    bool reliable = true;  // 両方で有効な画素が十分あったか
};

struct AlignOptions {
    int max_shift_px = 96;  // 探す範囲（画素）
    // 回転・1 画素未満の移動も詰める。false なら平行移動（周期の倍数）だけ。
    bool subpixel = true;
    // 平行移動だけとの違いが、どの隅でもこれ以下なら再モザイクしない（画素）。
    double warp_threshold_px = 0.3;
};

// 各フレームのずれを推定する（入力の番号の順）。plan は位置合わせ前の推定でよい。
std::vector<FrameShift> estimate_shifts(const std::vector<RawFrame>& frames, const ExposurePlan& plan, int reference,
                                        const AlignOptions& options = {});

// ずれを当てはめる（元の画素は frame.original に残すので、何度当て直してもよい）。
// 移動量は CFA の周期の倍数に丸める。
void apply_shift(RawFrame& frame, int dx, int dy);

// 相似変換を当てはめる（元の画素は frame.original に残す）。変換が周期の倍数の平行移動と見なせるなら apply_shift と同じ。
// clip はこのフレームの飽和レベル（飽和した画素の近くは、動かした後も飽和の値にして、合成で使われないようにする）。
void apply_warp(RawFrame& frame, const double warp[4], const ClipLevels& clip, double threshold_px = 0.3);

// FrameShift をそのまま当てはめる（needs_warp なら apply_warp、そうでなければ apply_shift）。
void apply_frame_shift(RawFrame& frame, const FrameShift& shift, const ClipLevels& clip);

// すべてのフレームで有効な範囲（基準フレームの座標）。
void valid_area(const std::vector<RawFrame>& frames, int& x0, int& y0, int& x1, int& y1);

}  // namespace hdr
