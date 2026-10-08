#include "hdrcore/dng_writer.hpp"
#include "hdrcore/merged_rgb.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "hdrcore/align.hpp"
#include "hdrcore/demosaic.hpp"
#include "hdrcore/noise.hpp"
#include "hdrcore/parallel.hpp"
#include "hdrcore/preview.hpp"
#include "hdrcore/tiff_builder.hpp"

namespace hdr {

namespace {

// ---- タグ番号 ----
enum : uint16_t {
    kNewSubFileType = 254,
    kImageWidth = 256,
    kImageLength = 257,
    kBitsPerSample = 258,
    kCompression = 259,
    kPhotometric = 262,
    kImageDescription = 270,
    kMake = 271,
    kModel = 272,
    kStripOffsets = 273,
    kOrientation = 274,
    kSamplesPerPixel = 277,
    kRowsPerStrip = 278,
    kStripByteCounts = 279,
    kPlanarConfig = 284,
    kSoftware = 305,
    kDateTime = 306,
    kArtist = 315,
    kPredictor = 317,
    kTileWidth = 322,
    kTileLength = 323,
    kTileOffsets = 324,
    kTileByteCounts = 325,
    kSubIFDs = 330,
    kSampleFormat = 339,
    kXmp = 700,
    kCopyright = 33432,
    kCfaRepeatPatternDim = 33421,
    kCfaPattern = 33422,
    kExifIfd = 34665,
    kGpsIfd = 34853,
    kDngVersion = 50706,
    kDngBackwardVersion = 50707,
    kUniqueCameraModel = 50708,
    kCfaPlaneColor = 50710,
    kCfaLayout = 50711,
    kBlackLevelRepeatDim = 50713,
    kBlackLevel = 50714,
    kWhiteLevel = 50717,
    kDefaultScale = 50718,
    kDefaultCropOrigin = 50719,
    kDefaultCropSize = 50720,
    kColorMatrix1 = 50721,
    kAsShotNeutral = 50728,
    kBaselineExposure = 50730,
    kBaselineNoise = 50731,
    kBaselineSharpness = 50732,
    kLinearResponseLimit = 50734,
    kCameraSerialNumber = 50735,
    kLensInfo = 50736,
    kDngPrivateData = 50740,
    kCalibrationIlluminant1 = 50778,
    kRawDataUniqueId = 50781,
    kOriginalRawFileName = 50827,
    kPreviewColorSpace = 50970,
    kNoiseProfile = 51041,
    kDefaultBlackRender = 51110,
};

// ---- 浮動小数点の形式 ----
// 32bit: IEEE の単精度。16bit: IEEE の半精度。24bit: DNG 独自（符号1・指数7（バイアス 64）・仮数16）。
// どれも「最上位バイトが先頭」の並びで作り、書き出すときに向きを決める。

void to_half_be(float v, uint8_t* out) {
    uint32_t f;
    std::memcpy(&f, &v, 4);
    const uint32_t sign = (f >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((f >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = f & 0x7FFFFFu;
    uint16_t h;
    if (((f >> 23) & 0xFF) == 0xFF) {
        h = static_cast<uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));
    } else if (exp >= 31) {
        h = static_cast<uint16_t>(sign | 0x7C00u);  // 大きすぎる値は無限大
    } else if (exp <= 0) {
        // 非正規化数（最小の刻みは 2^-24）。
        if (exp < -10) {
            h = static_cast<uint16_t>(sign);
        } else {
            mant |= 0x800000u;
            const int shift = 14 - exp;
            uint32_t hm = mant >> shift;
            const uint32_t rem = mant & ((1u << shift) - 1), half = 1u << (shift - 1);
            if (rem > half || (rem == half && (hm & 1u))) ++hm;
            h = static_cast<uint16_t>(sign | hm);
        }
    } else {
        uint32_t hm = mant >> 13;
        const uint32_t rem = mant & 0x1FFFu;
        uint32_t he = static_cast<uint32_t>(exp);
        if (rem > 0x1000u || (rem == 0x1000u && (hm & 1u))) {
            if (++hm == 0x400u) {
                hm = 0;
                ++he;
            }
        }
        h = static_cast<uint16_t>(sign | (he >= 31 ? 0x7C00u : (he << 10) | hm));
    }
    out[0] = static_cast<uint8_t>(h >> 8);
    out[1] = static_cast<uint8_t>(h);
}

void to_fp24_be(float v, uint8_t* out) {
    uint32_t f;
    std::memcpy(&f, &v, 4);
    const uint32_t sign = f >> 31;
    const int32_t e = static_cast<int32_t>((f >> 23) & 0xFF);
    uint32_t mant = f & 0x7FFFFFu;
    int32_t e24 = e - 64;  // DNG SDK の dng_fp24ToFloat の逆（float の指数 = e24 + 64）
    uint32_t m24 = mant >> 7;
    if ((mant & 0x40u) && ((mant & 0x3Fu) || (m24 & 1u))) {
        if (++m24 == 0x10000u) {
            m24 = 0;
            ++e24;
        }
    }
    if (e == 0 || e24 <= 0) {
        e24 = 0;  // とても小さい値（2^-62 未満）は 0 にする
        m24 = 0;
    } else if (e24 >= 0x7F) {
        e24 = 0x7F;
        m24 = 0;
    }
    out[0] = static_cast<uint8_t>((sign << 7) | static_cast<uint32_t>(e24));
    out[1] = static_cast<uint8_t>(m24 >> 8);
    out[2] = static_cast<uint8_t>(m24);
}

void to_float_be(float v, uint8_t* out, int bytes) {
    if (bytes == 2) {
        to_half_be(v, out);
    } else if (bytes == 3) {
        to_fp24_be(v, out);
    } else {
        uint32_t f;
        std::memcpy(&f, &v, 4);
        out[0] = static_cast<uint8_t>(f >> 24);
        out[1] = static_cast<uint8_t>(f >> 16);
        out[2] = static_cast<uint8_t>(f >> 8);
        out[3] = static_cast<uint8_t>(f);
    }
}

// 1行を符号化する。圧縮するときは浮動小数点プレディクタ（TIFF Technical Note 3。libtiff の fpDiff と同じ）:
// 「最上位バイトの並び、次のバイトの並び…」に並べ替えてから、バイト単位で差分を取る。
// 圧縮しないときはリトルエンディアンでそのまま並べる。
void encode_row(const float* src, int n, int bytes, bool predict, uint8_t* out, int stride = 1) {
    uint8_t be[4];
    for (int i = 0; i < n; ++i) {
        to_float_be(src[i], be, bytes);
        for (int b = 0; b < bytes; ++b) {
            if (predict) {
                out[static_cast<std::size_t>(b) * n + i] = be[b];
            } else {
                out[static_cast<std::size_t>(i) * bytes + (bytes - 1 - b)] = be[b];
            }
        }
    }
    if (predict) {
        for (std::size_t i = static_cast<std::size_t>(n) * bytes - 1; i >= static_cast<std::size_t>(stride); --i) {
            out[i] = static_cast<uint8_t>(out[i] - out[i - stride]);
        }
    }
}

// 画素（channels 個ずつ並んだ float）をタイルに分けて符号化する。
std::vector<std::vector<uint8_t>> encode_tiles(const float* data, int width, int height, int channels, int tile, bool compress,
                                               float scale, int bytes, float offset = 0.0f) {
    const int tx = (width + tile - 1) / tile, ty = (height + tile - 1) / tile;
    std::vector<std::vector<uint8_t>> blocks(static_cast<std::size_t>(tx) * ty);
    bool failed = false;
    parallel_for(static_cast<int>(blocks.size()), [&](int i0, int i1) {
        std::vector<float> row(static_cast<std::size_t>(tile) * channels);
        std::vector<uint8_t> raw(static_cast<std::size_t>(tile) * tile * bytes * channels);
        for (int i = i0; i < i1; ++i) {
            const int x0 = (i % tx) * tile, y0 = (i / tx) * tile;
            for (int y = 0; y < tile; ++y) {
                const int sy = std::min(height - 1, y0 + y);
                const float* src = data + static_cast<std::size_t>(sy) * width * channels;
                for (int x = 0; x < tile; ++x) {
                    const int sx = std::min(width - 1, x0 + x);
                    for (int c = 0; c < channels; ++c) row[static_cast<std::size_t>(x) * channels + c] = (src[static_cast<std::size_t>(sx) * channels + c] + offset) * scale;
                }
                encode_row(row.data(), tile * channels, bytes, compress,
                           raw.data() + static_cast<std::size_t>(y) * tile * bytes * channels, channels);
            }
            if (!compress) {
                blocks[i] = raw;
                continue;
            }
            uLongf len = compressBound(static_cast<uLong>(raw.size()));
            std::vector<uint8_t> z(len);
            if (compress2(z.data(), &len, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) {
                failed = true;
                continue;
            }
            z.resize(len);
            blocks[i] = std::move(z);
        }
    });
    if (failed) throw std::runtime_error("画素の圧縮に失敗しました");
    return blocks;
}



std::vector<uint8_t> rgb_bytes(const Rgb8Image& img) { return img.rgb; }

void set_rgb8_image(TiffIfd& ifd, const Rgb8Image& img, uint32_t subfile_type) {
    ifd.set_long(kNewSubFileType, subfile_type);
    ifd.set_long(kImageWidth, static_cast<uint32_t>(img.width));
    ifd.set_long(kImageLength, static_cast<uint32_t>(img.height));
    ifd.set_short(kBitsPerSample, std::vector<uint16_t>{8, 8, 8});
    ifd.set_short(kCompression, 1);
    ifd.set_short(kPhotometric, 2);
    ifd.set_short(kSamplesPerPixel, 3);
    ifd.set_long(kRowsPerStrip, static_cast<uint32_t>(img.height));
    ifd.set_short(kPlanarConfig, 1);
    ifd.set_image_blocks(kStripOffsets, kStripByteCounts, {rgb_bytes(img)});
}

std::string xml_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c; break;
        }
    }
    return o;
}

std::string format_double(double v, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return buf;
}

std::string make_xmp(const MergeResult& m, const std::vector<RawFrame>& frames, const ExposurePlan& plan,
                     const DngWriteOptions& opt) {
    std::string sources, ratios;
    for (std::size_t o = 0; o < plan.order.size(); ++o) {
        if (o) {
            sources += ";";
            ratios += ";";
        }
        sources += frames[plan.order[o]].file_name;
        ratios += format_double(std::log2(plan.rel_exposure[o]), 4);
    }
    std::string x;
    x += "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
    x += "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n";
    x += " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n";
    x += "  <rdf:Description rdf:about=\"\"\n";
    x += "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n";
    if (opt.enable_lens_profile) x += "    xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\"\n";
    // 名前空間の URI は識別子なので、アプリ・リポジトリの名前を変えても変えない（以前に書き出した DNG と揃える）。
    x += "    xmlns:rbh=\"https://github.com/Geology-cat/RawBracketHDR/ns/1.0/\"\n";
    x += "   xmp:CreatorTool=\"" + xml_escape(opt.software) + "\"\n";
    if (opt.enable_lens_profile) {
        x += "   crs:LensProfileEnable=\"1\"\n";
        x += "   crs:LensProfileSetup=\"LensDefaults\"\n";
    }
    x += "   rbh:ReferenceFile=\"" + xml_escape(frames[m.reference].file_name) + "\"\n";
    x += "   rbh:SourceFiles=\"" + xml_escape(sources) + "\"\n";
    x += "   rbh:RelativeExposureEV=\"" + ratios + "\"\n";
    // 明暗差の圧縮（覆い焼き・焼き込みの倍率をデータに焼き込んだか）。
    x += "   rbh:ToneCompression=\"" + format_double(m.tone_strength, 2) + "\"\n";
    x += "   rbh:OpeningExposureEV=\"" + format_double(m.opening_ev, 3) + "\"/>\n";
    x += " </rdf:RDF>\n";
    x += "</x:xmpmeta>\n";
    x += "<?xpacket end=\"w\"?>";
    return x;
}

// 出力データから 16 バイトの識別子を作る（Lightroom のキャッシュの区別に使われる）。
std::vector<uint8_t> unique_id(const std::vector<float>& data) {
    uint64_t h1 = 1469598103934665603ull, h2 = 0x9E3779B97F4A7C15ull;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
    const std::size_t n = data.size() * sizeof(float);
    for (std::size_t i = 0; i < n; i += 7) {
        h1 = (h1 ^ p[i]) * 1099511628211ull;
        h2 = (h2 ^ p[n - 1 - i]) * 0xBF58476D1CE4E5B9ull + i;
    }
    std::vector<uint8_t> id(16);
    for (int i = 0; i < 8; ++i) {
        id[i] = static_cast<uint8_t>(h1 >> (8 * i));
        id[8 + i] = static_cast<uint8_t>(h2 >> (8 * i));
    }
    return id;
}

// 明暗差の圧縮で倍率 g を掛けた後のノイズを、ひとつの NoiseProfile（分散 = S·x + O）で表す。
// 持ち上げた暗部では、ノイズも g 倍（分散は g² 倍）になる: 出力の明るさ x の所の分散は g·S·x + g²·O。
// 元のノイズのまま書くと、Camera Raw は暗部のノイズを小さく見積もり、色のノイズ除去が効かず、
// 持ち上げた暗部に緑・マゼンタの粒が大量に残る（灯台、圧縮 91% で確認。野原の色差のばらつき 7.5 → 1.5）。
// 倍率は色によらないので、代表の倍率 ĝ をひとつ決めて S' = ĝ·S、O' = ĝ²·O とする（16×16 画素ごとに調べる）。
NoiseModel noise_after_gain(const MergeResult& m, const NoiseModel& in) {
    const int b = m.block, cell = 8;  // 8 ブロック（= 16 画素）ごと
    const int cw = (m.grid_w + cell - 1) / cell, ch = (m.grid_h + cell - 1) / cell;
    // 16×16 画素ごとの、倍率（対数の平均）と、色ごとの出力の平均。
    std::vector<double> gs, xs[3];
    for (int cy = 0; cy < ch; ++cy) {
        for (int cx = 0; cx < cw; ++cx) {
            double sum[3] = {}, lg = 0.0;
            int n[3] = {}, ng = 0;
            for (int by = cy * cell; by < std::min(m.grid_h, (cy + 1) * cell); ++by) {
                for (int bx = cx * cell; bx < std::min(m.grid_w, (cx + 1) * cell); ++bx) {
                    lg += std::log2(m.gain[static_cast<std::size_t>(by) * m.grid_w + bx]);
                    ++ng;
                    for (int y = by * b; y < std::min(m.height, (by + 1) * b); ++y) {
                        for (int x = bx * b; x < std::min(m.width, (bx + 1) * b); ++x) {
                            const int c = m.cfa.at(x, y);
                            sum[c] += m.data[static_cast<std::size_t>(y) * m.width + x];
                            ++n[c];
                        }
                    }
                }
            }
            if (!ng || !n[0] || !n[1] || !n[2]) continue;
            gs.push_back(std::exp2(lg / ng));
            for (int c = 0; c < 3; ++c) xs[c].push_back(std::max(0.0, sum[c] / n[c]));
        }
    }
    if (gs.size() < 16) return in;
    // 暗い側: G の出力が中央値以下の所。
    std::vector<double> sorted(xs[1]);
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double median = sorted[sorted.size() / 2];
    // 見積もりが小さすぎると色の粒が残るので、暗い側の 97% の所で実際のノイズ以上になる最小の ĝ にする
    // （大きすぎると暗部の色が少しにじむだけで済む）。
    double best_g = 1.0;
    for (double l = 0.0; l <= 12.0; l += 0.05) {
        const double gh = std::exp2(l);
        std::size_t covered = 0, total = 0;
        for (std::size_t i = 0; i < gs.size(); ++i) {
            if (xs[1][i] > median) continue;
            const double x = xs[1][i], g = gs[i];
            const double actual = g * in.S[1] * x + g * g * in.O[1];
            const double model = gh * in.S[1] * x + gh * gh * in.O[1];
            ++total;
            if (model >= actual) ++covered;
        }
        best_g = gh;
        if (total == 0 || covered * 100 >= total * 97) break;
    }
    if (std::getenv("RBH_NOISE_DEBUG")) std::fprintf(stderr, "noise: 代表の倍率 %.2f 段（暗い側の中央値 %.3g）\n", std::log2(best_g), median);
    return scale_noise(in, best_g);
}

}  // namespace

double dng_baseline_ev(const MergeResult& m, const DngTemplate* tmpl, double camera_baseline) {
    double baseline = camera_baseline + m.reference_ev_offset;
    if (tmpl && tmpl->valid && tmpl->white_minus_black > 0.0) {
        baseline = tmpl->baseline_exposure + std::log2(m.reference_rel_exposure * m.darkest_clip / tmpl->white_minus_black);
    }
    return baseline + m.opening_ev;  // 明暗差の圧縮で開いたときの明るさを整えた分
}

void write_dng(const std::string& path, const MergeResult& m, const std::vector<RawFrame>& frames,
               const ExposurePlan& plan, const DngWriteOptions& requested) {
    // 形式に合わせてビット数・倍率・白レベルを決める（指定があればそれ）。
    DngWriteOptions opt = requested;
    const bool half = opt.bits == 16 || (opt.bits == 0 && opt.linear_raw);
    if (opt.bits == 0) opt.bits = opt.linear_raw ? 16 : 32;
    // 値の倍率と WhiteLevel。Camera Raw は BlackLevel を書いた値の 1/65536 刻みに丸めて引くらしく、
    // 白 = 1 のままだと黒の底上げ（2×10⁻⁵ 程度）が刻みと同じくらいしかなく、暗部が大きく狂う
    // （灯台で 4 倍明るくマゼンタに）。倍率を大きくして、底上げを刻みより十分大きくする。
    // 半精度は 65504 までしか表せないので 32768、32bit は 65535。
    if (opt.white_level == 0) opt.white_level = half ? 32768u : 65535u;
    if (opt.data_scale <= 0.0) {
        opt.data_scale = half ? 32768.0 : 65535.0;
        // LinearRaw では、画像の最も明るい値が WhiteLevel の手前（0.9 倍）に来るまで倍率を上げる。Camera Raw の
        // 「かすみの除去」は WhiteLevel に対する値でかすみの量を見積もるらしく、明暗差を圧縮して最も明るい値が
        // WhiteLevel より 9 段も下にあると、まったく効かない（柱状節理と月で確認。基準フレームの白で切ると効く）。
        // 浮動小数点なので倍率を上げても精度は落ちない。明るさは BaselineExposure で戻す。
        if (opt.linear_raw) {
            float mx = 0.0f;
            for (float v : m.data) mx = std::max(mx, v);
            if (mx > 0.0f) opt.data_scale = std::max(opt.data_scale, 0.9 * opt.white_level / mx);
        }
    }
    if (m.reference < 0 || m.reference >= static_cast<int>(frames.size())) throw std::runtime_error("基準フレームがありません");
    const RawFrame& ref = frames[m.reference];
    const SourceMetadata& meta = ref.meta;

    TiffWriter writer;
    TiffIfd& ifd0 = writer.root();

    // ---- 確認用の画像（IFD0 のサムネイルと、SubIFD のプレビュー） ----
    PreviewOptions po;
    po.exposure_ev = m.reference_ev_offset + m.opening_ev;
    po.max_size = 256;
    const Rgb8Image thumb = render_preview(m.data.data(), m.width, m.height, m.cfa, ref.color_matrix, ref.as_shot_neutral, po);
    set_rgb8_image(ifd0, thumb, 1);

    // ---- 元のファイルから引き継ぐ IFD0 のタグ ----
    for (const TiffEntry& e : meta.ifd0) {
        if (e.tag == 0xC614 || e.tag == kOrientation) continue;
        ifd0.set_raw(e.tag, e.type, e.count, to_little_endian(e.data, e.type, meta.little_endian));
    }
    if (!ifd0.has(kMake) && !ref.make.empty()) ifd0.set_ascii(kMake, ref.make);
    if (!ifd0.has(kModel) && !ref.model.empty()) ifd0.set_ascii(kModel, ref.model);
    ifd0.set_short(kOrientation, static_cast<uint16_t>(ref.orientation));
    ifd0.set_ascii(kSoftware, opt.software);

    // ---- DNG のタグ ----
    ifd0.set_bytes(kDngVersion, {1, 4, 0, 0});
    ifd0.set_bytes(kDngBackwardVersion, {1, 4, 0, 0});
    ifd0.set_ascii(kUniqueCameraModel, ref.unique_camera_model);
    if (ref.has_color_matrix) {
        std::vector<double> cm;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) cm.push_back(ref.color_matrix[i][j]);
        }
        ifd0.set_srational(kColorMatrix1, cm);
        ifd0.set_short(kCalibrationIlluminant1, 21);  // D65
    }
    ifd0.set_rational(kAsShotNeutral, {ref.as_shot_neutral[0], ref.as_shot_neutral[1], ref.as_shot_neutral[2]});
    // 明るさ: 基準フレームを Camera Raw で開いたときと同じになるように BaselineExposure を決める。
    // 出力の値 o は「基準フレームの DN / (相対露光量 · 最も暗いフレームの飽和レベル)」なので、
    // 基準フレームを白レベル W で割った値 r とは r = o · E_ref · L0 / W の関係にある。
    const DngTemplate* tmpl = opt.adobe_template && opt.adobe_template->valid ? opt.adobe_template : nullptr;

    // ---- 黒の底上げとノイズ ----
    // 合成の値は黒より下のノイズを負の値で持っている。Camera Raw に CR2 と同じように黒を引かせるため、
    // 値を P だけ底上げして書き、BlackLevel を P にする（0 で切ると暗部の平均が持ち上がり色がかぶる）。
    // P は最も明るいフレームの読み出しノイズの 16 倍（負に振れる分を十分に含む）。
    const double el0 = m.brightest_rel_exposure * m.darkest_clip;  // 出力の 1 にあたる、最も明るいフレームの DN
    const NoiseModel& ndn = m.brightest_noise_dn;
    double read_sd = 0.0;
    for (int c = 0; c < 3; ++c) read_sd = std::max(read_sd, std::sqrt(ndn.O[c]));
    if (!ndn.valid || !(read_sd > 0.0)) read_sd = 8.0;
    // BlackLevel は有理数で書くので、2^-30 刻みに丸めた値をそのまま底上げにも使う（両者を厳密に一致させる）。
    const double ds = opt.data_scale;
    // 明暗差の圧縮で暗部を持ち上げた所は、ノイズも倍率の分だけ大きいので、底上げもその分大きくする。
    // 分母は 2^30 から、分子が 32bit に収まるまで小さくする（倍率が大きいと底上げも大きくなる）。
    const double black_want = 16.0 * read_sd * std::max(1.0, m.max_gain) / el0 * ds;
    uint32_t black_den = 1u << 30;
    while (black_den > 1 && black_want * black_den > 2147483647.0) black_den >>= 1;
    const uint32_t black_num = static_cast<uint32_t>(std::lround(black_want * black_den));
    const double black_stored = static_cast<double>(black_num) / black_den;  // 書き出す値の単位
    const float pedestal = static_cast<float>(black_stored / ds);  // 合成の値の単位
    // 白と黒の間（Camera Raw が割る値）。
    const double range = static_cast<double>(opt.white_level) - black_stored;

    const double baseline = dng_baseline_ev(m, tmpl, opt.camera_baseline_exposure);
    // 値を data_scale 倍して WhiteLevel で割り戻されるので、明るさの差は data_scale / WhiteLevel の分。
    // 値を data_scale 倍して底上げし、Camera Raw は (値 − 黒) / (白 − 黒) で割り戻すので、
    // 明るさの差は data_scale / (白 − 黒) の分。
    ifd0.set_srational(kBaselineExposure, {baseline - std::log2(ds / range)});
    // Adobe の解釈（色の補正・2光源の行列・プロファイルなど）で上書きする。
    if (tmpl) {
        for (const TiffEntry& e : tmpl->ifd0) ifd0.set_raw(e.tag, e.type, e.count, e.data);
    }
    // 黒の自動調整をさせない（DefaultBlackRender = 1: None）。指定が無いと Camera Raw は画像の統計から黒を
    // 沈める量を自動で決め、明暗差の大きい HDR では暗部が大きく沈んで色もずれる（灯台で、1 秒の CR2 に比べ
    // 暗部が 0.63〜0.75 倍。None にすると 0.95〜1.04 倍に揃った）。
    ifd0.set_long(kDefaultBlackRender, 1);
    ifd0.set_rational(kBaselineNoise, {1.0});
    ifd0.set_rational(kBaselineSharpness, {1.0});
    ifd0.set_rational(kLinearResponseLimit, {1.0});
    if (!ref.camera_serial.empty()) ifd0.set_ascii(kCameraSerialNumber, ref.camera_serial);
    if (ref.lens_info[0] > 0.0) {
        ifd0.set_rational(kLensInfo, {ref.lens_info[0], ref.lens_info[1], ref.lens_info[2], ref.lens_info[3]});
    }
    ifd0.set_ascii(kOriginalRawFileName, ref.file_name);
    ifd0.set_bytes(kRawDataUniqueId, unique_id(m.data));
    ifd0.set_long(kPreviewColorSpace, 2);  // sRGB

    // メーカーノート: DNG の決まり（"Adobe\0" + "MakN" + 長さ + バイト順 + 元のオフセット + 本体）。
    if (!meta.makernote.empty()) {
        std::vector<uint8_t> p = {'A', 'd', 'o', 'b', 'e', 0, 'M', 'a', 'k', 'N'};
        put_u32_be(p, static_cast<uint32_t>(meta.makernote.size() + 6));
        p.push_back(meta.makernote_little_endian ? 'I' : 'M');
        p.push_back(meta.makernote_little_endian ? 'I' : 'M');
        put_u32_be(p, meta.makernote_offset);
        p.insert(p.end(), meta.makernote.begin(), meta.makernote.end());
        ifd0.set_bytes(kDngPrivateData, p);
    }

    const std::string xmp = make_xmp(m, frames, plan, opt);
    ifd0.set_bytes(kXmp, std::vector<uint8_t>(xmp.begin(), xmp.end()));

    // ---- EXIF・GPS ----
    if (!meta.exif.empty()) {
        TiffIfd* exif = ifd0.add_child(kExifIfd);
        for (const TiffEntry& e : meta.exif) exif->set_raw(e.tag, e.type, e.count, to_little_endian(e.data, e.type, meta.little_endian));
    }
    if (!meta.gps.empty()) {
        TiffIfd* gps = ifd0.add_child(kGpsIfd);
        for (const TiffEntry& e : meta.gps) gps->set_raw(e.tag, e.type, e.count, to_little_endian(e.data, e.type, meta.little_endian));
    }

    // ---- RAW 本体（SubIFD 0） ----
    TiffIfd* raw = ifd0.add_child(kSubIFDs);
    raw->set_long(kNewSubFileType, 0);
    raw->set_long(kImageWidth, static_cast<uint32_t>(m.width));
    raw->set_long(kImageLength, static_cast<uint32_t>(m.height));
    const int bits = opt.bits == 16 || (opt.bits == 24 && opt.compress) ? opt.bits : 32;
    raw->set_short(kCompression, opt.compress ? 8 : 1);
    if (opt.compress) raw->set_short(kPredictor, 3);
    raw->set_short(kPlanarConfig, 1);
    raw->set_long(kTileWidth, static_cast<uint32_t>(opt.tile_size));
    raw->set_long(kTileLength, static_cast<uint32_t>(opt.tile_size));
    if (opt.linear_raw) {
        // 色補間済み（LinearRaw）。Camera Raw は浮動小数点のまま処理するので、明暗差の制限がない。
        // 明暗差の圧縮の倍率は、色補間の前に外してから（倍率を掛ける前の値で色補間し）、後で画素ごとに
        // 滑らかに補間した倍率を掛け直す。倍率が CFA の上で急に変わると、隣り合う色の画素の関係が崩れ、
        // 縁に色の縞が出るため（月の縁で確認）。
        std::vector<float> rgb = merged_camera_rgb(m, pedestal);
        raw->set_short(kBitsPerSample, std::vector<uint16_t>(3, static_cast<uint16_t>(bits)));
        raw->set_short(kSampleFormat, std::vector<uint16_t>(3, 3));
        raw->set_short(kPhotometric, 34892);  // LinearRaw
        raw->set_short(kSamplesPerPixel, 3);
        raw->set_image_blocks(kTileOffsets, kTileByteCounts,
                              encode_tiles(rgb.data(), m.width, m.height, 3, opt.tile_size, opt.compress, static_cast<float>(opt.data_scale), bits / 8));
    } else {
        raw->set_short(kPhotometric, 32803);  // CFA
        raw->set_short(kSamplesPerPixel, 1);
        raw->set_image_blocks(kTileOffsets, kTileByteCounts,
                              encode_tiles(m.data.data(), m.width, m.height, 1, opt.tile_size, opt.compress, static_cast<float>(opt.data_scale), bits / 8, pedestal));
        raw->set_short(kCfaRepeatPatternDim, std::vector<uint16_t>{static_cast<uint16_t>(m.cfa.h), static_cast<uint16_t>(m.cfa.w)});
        std::vector<uint8_t> pattern;
        for (int y = 0; y < m.cfa.h; ++y) {
            for (int x = 0; x < m.cfa.w; ++x) pattern.push_back(m.cfa.color[y][x]);
        }
        raw->set_bytes(kCfaPattern, pattern);
        raw->set_bytes(kCfaPlaneColor, {0, 1, 2});
        raw->set_short(kCfaLayout, 1);
    }
    if (!opt.linear_raw) {
        raw->set_short(kBitsPerSample, static_cast<uint16_t>(bits));
        raw->set_short(kSampleFormat, 3);  // IEEE 浮動小数点
    }
    // 黒と白は「繰り返しの大きさ × 1画素の値の数」だけ並べる（LinearRaw は 3 つずつ）。
    const std::size_t spp = opt.linear_raw ? 3 : 1;
    raw->set_short(kBlackLevelRepeatDim, std::vector<uint16_t>{1, 1});
    raw->set_rational_exact(kBlackLevel, std::vector<std::pair<uint32_t, uint32_t>>(spp, {black_num, black_den}));
    // NoiseProfile（Camera Raw のノイズ除去が使う）。シャドウと中間調は最も明るいフレームから来るので、
    // そのノイズを書き出す値の正規化（(値 − 黒) / (白 − 黒)）に換算する: S·k, O·k²、k = ds / (range · el0)。
    // Adobe の校正値があればそれを使う（1 枚の CR2 を開いたときと同じ効き方になる）。
    {
        NoiseModel src = ndn;
        if (tmpl && tmpl->has_noise) {
            for (int c = 0; c < 3; ++c) {
                src.S[c] = tmpl->noise_S[c] * tmpl->white_minus_black;
                src.O[c] = tmpl->noise_O[c] * tmpl->white_minus_black * tmpl->white_minus_black;
            }
            src.valid = true;
        }
        if (src.valid) {
            NoiseModel n = scale_noise(src, 1.0 / el0);  // 合成の値の単位
            if (!m.gain.empty()) n = noise_after_gain(m, n);
            n = scale_noise(n, ds / range);
            raw->set_double(kNoiseProfile, {n.S[0], n.O[0], n.S[1], n.O[1], n.S[2], n.O[2]});
        }
    }
    raw->set_long(kWhiteLevel, std::vector<uint32_t>(spp, opt.white_level));
    raw->set_rational(kDefaultScale, {1.0, 1.0});
    // レンズ補正などの命令は、画像の寸法が Adobe の有効範囲と同じときだけ引き継ぐ（座標が変わるため）。
    // OpcodeList2（色補間の前）は CFA の並びを前提にした命令なので、LinearRaw では使わない。
    if (tmpl && tmpl->active_width == m.width && tmpl->active_height == m.height) {
        for (const TiffEntry& e : tmpl->raw) {
            if (opt.linear_raw && (e.tag == 51009 || e.tag == 50733)) continue;
            raw->set_raw(e.tag, e.type, e.count, e.data);
        }
    }
    // 既定の切り抜き: 機種の既定の範囲と、位置合わせで全フレームが有効な範囲の重なり。
    // 位置合わせで動かした分の端（無効な所）はここで隠す（画素は切り取らない）。
    int vx0, vy0, vx1, vy1;
    valid_area(frames, vx0, vy0, vx1, vy1);
    const int cx0 = std::max(ref.crop_x, vx0), cy0 = std::max(ref.crop_y, vy0);
    const int cx1 = std::min(ref.crop_x + ref.crop_w, vx1), cy1 = std::min(ref.crop_y + ref.crop_h, vy1);
    if (cx1 - cx0 < 16 || cy1 - cy0 < 16) throw std::runtime_error("位置合わせのずれが大きすぎて、有効な範囲が残りません");
    raw->set_long(kDefaultCropOrigin, std::vector<uint32_t>{static_cast<uint32_t>(cx0), static_cast<uint32_t>(cy0)});
    raw->set_long(kDefaultCropSize, std::vector<uint32_t>{static_cast<uint32_t>(cx1 - cx0), static_cast<uint32_t>(cy1 - cy0)});

    // ---- プレビュー（SubIFD 1） ----
    if (opt.embed_preview) {
        po.max_size = 1024;
        const Rgb8Image preview = render_preview(m.data.data(), m.width, m.height, m.cfa, ref.color_matrix, ref.as_shot_neutral, po);
        TiffIfd* pv = ifd0.add_child(kSubIFDs);
        set_rgb8_image(*pv, preview, 1);
    }

    writer.write(path);
}

}  // namespace hdr
