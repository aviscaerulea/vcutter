# avply

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)
[![Release](https://img.shields.io/github/v/release/aviscaerulea/avply)](https://github.com/aviscaerulea/avply/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/avply)](LICENSE)
[![Build](https://github.com/aviscaerulea/avply/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/avply/actions/workflows/release.yml)

会議録画を「速く・聞きやすく・必要な所だけ」見直すためのメディアプレイヤーです。
倍速再生と音声強調で視聴にかかる時間を削れます。必要な区間だけを素早く切り出せます。
起動が軽いため、会議録画に限らず普段の動画・音声再生にも使えます。

![avply スクリーンショット](docs/images/screenshot.png)

## 機能

- 高速起動：起動してから再生できるまでが速い
- 倍速再生：音程を保ったまま再生速度を 0.05 単位で変更（ファイルを切り替えても保持）
- 音声強調：話者ごとの音量のバラつきを自動で均し、小さく録れた発言を持ち上げる
- 出力デバイス追従：OS の既定の音声出力デバイスを切り替えると、再生中でも出力先が切り替わる
- シークバープレビュー：カーソルを合わせた位置のサムネイルと再生時刻をポップアップ表示
- 音声波形表示：読み込んだ音声の波形をシークバーへ重ねて描画
- 高速トリム：指定した区間を再エンコードせずキーフレーム単位で切り出し
- 変換：動画は AV1 + Opus、音声は Opus へ再エンコード

### 対応ファイル形式

| 種別 | 拡張子 |
| --- | --- |
| 動画 | mp4, mkv, mov, avi, webm |
| 音声 | mp3, wav, flac, ogg, opus |

音声ファイルを読み込んだときは、プレビュー領域を省いたコンパクトな画面へ切り替わります。

### 音声強調

会議録画は、マイクから遠い発言が小さく、近い発言が大きく録れます。
音声強調はノイズ抑制と自動ゲイン制御を組み合わせ、この差を再生しながら均します。
C キーを押すたびに ON と OFF が切り替わります。
起動時は常に OFF で、設定を保存しません。

## インストール

### 動作要件

- Windows 11
- ffmpeg（別途インストールが必要。再生時もメディア情報の取得に使用する）
- NVIDIA GPU（動画を変換するときのみ必要。AV1 NVENC 対応、RTX 30 シリーズ以降を推奨）

トリムは再エンコードしないため GPU は不要です。音声だけの変換も CPU で動作します。

### 手順

#### リリースの ZIP から

[Releases](https://github.com/aviscaerulea/avply/releases) から `avply-<version>-x64.zip` をダウンロードします。次にダウンロードしたファイルを展開します。最後に `avply.exe` を起動します。
この場合、ffmpeg は別途インストールしてください（`scoop install ffmpeg` または [公式ビルド](https://www.gyan.dev/ffmpeg/builds/)）。

#### Scoop から

Scoop が使える環境ではこちらを推奨します。依存パッケージとして ffmpeg も一緒に入ります。

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install aviscaerulea/avply
```

#### アンインストール

アプリを削除した後も、右クリックメニューで変更した設定（最前面表示など）がレジストリの `HKCU\Software\avply` に残ります。
完全に消すには、レジストリエディタでこのキーを削除してください。

## 使い方

### ファイルの読み込み

以下のいずれかの方法で読み込めます。

- 右クリックメニューの「ファイルを開く」
- ウィンドウへのドラッグ＆ドロップ
- `avply.exe` へのドラッグ＆ドロップ、Windows の「送る」、「プログラムから開く」
- コマンドラインから `avply.exe <メディアファイル>` を実行

同じファイルをもう一度読み込むと、先頭から再生し直します。

### キー・マウス操作

再生の操作は以下のとおりです。

| 操作 | キー | マウス |
| --- | --- | --- |
| 再生 / 停止 | スペース | プレビュー領域をクリック（音声のみのときは不可） |
| シーク | ← → | シークバーのドラッグ、シークバー・プレビュー領域のホイール |
| フォルダ内の前 / 次のファイルへ切替 | Alt + ← / Alt + → | |
| 再生速度 ±0.05 倍 | `.` 速く / `,` 遅く | Ctrl + ホイール |
| 音量 ±0.05 | ↑ ↓ | Shift + ホイール |
| 音声強調の切替 | C | |
| 再生条件の一括リセット | G | |

トリムの操作は以下のとおりです。

| 操作 | キー | マウス |
| --- | --- | --- |
| 区間の開始位置を指定 | `[` | 【 ボタン |
| 区間の終了位置を指定 | `]` | 】 ボタン |
| 区間のみクリア（再生位置は維持） | R | |
| トリムの実行 / 中断 | | ✂ ボタン |

G キーは 1 回目で中立の状態（速度 1.00、音量 100%、音声強調 OFF）へ、2 回目で速度と音量を起動時の設定値へ戻します。
ファイルの切り替えは、ファイル名順で先頭・末尾に達するとそこで止まります。
画面下部には、現在の再生速度・音量・音声強調の状態を常に表示します。

### トリムと変換

トリムは区間を指定してから実行します。
指定した区間は、シークバー上へ赤く表示します。
再エンコードしないため、ディスクのコピーに近い速度で保存できます。

変換は右クリックメニューの「ファイルを変換する」で実行します。
動画は AV1 + Opus、音声は Opus 96kbps へ再エンコードします。
横幅が 2048px を超える映像は、縦横比を保ったまま自動で縮小します。

### 出力ファイル

入力ファイルと同じフォルダへ `<元ファイル名>_mod.<拡張子>` として出力します。
同じ名前が既にあるときは `_mod2`、`_mod3` と番号が付きます。
`_mod` が付いたファイルをもう一度処理したときは、同じ名前へ上書きします。

| モード | 入力 | 出力拡張子 |
| --- | --- | --- |
| 変換 | 動画 | `.mp4`（AV1 + Opus） |
| 変換 | 音声 | `.opus` |
| トリム | 動画・音声 | 入力と同じ |

## 設定

実行ファイルと同じフォルダの `avply.toml` で挙動を調整します。
PC 固有の値をリポジトリの管理から外したいときは、同じフォルダの `avply.local.toml` に同じキーを書くと後勝ちで上書きします。
主要な項目は以下のとおりです。各キーの既定値と調整できる範囲は `avply.toml` 内のコメントに記載しています。

| セクション | 主な内容 |
| --- | --- |
| `[ffmpeg]` | ffmpeg.exe のパス |
| `[seek]` | カーソルキー・ホイールでのシーク量 |
| `[playback]` | 初期の再生速度、ハードウェアデコーダの優先順位 |
| `[window]` | 読込時のウィンドウサイズ上限（モニタに対する比率） |
| `[audio]` | 初期の音量、サイレンストーン |

ffmpeg のパスは、`[ffmpeg]` の `path`、Scoop の既定パス、`PATH` 環境変数の順で解決します。
Scoop か `PATH` から見つかる環境では設定不要です。
明示するときは以下のように書きます。

```toml
[ffmpeg]
path = "C:/Users/yourname/scoop/apps/ffmpeg/current/bin/ffmpeg.exe"
```

再生中の最前面表示、多重起動の抑止、プロセス優先度は、右クリックメニューの設定から切り替えます。
これらはレジストリへ保存し、次回の起動でも維持します。

## 制限事項

- トリムはキーフレーム単位で切り出すため、開始位置を指定より手前へ丸める
- Ogg/Opus など一部の形式では、トリム直後の再生位置が数十ミリ秒ずれることがある
- 音量の上限は 100% で、それを超える増幅には対応しない（小さい発言は音声強調で持ち上げる）
- 動画の変換には AV1 NVENC に対応した NVIDIA GPU が必要

## ビルド

以下のツールが必要です。

- Visual Studio 2026 Build Tools（C++ ワークロード）
- CMake `v3.25` 以上
- Qt `v6.10.3` MSVC2022 x64

Qt は以下のコマンドで導入できます。
インストール先は `CMakePresets.json` の `CMAKE_PREFIX_PATH` に合わせてください。

```powershell
python -m aqt install-qt windows desktop 6.10.3 win64_msvc2022_64 --outputdir <インストールフォルダ> --modules qtmultimedia
```

ビルドは以下のコマンドで実行します。
実行ファイルは `out/Release/avply.exe` へ出力します。

```powershell
pwsh.exe -File build.ps1
```

## ライセンス

avply 本体は GNU LGPL v3 で配布します。
全文は同梱の `LICENSE`（LGPL v3）と `COPYING`（GPL v3）を参照してください。

依存ライブラリのライセンス対応は以下のとおりです。

- Qt `v6.10`（LGPL v3）  
  DLL を動的リンクする。利用者は同名の DLL を差し替えて Qt を入れ替えられる。

- SoundTouch `v2.4.0`（LGPL v2.1 以降）  
  静的リンクのため、本体を LGPL v3 とすることで再リンク権を確保する。

- WebRTC Audio Processing（BSD）  
  静的リンクする。BSD は LGPL v3 と両立し、再配布上の追加義務は生じない。

- ffmpeg  
  外部プロセスとして呼び出すため、リンク関係は生じない。
