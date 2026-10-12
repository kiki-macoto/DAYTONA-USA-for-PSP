<!--
  README のテンプレート（PSP 版）。<...> の部分を書き換えてください。
  このファイルを README.md として使うときは、元の README.md と入れ替えてください。
-->

# Daytona USA Recomp for PSP

アーケード版『デイトナUSA』（SEGA Model 2、1994年 Revision A）を、
エミュレーションを使わずに **PSP のネイティブコードとして動かす** 非公式の移植です。

<!-- スクリーンショット（ROM 由来の画像なので、載せるかどうかは自分で判断してください） -->
　　　　　　<img width="480" height="272" alt="download" src="https://github.com/user-attachments/assets/7c152217-cc77-43e6-b1f4-e62a24ac6afa" />

> [!IMPORTANT]
> 一部AIを使用しています。
> 非公式のファンプロジェクトで、SEGA とは関係ありません。
> ゲームのデータ（ROM）はこのリポジトリに含まれていません。各自が持っている ROM からビルドしてください。
> ビルドしてできたファイル（`EBOOT.PBP`、`rom/`）は ROM を変換したものを含むため、配布しないでください。

## 仕組み

基板の CPU（i960）、3D 演算用プロセッサ（TGP）、サウンド用の 68000 のプログラムを、
あらかじめ C++ に変換（静的リコンパイル）し、PSP 用にコンパイルしています。
描画は PSP の GPU（GE）、音声は PSP の音声出力で鳴らします。

このプロジェクトは [daytona-arcade-recomp](https://github.com/alphanu1/daytona-arcade-recomp) の PSP 移植です。

## 現在の状態

<!-- 実機で測ったら更新してください -->

| 項目 | 状態 |
| --- | --- |
| 動作確認 | PSP-3000（ARK-5、333MHz）、PPSSPP 1.19.3 |
| 速度 | レース中は1フレーム約23ms（アーケードは17.4ms）。アーケードの約75%の速さです |
| 描画 | 2フレームに1回。重い場面では3〜4フレームに1回に自動で落とします |
| 音声 | あり |
| 車の台数 | 初級20台、中級10台、上級10台（アーケードは40、20、30台）。速度のために減らしています |
| セーブ | 設定と記録を `save/` に保存します |

### 既知の問題

- まだアーケードの速度に届いていません（高速化を続けています）
- 333MHz を超えるクロックは、ARK-5 では設定できませんでした
- まだ安定していません。導入は自己責任でお願いします。

## 必要なもの

- PSP-2000 / 3000 / Go と、カスタムファームウェア（ARK-4、ARK-5 などで確認）
  - PSP-1000 はメモリが足りないため動きません
- 自分で用意した『デイトナUSA』Revision A の ROM セット（MAME の `daytona`）
- ビルドする PC：Linux、または Windows の WSL（Ubuntu 24.04 で確認）

## ビルド

詳しい手順は [HOW_TO_COMPILE_PSP.md](HOW_TO_COMPILE_PSP.md) を見てください。概要は次のとおりです。

```sh
# 1. 必要なパッケージ
sudo apt install -y git python3 cmake make g++ ninja-build

# 2. pspdev（PSP 用のコンパイラ）を ~/pspdev に入れる
wget https://github.com/pspdev/pspdev/releases/latest/download/pspdev-ubuntu-latest-x86_64.tar.gz
tar xf pspdev-ubuntu-latest-x86_64.tar.gz -C ~
export PSPDEV=~/pspdev PATH=$PATH:~/pspdev/bin

# 3. リポジトリを取得して、ROM を roms/ に置く
git clone <このリポジトリの URL>
cd <リポジトリのフォルダ>
cp /path/to/daytona.zip roms/

# 4. ビルド
python3 platform/psp/build_psp.py host   # ROM のプログラムを C++ に変換（初回とリポジトリの更新後）
python3 platform/psp/build_psp.py game   # PSP 用にビルド
```

できあがるのは `build-daytona/psp/DAYTONA/` フォルダです。

## インストール

`build-daytona/psp/DAYTONA` フォルダを、メモリースティックの `PSP/GAME/` にコピーします。

```
PSP/GAME/DAYTONA/
├── EBOOT.PBP      ゲーム本体
├── rom/           ROM から変換したデータ
├── settings.txt   設定（任意）
├── overlay.bin    オーバーレイ画像（任意）
└── save/          セーブ（ゲームが作ります）
```

更新するときは `EBOOT.PBP` と `rom/` を上書きしてください。`save/` と `settings.txt` はそのまま残ります。

## 操作

| PSP | アーケード |
| --- | --- |
| アナログスティック（左右） | ハンドル |
| × / □ | アクセル / ブレーキ |
| R / L | シフトアップ / ダウン（1〜4速） |
| 十字キー（上・右・下・左） | 視点切り替え（VR1〜VR4） |
| START | スタート（クレジットがなければコインも入れます） |
| START + × | タイムアタック |
| SELECT（単独で押して離す） | コイン |
| SELECT + L / SELECT + R | テストモード / サービス |

## 設定（settings.txt）

`PSP/GAME/DAYTONA/settings.txt` の行頭の `#` を消すと有効になります。

```
cpu 333
```

- `cpu`：CPU クロック（222〜444MHz）。書かなければ 333MHz で動きます
  - CFW のオーバークロックで 333MHz より高くしてある場合は、そのクロックを使います
- 実際に動いたクロックは `daytona.log` の1行目に出ます

## オーバーレイ（ベゼル）

ゲーム画面の左右の黒い部分に、好きな画像を表示できます。処理は重くなりません。

```sh
python3 platform/psp/build_psp.py game --overlay 画像.png
```

- 透明な部分（または白い部分）がゲーム画面の位置になります
- できた `overlay.bin` を `EBOOT.PBP` と同じフォルダに置くと表示され、消すと元に戻ります
- PNG のみ対応です
- おまけで、実機風のオーバーレイがリポジトリにあらかじめ含まれています。

## 不具合の報告

<!-- Issues の URL など -->
`PSP/GAME/DAYTONA/daytona.log` を添えて報告してください。

## ライセンス

このプロジェクトのコードは [BSD-3-Clause ライセンス](LICENSE) です。
他のプロジェクトから取り込んだコードは、それぞれのライセンスに従います
（[THIRD_PARTY.md](THIRD_PARTY.md)、[platform/psp/THIRD_PARTY.md](platform/psp/THIRD_PARTY.md)）。

ライセンスは『デイトナUSA』そのものには及びません。
Daytona USA および SEGA は株式会社セガの商標です。

## クレジット

- 元のプロジェクト：[daytona-arcade-recomp](https://github.com/alphanu1/daytona-arcade-recomp)（Ben Templeman and contributors）
- PSP 移植：木々　諒（Makoto Kiki）
- [MAME](https://github.com/mamedev/mame)：CPU や描画の仕組みの参考と、一部コードの移植（BSD-3-Clause）
- [pspdev](https://github.com/pspdev)：PSP 用のコンパイラと SDK
- [ymfm](https://github.com/aaronsgiles/ymfm)、[Berkeley SoftFloat](https://github.com/ucb-bar/berkeley-softfloat-3)、[LZMA SDK](https://www.7-zip.org/sdk.html)
