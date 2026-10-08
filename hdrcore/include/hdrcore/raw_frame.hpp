#pragma once

// カメラRAW 1枚の読み込み。
//
// 合成はすべて「黒を引いた線形のCFA値（DN）」の上で行う。色補間・ホワイトバランス・
// 色変換・ガンマは一切掛けない。出力DNGに引き継ぐため、EXIF・メーカーノートなどの
// メタデータも元の形（バイト列）のまま取り出しておく。

#include <cstdint>
#include <string>
#include <vector>

namespace hdr {

// カラーフィルター配列。Bayer は 2×2、X-Trans は 6×6 の周期。
struct CfaPattern {
    int w = 0;  // 周期（横）。0 のときは CFA ではない
    int h = 0;  // 周期（縦）
    uint8_t color[6][6] = {};  // 0=R 1=G 2=B（DNG の CFAPattern と同じ番号）

    bool valid() const { return w > 0 && h > 0; }
    int at(int x, int y) const { return color[y % h][x % w]; }
    bool is_bayer() const { return w == 2 && h == 2; }
    bool is_xtrans() const { return w == 6 && h == 6; }
};

// 元ファイルの TIFF/EXIF のエントリ1つ。値は元のバイト順のまま持つ。
struct TiffEntry {
    uint16_t tag = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    std::vector<uint8_t> data;  // count × 型の大きさ のバイト列（元ファイルのバイト順）
};

// 出力DNGへ引き継ぐメタデータ。
struct SourceMetadata {
    bool little_endian = true;       // エントリの値のバイト順
    std::vector<TiffEntry> ifd0;     // Make・Model・Orientation・DateTime・Artist・Copyright など
    std::vector<TiffEntry> exif;     // EXIF IFD（MakerNote とサブIFDへのポインタを除く）
    std::vector<TiffEntry> gps;      // GPS IFD
    std::vector<uint8_t> makernote;  // メーカーノート（生のバイト列）
    bool makernote_little_endian = true;
    uint32_t makernote_offset = 0;   // 元の TIFF 内でのオフセット（DNGPrivateData に記録する）

    const TiffEntry* find_exif(uint16_t tag) const;
    const TiffEntry* find_ifd0(uint16_t tag) const;
};

struct RawFrame {
    std::string path;
    std::string file_name;  // パスの最後の部分

    // ---- 画素 ----
    int width = 0;    // 出力する範囲（LibRaw の「見える範囲」）
    int height = 0;
    CfaPattern cfa;
    std::vector<float> data;  // 黒を引いた値（DN）。行優先・CFA 1ch。負の値もそのまま持つ
    float white[3] = {};      // 色ごとの白レベル（黒を引いた後の DN）。LibRaw の機種表の値
    int bits = 0;             // ADC のビット数（分かる範囲で）

    // 位置合わせ（align.hpp）。data は ずれ を当てはめた後の画素で、元の画素は original に残す
    // （ずれが 0 のときは original は空）。
    std::vector<float> original;
    int shift_x = 0, shift_y = 0;
    // 回転・1 画素未満の位置合わせ（apply_warp）。warped なら data は、元の画素を色補間して動かし、
    // 元の色の並び（CFA）に戻したもの。warp = {a, b, tx, ty}: 画像の中心 c からの位置 q について
    // aligned(c + q) = original(c + [a −b; b a]·q + t)。shift_x・shift_y は t を丸めた値（表示用）。
    bool warped = false;
    double warp[4] = {1.0, 0.0, 0.0, 0.0};

    // 機種の既定の切り抜き（見える範囲の座標）。DNG の DefaultCrop に書く。
    int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;

    // ---- 露出 ----
    double exposure_time = 0.0;  // 秒
    double fnumber = 0.0;
    double iso = 0.0;
    // 露光量の名目値（log2）。大きいほど明るい。= log2(t) − 2·log2(N) + log2(ISO/100)
    double nominal_ev() const;

    // ---- カメラ・色 ----
    std::string make, model;
    std::string unique_camera_model;  // DNG の UniqueCameraModel（Adobe の命名に合わせる）
    std::string camera_serial;
    double color_matrix[3][3] = {};   // XYZ(D65) → カメラRGB（DNG の ColorMatrix。LibRaw の Adobe 係数）
    bool has_color_matrix = false;
    double as_shot_neutral[3] = {1, 1, 1};  // 撮影時のホワイトバランス（DNG の AsShotNeutral）
    int orientation = 1;                    // EXIF の Orientation

    // ---- レンズ ----
    std::string lens_model;
    double lens_info[4] = {};  // 最短・最長焦点距離、その開放F値（DNG の LensInfo）
    double focal_length = 0.0;

    // ---- 撮影日時 ----
    std::string datetime_original;  // "YYYY:MM:DD hh:mm:ss"

    SourceMetadata meta;

    float value(int x, int y) const { return data[static_cast<std::size_t>(y) * width + x]; }
};

// RAW を読む。失敗したら std::runtime_error（日本語のメッセージ）を投げる。
// load_pixels=false なら画素を読まずにメタデータだけ埋める。
RawFrame load_raw_frame(const std::string& path, bool load_pixels = true);

}  // namespace hdr
