#pragma once

// 合成結果を浮動小数点の CFA DNG として書き出す（開発計画書 §6）。
//
// - 画素: 32bit float、CFA のまま（Lightroom / Camera Raw がデモザイクする）
// - 値: 最も暗いフレームの飽和点 = 1.0（WhiteLevel = 1、BlackLevel = 0。黒は合成で引き済み）
// - 見た目: BaselineExposure で基準フレームと同じ明るさに開く
// - 色: 基準フレームの ColorMatrix・AsShotNeutral（ホワイトバランスは焼き込まない）
// - レンズ: EXIF のレンズ情報・LensInfo・メーカーノート（DNGPrivateData）を引き継ぐ。
//   Lightroom / Camera Raw はこれを見てレンズプロファイルを自動で当てる
// - 向き・撮影日時・GPS・著作権なども基準フレームから引き継ぐ

#include <string>
#include <vector>

#include "hdrcore/dng_template.hpp"
#include "hdrcore/exposure.hpp"
#include "hdrcore/merge.hpp"
#include "hdrcore/raw_frame.hpp"

namespace hdr {

struct DngWriteOptions {
    bool compress = true;       // Deflate＋浮動小数点プレディクタ（可逆）
    int tile_size = 256;
    // 画素の浮動小数点のビット数（16・24・32）。24 は圧縮するときだけ。
    // 0 = 形式に合わせて決める: CFA は 32bit、LinearRaw は 16bit（半精度）。
    // LinearRaw の半精度は、極端な補正（露光量+3・シャドウ+100）でも 32bit との差が最大 1.3/255 で、
    // ファイルは 233MB → 96MB になる。CFA は Camera Raw の色補間がわずかな違いで向きを変え、
    // 一部の画素で 10/255 の差が出たので 32bit のまま（docs/検証記録.md）。
    int bits = 0;
    // 色補間して LinearRaw として書く（false なら CFA）。どちらにするかは decide_output_format() で決める。
    bool linear_raw = false;
    bool embed_preview = true;  // 簡易現像したプレビュー（Finder・カタログの表示用）
    // XMP にレンズプロファイル補正を有効にする初期設定を入れる（crs:LensProfileEnable=1）。
    // 既定は入れない。crs: の設定が1つでも入っていると、Camera Raw はユーザーの既定の現像設定
    // （プロファイルなど）を使わず Adobe の既定に戻してしまうため（Camera Raw 18.6 で確認）。
    // レンズの認識自体は EXIF のレンズ情報で行われるので、これが無くてもプロファイルは選べる。
    bool enable_lens_profile = false;
    // 機種ごとの BaselineExposure（Adobe が機種ごとに持つ値。分からなければ 0）。テンプレートがあれば使わない。
    double camera_baseline_exposure = 0.0;
    // Adobe DNG Converter で作ったテンプレート（無ければ nullptr か valid = false）。
    // あれば色・プロファイル・明るさの基準・レンズ補正の命令を Adobe の解釈に揃える。
    const DngTemplate* adobe_template = nullptr;
    std::string software = "RawHDR Composer";
    // 書き出す値に掛ける倍率と WhiteLevel（0 = 形式に合わせて決める: 半精度は 32768、32bit は 65535）。
    // Camera Raw は WhiteLevel で割り戻すので見た目は変わらない。大きくする理由は 2 つ:
    // 半精度は小さい値の精度が落ちること、Camera Raw が BlackLevel を 1/65536 刻みに丸めること。
    double data_scale = 0.0;
    uint32_t white_level = 0;
};

// DNG を開いたときの明るさ（BaselineExposure のうち、値の倍率の分を除いたもの。段）。プレビューも同じ明るさで描く。
// テンプレートがあれば Adobe の基準（機種の BaselineExposure と白レベル）、無ければ camera_baseline と基準フレームの白。
double dng_baseline_ev(const MergeResult& merged, const DngTemplate* adobe_template, double camera_baseline);

void write_dng(const std::string& path, const MergeResult& merged, const std::vector<RawFrame>& frames,
               const ExposurePlan& plan, const DngWriteOptions& options = {});

}  // namespace hdr
