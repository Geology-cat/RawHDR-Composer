#pragma once

// 合成結果を色補間した RGB（カメラの色、ホワイトバランス前）。LinearRaw の書き出しとプレビューで同じものを使う
// （プレビューが、Camera Raw で開いたときの見え方とずれないように）。

#include <vector>

#include "hdrcore/merge.hpp"

namespace hdr {

// 色補間した RGB（画素ごとに R,G,B、値に pedestal を足したまま）。明暗差の圧縮の倍率は、色補間の前に外し、
// 画素ごとに滑らかに配り直して掛ける（局所線形モデルと大きな模様の縮め）。pedestal は黒より下のノイズを残す底上げ。
std::vector<float> merged_camera_rgb(const MergeResult& m, float pedestal);

// プレビューで使う底上げ（書き出しと同じ考え方: 最も明るいフレームの読み出しノイズの 16 倍）。
float preview_pedestal(const MergeResult& m);

}  // namespace hdr
