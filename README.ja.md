# AviSynth — Iris

[English](README.md) | [简体中文](README.zh-CN.md) | **日本語**

AviSynth — Iris は、AviSynthMinus とともに開発されている独立したピクセル式エンジンです。逆ポーランド記法（RPN）の式を型付き中間表現にコンパイルし、スカラーインタープリターまたは任意の LLVM JIT バックエンドで実行します。

インターフェースは C の型と関数を使用し、内部実装は C++17 を使用します。エンジンのビルドは AviSynth SDK、AvsCore、AvsSimd に依存しません。AviSynth への組み込みは別のホストアダプターが担当します。

## 式の評価を分離する理由

式エンジンをフレームサーバーから分離することで、解析、最適化、数値動作、実行バックエンドを独立して保守・検証できます。ホストはクリップ、スクリプト登録、フレーム割り当て、プロパティ、スケジューリングを担当し、Iris は明示的なプレーンバッファー、コンパイル済みプラン、実行 context を処理します。

プランは再利用可能な式とバックエンドの状態を保持します。並列実行では個別の context と出力領域を使用するため、ホストはワーカーごとにフィルターを複製せずにコンパイル済みコードを共有できます。

## 対応する機能

| 分類 | 機能 |
|---|---|
| サンプル | U8、有効ビット深度 9–16 の U16、F32。入出力形式の混在と符号付きバイト stride に対応。 |
| 式 | 算術、比較、論理、条件選択、変数、スタック操作、丸め、超越関数。 |
| 入力 | v1 API で最大 26 入力。名前は `x`、`y`、`z`、続いて `a` から `w`。 |
| 空間アクセス | ピクセル座標、プレーン寸法、正規化座標、画像端にクランプされる固定近傍オフセット。 |
| フレーム情報 | フレーム番号、時間、フレームプロパティ、ビット深度・レンジ定数、Expr の入力スケーリングモード。 |
| 最適化 | 共通 IR 最適化、塗りつぶし・コピー、任意の自動 U8 LUT、明示的な整数 1D/2D LUT。 |

式言語は AviSynth Expr に基づき、意図的な修正と厳密な解析規則を含みます。すべてのスクリプトオプションとの互換性や、過去のすべての Expr 実装との出力一致は保証しません。予約語への代入や、`dup0tail` のような不正なスタックインデックスは拒否します。

数値の中間値には binary32 を使用します。負数の `sqrt` は正のゼロを返し、NaN は NaN のままです。`round` は中間値をゼロから遠い方向へ丸めます。整数出力は範囲内にクランプして half-up で丸め、NaN をゼロに変換します。Iris はホストの浮動小数点環境を変更しません。非正規化数や丸め境界の動作はバックエンドや最適化設定によって異なる場合があります。

## バックエンドと CPU 選択

| バックエンド | 実行と数学関数 |
|---|---|
| `scalar` | 既定のスカラー参照インタープリター。ホストの数学ライブラリを使用。 |
| `llvm` | LLVM JIT。ホストの数学ライブラリを使用。 |
| `sleef` | LLVM JIT と SLEEF u10 数学関数。 |
| `sleef-fast` | LLVM JIT、一部の SLEEF u35 関数、範囲を限定したガンマべき乗の高速処理。 |

利用できないバックエンドの明示的な要求は失敗します。LLVM と SLEEF は任意のビルド依存関係ですが、二つの SLEEF バックエンドには両方が必要です。`iris_backend_available` で利用可否を照会できます。

LLVM は実行環境の CPU 機能とコストモデルを使用します。ベクトル対応が組み込まれ、CPU と OS が対応している場合、SLEEF は AVX2/FMA ベクトル呼び出しを使用でき、それ以外は対応するスカラー関数を使用します。`IRIS_SLEEF_AVX2=OFF` でこのベクトルマッピングを無効にできます。変換モジュールとは異なり、Iris の JIT ターゲット選択は現在 AviSynth の `SetMaxCPU` 制限と連携していません。

SLEEF バックエンド名は数学処理の方針を選ぶもので、式全体の誤差上限を示すものではありません。高速処理では、底が [1/65535, 1]、指数が [0.25, 4] の範囲で `exp2(exponent * log2(base))` を使用し、許容する絶対誤差のしきい値を 1e-6 としています。範囲外のべき乗と `exp` は u10 を使用します。現在、非 FMA スカラー u10 `atan2` には 2 ULP の誤差を許容しています。バックエンド間のビット単位の一致は要求しません。

## ビルドと組み込み

CMake 3.16 以降と、浮動小数点の `std::from_chars` に対応する標準ライブラリを備えた C++17 コンパイラーが必要です。C のサンプルとインターフェーステストは C99 を使用します。既定のビルドには LLVM も AviSynth も不要で、依存関係をダウンロードしません。

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/release --config Release --parallel
ctest --test-dir build/release -C Release --output-on-failure
```

`-DBUILD_TESTING=OFF` でテストを無効にできます。CMake 3.21 以降では付属の Ninja presets、`scalar`、`llvm`、`sanitize`、`llvm-sanitize` も使用できます。

静的ライブラリーのターゲットは `iris`、エイリアスは `Iris::Iris` です。ソースをホストの依存関係ディレクトリーに配置した後、直接リンクします。

```cmake
add_subdirectory(third_party/iris)
target_link_libraries(MyHost PRIVATE Iris::Iris)
```

対応するヘッダーとライブラリーを組み合わせてください。C 境界は C++ や LLVM の型を公開せず、C++ 例外も境界を越えません。対応する組み込み方法はソースを一緒にビルドする方式であり、独立 DLL の安定した ABI を保証するものではありません。最終リンクには C++ ランタイムが必要です。

公開 API は [include/iris/iris.h](include/iris/iris.h) にあります。`iris_compile_v1` または `iris_compile_expr_v1` でコンパイルし、入力・プロパティ依存関係を照会して context を作成し、`iris_execute_v1` を呼び出します。バージョン付き構造体の `struct_size` は、その構造体の正確な `sizeof` に設定してください。plan は共有できますが、同じ context を同時実行することはできません。呼び出し側が有効なバッファー、符号付きバイト stride、重複しない入出力領域を提供します。[C サンプル](examples/example.c) を参照してください。

### 任意の LLVM と SLEEF

`-DIRIS_LLVM=ON -DLLVM_DIR=/path/to/lib/cmake/llvm` で LLVM を有効にします。実装は LLVM 20–22 C API に対応し、エクスポートされた `LLVM` CMake ターゲットを必要とします。Ubuntu 24.04 では公式リポジトリーから `llvm-20-dev` をインストールし、`LLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm` を指定できます。

`-DIRIS_SLEEF=ON -DIRIS_FETCH_SLEEF=ON` を指定すると、SLEEF 3.9.0 をダウンロードし、SHA256 を検証して Iris とともに静的ライブラリーをビルドします。CMake 3.18 以降が必要です。ダウンロードは既定では無効です。有効にするとビルドディレクトリーを使用し、SLEEF の個別インストールは不要です。

```sh
cmake -S . -B build/release -DIRIS_LLVM=ON -DLLVM_DIR=/path/to/lib/cmake/llvm -DIRIS_SLEEF=ON -DIRIS_FETCH_SLEEF=ON
cmake --build build/release --config Release --parallel
```

オフラインでは、展開済みの SLEEF 3.9.0 ソースを指す絶対パスを `FETCHCONTENT_SOURCE_DIR_SLEEF` に指定します。または、`IRIS_SLEEF_INCLUDE_DIR`（`sleef.h` を含むディレクトリー）と `IRIS_SLEEF_LIBRARY`（互換性のある静的ライブラリー）の両方を指定できます。手動指定がダウンロードより優先され、不完全または無効なパスはエラーになります。ホスト実行時には必要な LLVM ランタイムライブラリーを検索できる必要があります。静的リンクされた SLEEF のランタイムを別途インストールする必要はありません。

Windows ARM64/ARM64EC は SLEEF に対応していないため、これらのターゲットでは `IRIS_SLEEF=OFF` を指定してください。

## AviSynth への組み込み

AviSynthMinus のネイティブアダプターは内部の静的 [ホストブリッジ](include/iris/host.h) を使用し、`MT_NICE_FILTER` を宣言します。plan、JIT コード、構築済み LUT を共有し、フレーム参照、プロパティ、context、診断は要求ごとに保持します。手動 LUT の作成時には plan が選択したバックエンドを引き継ぎます。

任意の Windows C プラグインは `IRIS_AVS=ON` でビルドし、`IRIS_AVS_INCLUDE_DIR` に SDK ヘッダーのディレクトリーを指定します。テストを有効にする場合は、既存の AviSynth DLL の絶対パスを `IRIS_AVS_RUNTIME` に指定します。プラグイン名は `IrisExpr.dll` です。`LoadPlugin` で読み込んだ後に `IrisExpr` を呼び出します。公開 AVS C ラッパーに可変状態があるため、この独立 C プラグインは引き続き `MT_SERIALIZED` を宣言します。

```avs
IrisExpr(clip, "x 2 *", backend="llvm")
IrisExpr(a, b, "x y + 0.5 *", backend="sleef")
IrisExpr(clip, "x 255 / 0.45 pow 255 *", backend="sleef-fast")
```

式は Y/U/V/A または R/G/B/A のプレーン順に指定します。空の式は先頭入力の対応するプレーンをコピーします。出力ビット深度を変更する場合は alpha も含めて明示的な式が必要です。オプションには `format`、`backend`、`scale_inputs`、`clamp_float`、`clamp_float_UV`、`optimize`、`lut`、`lut_max_mb` があります。既定ではスカラー実行、入力スケーリングなし、浮動小数点クランプなし、最適化有効、`lut=0` です。

手動の `lut=1` と `lut=2` は、それぞれ一つと二つの整数入力を必要とします。フレーム 0 のプロパティを固定してテーブルを構築し、座標、時間、相対入力アクセスを拒否します。`lut_max_mb=256` は一つのフィルターインスタンスの全出力プレーンに対するテーブル容量の合計を制限します。正の値で上限を変更し、`-1` で解除できます。超過時はこのオプションを説明するエラーを返し、実行方式を自動変更しません。NICE ワーカーはテーブルを共有し、別々に作成したフィルターインスタンスは個別のテーブルを保持します。

## テスト

単独テストは解析、数値規則、形式の混在、プロパティ、符号付き stride、メモリー境界、plan/context の寿命、並列実行、スカラーと LLVM の比較を検証します。純粋な C のテストで公開 API と内部 LUT ブリッジを確認します。任意の AVS テストはスクリプト動作、形式、メタデータ、LUT 容量制限、プラグイン読み込みを対象とし、ネイティブホストには別途 NICE 並列テストがあります。

Clang/GCC では `IRIS_SANITIZE=ON` で ASan/UBSan を有効にできます。ビルド済みの依存ライブラリーや JIT が生成する機械語は、この計装の対象外です。

## 開発と貢献

メンテナーが技術方針、変更のレビュー、リリースに責任を持ちます。不具合報告、提案、コードの貢献を歓迎します。数値仕様、インターフェース、大きなアーキテクチャー変更は、実装前に目標と方法を相談してください。

本プロジェクトでは実装、テスト、レビューに AI 支援を使用しています。貢献には問題、解決方法、検証内容、AI の関与を記載してください。問題報告にはコミット、OS、CPU、コンパイラー、ビルド設定、バックエンド、式、入出力形式、寸法、最小再現例を含めてください。コードはリポジトリーの clang-format と clang-tidy 設定に従ってください。

## 謝辞とライセンス

式言語とホスト環境を支える AviSynth、AviSynth+、AviSynthMinus とその貢献者、および任意のコンパイル・数学バックエンドを提供する LLVM と SLEEF に感謝します。

本プロジェクトの開発に使用する LLM サブスクリプションを支援してくださる [SB.SB](https://sb.sb) に感謝します。

本プロジェクトは GPL バージョン 2 以降を使用し、AviSynth リンク例外の原文と適用範囲を維持します。[LICENSE](LICENSE) を参照してください。C インターフェースの提供によって例外が拡張されることはありません。第三者の依存ライブラリーにはそれぞれのライセンスが適用されます。SLEEF の通知は [LICENSES/SLEEF.txt](LICENSES/SLEEF.txt) に含まれます。
