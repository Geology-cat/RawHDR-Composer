# 使用説明書の図（fig/*.jpg）を、capture.sh で撮った画面（fig/raw/*.png）から作る。
#   python3 docs/manual/make_figs.py
# 画面全体は幅 2000 画素に縮め、部分の図（c_*）は元の大きさのまま切り抜く。
# 切り抜きの座標は、ウインドウの中身が 3360×1892 画素（1680×946 ポイントの Retina）で撮ったときのもの。
# ダイアログなど手で撮った図（c_dmg・c_dng_prompt・c_open・c_save・c_script_editor）はここでは作らない。
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
RAW = HERE / "fig" / "raw"
FIG = HERE / "fig"

FULL = ["01_empty", "02_loaded", "03_before_merge", "04_merged", "05_overlay", "06_sourcemap", "07_changed",
        "08_diff", "09_align", "10_lighthouse", "11_lh_sourcemap", "12_lh_overlay", "18_lh_ghost"]

# 名前: (元の画面, (左, 上, 右, 下))
RIGHT = (2748, 0, 3360, 1620)  # 右の欄（基準フレーム〜DNG を書き出す）
CROPS = {
    "c_right_merged": ("04_merged", RIGHT),
    "c_right_align": ("09_align", (2748, 0, 3360, 560)),  # 右の欄の上（位置合わせまで）
    "c_right_changed": ("07_changed", RIGHT),
    "c_info": ("19_info", (2748, 250, 3360, 1250)),
    "c_toolbar": ("04_merged", (782, 10, 2070, 132)),
}


def save(img: Image.Image, name: str) -> None:
    img.convert("RGB").save(FIG / f"{name}.jpg", quality=88)
    print(f"{name}.jpg {img.size[0]}×{img.size[1]}")


def main() -> None:
    for name in FULL:
        img = Image.open(RAW / f"{name}.png")
        w, h = img.size
        save(img.resize((2000, round(h * 2000 / w)), Image.LANCZOS), name)
    for name, (src, box) in CROPS.items():
        save(Image.open(RAW / f"{src}.png").crop(box), name)


if __name__ == "__main__":
    main()
