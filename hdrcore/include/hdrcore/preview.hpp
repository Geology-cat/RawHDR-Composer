#pragma once

// 確認用の簡易現像（縮小・色補間・色変換・トーンカーブ）と、画像ファイルの書き出し。
// DNG のサムネイル・プレビューと、CLI・アプリでの確認表示に使う。
// 合成そのものには使わない（合成は線形のCFAのまま行う）。

#include <cstdint>
#include <string>
#include <vector>

#include "hdrcore/raw_frame.hpp"

namespace hdr {

struct Rgb8Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb;  // 行優先、RGB の順
};

struct PreviewOptions {
    int max_size = 1024;       // 長辺の画素数
    double exposure_ev = 0.0;  // 露出の補正（段）
    bool tone_map = true;      // ハイライトを滑らかに寝かせる
    // 局所トーンマッピング: 明るさの大まかな分布だけを tone_range 段に圧縮し、細かい模様は残す。
    // HDR の合成結果では、月や灯室のように基準の白より何段も明るい所の模様と、暗部の両方が見える。
    bool local_tone = false;
    // Camera Raw の既定の見え方に近いトーンカーブ（tone_map の代わりに使う）。合成した DNG を Camera Raw
    // （18.6〜18.7、既定の設定）で現像した結果と、このプレビューの線形の値を画素ごとに比べて求めた（2 場面で同じ曲線）。
    bool acr_curve = false;
    double tone_range = 9.0;
};

// 線形の RGB（sRGB の原色、白 = 1）。画素ごとに R,G,B。
struct RgbFloatImage {
    int width = 0, height = 0;
    std::vector<float> rgb;
};

// CFA の線形データ（値 1.0 が白の目安）から sRGB の 8bit 画像を作る。
// color_matrix は XYZ(D65)→カメラ、neutral は AsShotNeutral。
Rgb8Image render_preview(const float* cfa, int width, int height, const CfaPattern& pattern,
                         const double color_matrix[3][3], const double neutral[3], const PreviewOptions& options);

// 縮小・色補間・色変換・露出の補正までした線形の値（トーンカーブ・ガンマの前）。
RgbFloatImage render_preview_linear(const float* cfa, int width, int height, const CfaPattern& pattern,
                                    const double color_matrix[3][3], const double neutral[3], const PreviewOptions& options);

// 色補間済みの RGB（カメラの色、画素ごとに R,G,B、値に black を足したまま。merged_camera_rgb() の結果）から、
// render_preview_linear と同じ寸法の線形 sRGB を作る（縮小は面積の平均）。書き出す LinearRaw と同じ画素から作るので、
// Camera Raw で開いたときの見え方に近い（月の縁など、倍率を画素ごとに配り直した所もそのまま写る）。
RgbFloatImage camera_rgb_preview(const float* rgb, int width, int height, float black, const CfaPattern& pattern,
                                 const double color_matrix[3][3], const double neutral[3], const PreviewOptions& options);

// 線形の値に局所トーンマッピング（options.local_tone のとき）・トーンカーブ・ガンマを掛けて 8bit にする。
Rgb8Image finish_preview(RgbFloatImage linear, const PreviewOptions& options);

// 1枚の RAW を、その白レベルを 1.0 として簡易現像する。
Rgb8Image render_frame_preview(const RawFrame& frame, const PreviewOptions& options);

// 向き（EXIF の Orientation）に合わせて回す。
Rgb8Image apply_orientation(const Rgb8Image& image, int orientation);

// PNG・JPEG の書き出し（macOS の ImageIO を使う）。失敗したら false。
bool write_png(const std::string& path, const Rgb8Image& image);
bool write_gray_png(const std::string& path, int width, int height, const std::vector<uint8_t>& gray);
bool encode_jpeg(const Rgb8Image& image, double quality, std::vector<uint8_t>& out);

}  // namespace hdr
