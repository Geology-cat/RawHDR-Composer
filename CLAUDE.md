# RawHDR Composer — 開発の手引き（Claude 向け）

## 作業を始めるとき

- **まず [docs/開発録と今後.md](docs/開発録と今後.md) を読んで、現状を把握すること。**
  実装済み／未実装の一覧、既知の問題、検証で分かったこと（Camera Raw の癖など）、今後の提案と次の版の案がまとまっている。
- 細かい検証の経緯は [docs/検証記録.md](docs/検証記録.md)、最初の計画は [docs/開発計画書.md](docs/開発計画書.md)。

## 開発録の更新（必ず行う）

次のときは `docs/開発録と今後.md` を更新し、同じコミットに含める。

- 機能を足した・直した・やめた → 「3. 実装の現状」の ✅🔶❌ と補足、必要なら「3.5 計画書との対応」
- Camera Raw・Lightroom などで新しく分かったこと → 「2.3 重要な判断と、検証で分かったこと」（理由と対応を残す）
- 不具合を見つけた・直した → 「4. 既知の問題」
- 提案を実装した・優先度が変わった → 「5. より良くするための提案」と「5.5 次の版の案」
- リリースした → 「2.1 年表」と冒頭の版・最終更新日

## 決まりごと

- やり取り・コメント・文書・コミットメッセージは日本語。
- 最低対応 OS は macOS 10.12（Intel）。新しい API は `@available` で囲む（`-Werror=unguarded-availability` で検出される）。版と最低 OS は `CMakeLists.txt` の 1 か所で決まる。
- 画質に関わる変更は、自動テストだけでなく Camera Raw で現像して数値（明るさの比・色差のばらつきなど）で確かめる（`tools/acr_render.sh`）。検証用の XMP には `crs:ProcessVersion`・`crs:Version` を入れる。
- `test-photo/`（テスト用の写真）はリポジトリに入れない。画面の写真を撮るときは、利用者の個人情報（ユーザー名・ドライブ名・デスクトップ）が写らないようにする（ウインドウ単位で撮る）。
- GitHub のリリースなど公開を伴う操作は、ユーザーの了承を得てから行う。

## よく使うコマンド

```bash
cmake -S . -B .build/dev -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build .build/dev   # 開発用ビルド
.build/dev/tests/core_tests                                                               # テスト
scripts/build_app.sh                                                                      # 配布用アプリ（Universal）
scripts/make_dmg.sh                                                                       # dmg（アプリ・説明書・インストーラ）
```
