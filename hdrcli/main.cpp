// rawhdr: RAWブラケットのHDR合成（コマンドライン版）。
//
//   rawhdr info  RAW...                    各RAWの情報を表示する
//   rawhdr merge [オプション] RAW...        合成して DNG を書き出す
//
// merge のオプション:
//   -o, --output PATH     書き出す DNG（省略時は基準フレームの隣に「<名前>_HDR.dng」）
//   --ref N               基準フレーム（入力の順で 1 から数える。省略時は露光量が中央のもの）
//   --ramp X              明るいフレームの重みを下げ始める明るさ（飽和の閾値に対する比、既定 0.55）
//   --feather PX          重みのちらつきを抑えるぼかしの幅（画素、既定 16）
//   --safety X            飽和とみなす閾値（飽和レベルに対する比、既定 0.92）
//   --align               自動で位置合わせする（CFA の周期の倍数の平行移動。基準フレームは動かさない）
//   --shift N:dx,dy       N 枚目（入力の順）のずれを手で指定する（--align の結果より優先）
//   --no-deghost          動いた物のゴースト対策をしない（既定はする）
//   --ghost S             ゴーストの検出の感度（0〜1、既定 0.5）。大きいほど小さな違いも動いたとみなす
//   --compress S          明暗差の圧縮の強さ（0〜1、既定 0 = しない、0.5 = 標準、1 = 最大）。月などを抑え、暗部を持ち上げる倍率をデータに焼き込む
//   --no-ca               倍率色収差（月の縁の赤・緑の縁取り）を補正しない
//   --format F            出力形式: auto（既定）・cfa・linear（LinearRaw = 色補間済み）
//   --no-compress         DNG を圧縮しない
//   --lens-xmp            XMP でレンズプロファイル補正を有効にする（ユーザーの既定の現像設定は使われなくなる）
//   --baseline EV         機種の BaselineExposure（Adobe DNG Converter が無いときに使う。既定 0）
//   --no-adobe            Adobe DNG Converter があっても使わない
//   --debug DIR           確認用の画像（由来マップ・プレビュー）を DIR に書く

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "hdrcore/align.hpp"
#include "hdrcore/dng_writer.hpp"
#include "hdrcore/exposure.hpp"
#include "hdrcore/merge.hpp"
#include "hdrcore/output_format.hpp"
#include "hdrcore/preview.hpp"
#include "hdrcore/raw_frame.hpp"
#include "hdrcore/lateral_ca.hpp"
#include "hdrcore/tone_compress.hpp"

namespace {

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void usage() {
    std::fprintf(stderr,
                 "使い方:\n"
                 "  rawhdr info  RAW...\n"
                 "  rawhdr merge [-o OUT.dng] [--ref N] [--format auto|cfa|linear] [--ramp X] [--feather PX] [--safety X] [--no-compress]\n"
                 "               [--no-deghost] [--ghost S] [--compress S] [--align] [--no-ca]\n"
                 "               [--lens-xmp] [--baseline EV] [--debug DIR] RAW...\n");
}

std::string shutter_text(double t) {
    char buf[32];
    if (t > 0.0 && t < 0.3) {
        std::snprintf(buf, sizeof(buf), "1/%.0f", 1.0 / t);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f", t);
    }
    return buf;
}

int cmd_info(const std::vector<std::string>& files) {
    int rc = 0;
    for (const std::string& path : files) {
        try {
            const hdr::RawFrame f = hdr::load_raw_frame(path, true);
            const hdr::ClipLevels clip = hdr::detect_clip_levels(f);
            std::printf("%s\n", f.file_name.c_str());
            std::printf("  カメラ      : %s（UniqueCameraModel: %s）\n", f.model.c_str(), f.unique_camera_model.c_str());
            std::printf("  レンズ      : %s  %.0fmm\n", f.lens_model.c_str(), f.focal_length);
            std::printf("  露出        : %s秒  F%.1f  ISO%.0f  （名目 %.2f EV）\n", shutter_text(f.exposure_time).c_str(), f.fnumber,
                        f.iso, f.nominal_ev());
            std::printf("  寸法        : %d×%d（既定の切り抜き %d,%d %d×%d）  CFA %d×%d  %dbit\n", f.width, f.height, f.crop_x,
                        f.crop_y, f.crop_w, f.crop_h, f.cfa.w, f.cfa.h, f.bits);
            std::printf("  白（機種表）: %.0f %.0f %.0f\n", f.white[0], f.white[1], f.white[2]);
            std::printf("  飽和（実測）: %.0f%s %.0f%s %.0f%s  （* = 飽和した画素がある）\n", clip.level[0], clip.detected[0] ? "*" : "",
                        clip.level[1], clip.detected[1] ? "*" : "", clip.level[2], clip.detected[2] ? "*" : "");
            std::printf("  メタデータ  : EXIF %zu件  GPS %zu件  メーカーノート %zuバイト\n", f.meta.exif.size(), f.meta.gps.size(),
                        f.meta.makernote.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "エラー: %s\n", e.what());
            rc = 1;
        }
    }
    return rc;
}

void write_debug(const std::string& dir, const hdr::MergeResult& m, const std::vector<hdr::RawFrame>& frames) {
    mkdir(dir.c_str(), 0755);
    const hdr::RawFrame& ref = frames[m.reference];
    for (int ev : {-2, 0, 2}) {
        hdr::PreviewOptions o;
        o.max_size = 1600;
        o.exposure_ev = m.reference_ev_offset + m.opening_ev + ev;
        const hdr::Rgb8Image img = hdr::render_preview(m.data.data(), m.width, m.height, m.cfa, ref.color_matrix, ref.as_shot_neutral, o);
        hdr::write_png(dir + "/merged_" + (ev > 0 ? "+" : "") + std::to_string(ev) + "EV.png", hdr::apply_orientation(img, ref.orientation));
    }
    // 由来マップ（暗い→明るい の順に 青→水色→緑→黄→橙→赤→桃→白）。
    static const unsigned char pal[8][3] = {{40, 40, 255}, {0, 160, 255}, {0, 220, 120}, {200, 220, 0},
                                            {255, 140, 0}, {255, 40, 40}, {255, 0, 200}, {255, 255, 255}};
    hdr::Rgb8Image sm;
    sm.width = m.grid_w;
    sm.height = m.grid_h;
    sm.rgb.assign(static_cast<std::size_t>(sm.width) * sm.height * 3, 0);
    for (std::size_t i = 0; i < static_cast<std::size_t>(sm.width) * sm.height; ++i) {
        for (int c = 0; c < 3; ++c) {
            double v = 0;
            for (std::size_t o = 0; o < m.weights.size(); ++o) v += m.weights[o][i] * pal[o % 8][c];
            sm.rgb[i * 3 + c] = static_cast<unsigned char>(std::lround(std::min(255.0, v)));
        }
    }
    hdr::write_png(dir + "/source_map.png", hdr::apply_orientation(sm, ref.orientation));
    if (!m.ghost_mask.empty()) {
        // 動いた所（ゴースト対策で重みを変えた所）を白く。
        std::vector<uint8_t> g(m.ghost_mask.size());
        for (std::size_t i = 0; i < g.size(); ++i) g[i] = static_cast<uint8_t>(std::lround(255.0 * m.ghost_mask[i]));
        hdr::Rgb8Image gm;
        gm.width = m.grid_w;
        gm.height = m.grid_h;
        gm.rgb.resize(g.size() * 3);
        for (std::size_t i = 0; i < g.size(); ++i) gm.rgb[i * 3] = gm.rgb[i * 3 + 1] = gm.rgb[i * 3 + 2] = g[i];
        hdr::write_png(dir + "/ghost_mask.png", hdr::apply_orientation(gm, ref.orientation));
    }
}

int cmd_merge(int argc, char** argv) {
    std::vector<std::string> files;
    std::string output, debug_dir;
    hdr::MergeOptions mo;
    hdr::DngWriteOptions dopt;
    int ref = -1;
    bool use_adobe = true;
    hdr::OutputFormat format = hdr::OutputFormat::Auto;
    bool align = false;
    hdr::ToneCompressOptions compress;
    bool fix_ca = true;
    struct Manual {
        int index, dx, dy;
    };
    std::vector<Manual> manual_shifts;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s の値がありません\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-o" || a == "--output") {
            output = next();
        } else if (a == "--ref") {
            ref = std::atoi(next().c_str()) - 1;
        } else if (a == "--feather") {
            mo.feather_px = std::atoi(next().c_str());
        } else if (a == "--ramp") {
            mo.ramp_start = std::atof(next().c_str());
        } else if (a == "--safety") {
            mo.safety = std::atof(next().c_str());
        } else if (a == "--no-compress") {
            dopt.compress = false;
        } else if (a == "--no-adobe") {
            use_adobe = false;
        } else if (a == "--compress") {
            compress.strength = std::atof(next().c_str());
        } else if (a == "--no-deghost") {
            mo.deghost = false;
        } else if (a == "--ghost") {
            mo.ghost_sensitivity = std::atof(next().c_str());
        } else if (a == "--no-ca") {
            fix_ca = false;
        } else if (a == "--align") {
            align = true;
        } else if (a == "--shift") {
            // N:dx,dy（N は入力の順で 1 から）
            const std::string v = next();
            int k = 0, dx = 0, dy = 0;
            if (std::sscanf(v.c_str(), "%d:%d,%d", &k, &dx, &dy) != 3 || k < 1) {
                std::fprintf(stderr, "--shift は N:dx,dy の形で指定してください\n");
                return 2;
            }
            manual_shifts.push_back({k - 1, dx, dy});
        } else if (a == "--format") {
            const std::string f = next();
            if (f == "auto") format = hdr::OutputFormat::Auto;
            else if (f == "cfa") format = hdr::OutputFormat::Cfa;
            else if (f == "linear") format = hdr::OutputFormat::LinearRaw;
            else {
                std::fprintf(stderr, "--format は auto・cfa・linear のどれかです\n");
                return 2;
            }
        } else if (a == "--bits") {
            dopt.bits = std::atoi(next().c_str());
        } else if (a == "--data-scale") {
            dopt.data_scale = std::atof(next().c_str());
        } else if (a == "--white-level") {
            dopt.white_level = static_cast<uint32_t>(std::atol(next().c_str()));
        } else if (a == "--baseline") {
            dopt.camera_baseline_exposure = std::atof(next().c_str());
        } else if (a == "--lens-xmp") {
            dopt.enable_lens_profile = true;
        } else if (a == "--debug") {
            debug_dir = next();
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "不明なオプション: %s\n", a.c_str());
            return 2;
        } else {
            files.push_back(a);
        }
    }
    if (files.size() < 2) {
        std::fprintf(stderr, "RAW を2枚以上指定してください\n");
        return 2;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<hdr::RawFrame> frames;
    for (const std::string& f : files) frames.push_back(hdr::load_raw_frame(f, true));
    std::printf("読み込み: %d枚 %.1f秒\n", static_cast<int>(frames.size()), seconds_since(t0));

    const auto t1 = std::chrono::steady_clock::now();
    hdr::ExposurePlan plan = hdr::estimate_exposures(frames);
    if (align || !manual_shifts.empty()) {
        // 位置合わせ（基準フレームは動かさない）。合わせた後で露出比を測り直す。
        const int reference = ref >= 0 && ref < static_cast<int>(frames.size()) ? ref : hdr::auto_reference(frames, plan);
        std::vector<hdr::FrameShift> shifts(frames.size());
        if (align) shifts = hdr::estimate_shifts(frames, plan, reference);
        for (const Manual& mm : manual_shifts) {
            if (mm.index < static_cast<int>(shifts.size())) {
                shifts[mm.index].dx = mm.dx;
                shifts[mm.index].dy = mm.dy;
            }
        }
        for (std::size_t i = 0; i < frames.size(); ++i) {
            hdr::apply_shift(frames[i], shifts[i].dx, shifts[i].dy);
            std::printf("  ずれ [%zu] %+d, %+d 画素%s\n", i + 1, frames[i].shift_x, frames[i].shift_y,
                        shifts[i].reliable ? "" : "（推定が不確か）");
        }
        plan = hdr::estimate_exposures(frames);
        ref = reference;
    }
    std::printf("露出比の推定: %.1f秒\n", seconds_since(t1));
    for (std::size_t o = 0; o < plan.order.size(); ++o) {
        const int i = plan.order[o];
        const hdr::ClipLevels& c = plan.clip[i];
        std::printf("  [%d] %-32s %8s秒  相対 %+6.3f EV  飽和 %.0f/%.0f/%.0f\n", i + 1, frames[i].file_name.c_str(),
                    shutter_text(frames[i].exposure_time).c_str(), std::log2(plan.rel_exposure[o]), c.level[0], c.level[1], c.level[2]);
    }
    for (const hdr::PairFit& f : plan.fits) {
        std::printf("  [%d]→[%d] 比 %.4f（実測 %.4f・名目 %.4f、名目との差 %+.3f EV）%s\n", f.dark + 1, f.bright + 1, f.solved_ratio, f.ratio,
                    f.nominal_ratio, std::log2(f.solved_ratio / f.nominal_ratio), f.measured ? "" : "  ※実測できず名目値");
    }
    for (const hdr::PairFit& f : plan.wide_fits) {
        std::printf("  [%d]→[%d] 比 %.4f（離れた組の実測。全体の最適化に使用）\n", f.dark + 1, f.bright + 1, f.ratio);
    }

    mo.reference = ref;
    const auto t2 = std::chrono::steady_clock::now();
    hdr::MergeResult m = hdr::merge_frames(frames, plan, mo);
    std::printf("合成: %.1f秒  基準 [%d] %s  最暗でも飽和 %.4f%%\n", seconds_since(t2), m.reference + 1,
                frames[m.reference].file_name.c_str(), m.clipped_fraction * 100.0);
    if (mo.deghost) {
        std::printf("  ゴースト対策: 動いた所 %d か所、面積 %.2f%%（感度 %.2f）\n", m.ghost_regions, m.ghost_fraction * 100.0, mo.ghost_sensitivity);
    }
    if (m.anchor_samples >= 5000) {
        std::printf("  明るさの基準合わせ: ×%.4f（基準フレームとじかに比べた画素 %d）\n", m.anchor_correction, m.anchor_samples);
    } else {
        std::printf("  明るさの基準合わせ: しない（基準フレームがよく写っている所が少ない。露出比をつないだ値を使う）\n");
    }

    if (output.empty()) {
        const std::string& rp = frames[m.reference].path;
        const std::size_t dot = rp.find_last_of('.');
        output = (dot == std::string::npos ? rp : rp.substr(0, dot)) + "_HDR.dng";
    }
    if (fix_ca) {
        const auto tca = std::chrono::steady_clock::now();
        const hdr::LateralCa ca = hdr::estimate_lateral_ca(m);
        if (ca.valid) {
            hdr::apply_lateral_ca(m, ca);
            std::printf("倍率色収差の補正: 隅でのずれ R %+.2f px・B %+.2f px（輪郭の色のずれ R −%.0f%%・B −%.0f%%、輪郭 %d 点、%.1f秒）\n",
                        ca.corner_shift_px[0], ca.corner_shift_px[2], ca.improvement[0] * 100.0, ca.improvement[2] * 100.0,
                        ca.samples, seconds_since(tca));
        } else {
            std::printf("倍率色収差の補正: しない（%s）\n", ca.note.c_str());
        }
    }
    if (compress.strength > 0.0) {
        const hdr::ToneCompressResult tc = hdr::compress_tone(m, frames[m.reference].as_shot_neutral, compress);
        std::printf("明暗差の圧縮: 強さ %.2f  倍率 %+.1f〜%+.1f 段  大まかな明るさの幅 %.1f 段 → %.1f 段  開いたときの明るさ %+.1f 段\n",
                    compress.strength, std::log2(tc.min_gain), std::log2(tc.max_gain), tc.before_span, tc.after_span, tc.opening_ev);
    }
    // 明暗差を圧縮したときは、自動なら LinearRaw にする（倍率が輪郭で急に変わるので、CFA のまま
    // Camera Raw に色補間させると縁に色の縞が出る。LinearRaw では倍率を外して色補間してから掛け直す）。
    const hdr::FormatDecision fd = hdr::decide_output_format(m, !m.gain.empty() && format == hdr::OutputFormat::Auto ? hdr::OutputFormat::LinearRaw : format);
    dopt.linear_raw = fd.chosen == hdr::OutputFormat::LinearRaw;
    std::printf("出力形式: %s（暗部の値 %.3g、ノイズ %.3g）\n", fd.reason.c_str(), fd.shadow_level, fd.shadow_noise);

    // Adobe DNG Converter があれば、基準フレームを変換して Adobe の解釈を引き継ぐ。
    hdr::DngTemplate tmpl;
    if (use_adobe) {
        const std::string conv = hdr::find_dng_converter();
        if (!conv.empty()) {
            const auto ta = std::chrono::steady_clock::now();
            tmpl = hdr::make_dng_template(frames[m.reference].path, conv);
            std::printf("Adobe DNG Converter %s: %s（%.1f秒）  BaselineExposure %+.2f  白−黒 %.0f %s\n", tmpl.converter_version.c_str(),
                        tmpl.valid ? "使用" : "使えません", seconds_since(ta), tmpl.baseline_exposure, tmpl.white_minus_black, tmpl.note.c_str());
            dopt.adobe_template = &tmpl;
        } else {
            std::printf("Adobe DNG Converter が無いので、LibRaw の色と明るさで書きます\n");
        }
    }
    const auto t3 = std::chrono::steady_clock::now();
    hdr::write_dng(output, m, frames, plan, dopt);
    std::printf("書き出し: %.1f秒  %s\n", seconds_since(t3), output.c_str());
    if (!debug_dir.empty()) write_debug(debug_dir, m, frames);
    std::printf("合計: %.1f秒\n", seconds_since(t0));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string cmd = argv[1];
    try {
        if (cmd == "info") return cmd_info(std::vector<std::string>(argv + 2, argv + argc));
        if (cmd == "merge") return cmd_merge(argc - 2, argv + 2);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "エラー: %s\n", e.what());
        return 1;
    }
    usage();
    return 2;
}
