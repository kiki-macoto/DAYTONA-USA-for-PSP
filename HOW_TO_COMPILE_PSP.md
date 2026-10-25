# PSP 版のビルド方法

PSP 版は、リコンパイルしたゲーム（デイトナUSA、1994年 Revision A）を、
カスタムファームウェアを入れた PSP-2000 / PSP-3000 / PSP Go で動かします。
まだ開発中です。PSP-3000（ARK-5）のオーバークロック設定（実測約450MHz）で、レース中は約40〜57fps です
（アーケードは57.52fps。重い場面ほど下がります）。
車の台数は、どのコースも10台です（アーケードは初級40、中級20、上級30台）。
詳しくは [platform/psp/README.md](platform/psp/README.md) と
[platform/psp/HANDOFF.md](platform/psp/HANDOFF.md) を見てください（どちらも英語）。

このリポジトリに ROM のデータは含まれていません。ビルド中にダウンロードすることもありません。
ROM は各自で用意してください。ビルドしてできるフォルダには ROM から取り出したデータが入るので、
共有しないでください。

## 必要なもの

* **Linux**、または **Windows の WSL**（Ubuntu 24.04 で確認）。
  Git、Python 3、CMake、Make、C++20 対応のコンパイラ（GCC 13 以降）が必要です。

  ```sh
  sudo apt update
  sudo apt install -y git python3 cmake make g++ ninja-build
  ```

  リポジトリは **WSL のファイルシステムの中**（例：`~/daytona-arcade-recomp`）に置いてビルドしてください。
  `/mnt/c` の下ではビルドがずっと遅くなります。
  最初の変換では大きな生成ファイルを2つずつコンパイルし、それぞれ 1GB 以上のメモリを使います。
  WSL には 6GB 以上のメモリを割り当ててください。

* **[pspdev](https://github.com/pspdev/pspdev)**（psp-gcc と PSPSDK）。
  `~/pspdev`、または環境変数 `PSPDEV` が指す場所に入れます。GCC 15.2 で確認しました。
  配布されているビルド済みのものに、このビルドが使うライブラリ
  （`pspsystemctrl_user` と `pspkubridge` を含む）がすべて入っています。

  ```sh
  cd ~
  wget https://github.com/pspdev/pspdev/releases/latest/download/pspdev-ubuntu-latest-x86_64.tar.gz
  tar xf pspdev-ubuntu-latest-x86_64.tar.gz      # ~/pspdev ができます
  echo 'export PSPDEV=~/pspdev' >> ~/.bashrc
  echo 'export PATH=$PATH:$PSPDEV/bin' >> ~/.bashrc
  source ~/.bashrc
  psp-gcc --version
  ```

* **自分で用意した Revision A の ROM セット**（MAME の `daytona`）。
  リポジトリの `roms/daytona.zip`（または `.7z`）に置きます。
  ほかのバージョン（`daytona93` など）は PSP 版では使いません。

* 動かす環境：**カスタムファームウェアを入れた PSP-2000 / 3000 / Go**
  （PSP-3000 の ARK-5 で確認）、または PC の **[PPSSPP](https://www.ppsspp.org)**（1.19.3 で確認）。
  PSP-1000 はメモリが足りないため動きません。

## ビルド

Linux または WSL で、リポジトリのフォルダから実行します。

```sh
git clone <このリポジトリの URL>
cd <リポジトリのフォルダ>
cp /path/to/daytona.zip roms/

# 1. PC 側の準備。初回と、リポジトリを更新したあとに実行します。
#    固定したバージョンの外部ソース（SoftFloat、ymfm、LZMA SDK）を extern/ に取得し、
#    build-daytona/ を設定して、ROM のプログラムを C++ に変換します。
python3 platform/psp/build_psp.py host

# 2. PSP 用のプログラムと、メモリースティックにコピーするフォルダを作ります。
python3 platform/psp/build_psp.py game
```

できあがるのは **`build-daytona/psp/DAYTONA/`** フォルダです。

| ファイル | 内容 |
| --- | --- |
| `EBOOT.PBP` | ゲーム本体 |
| `rom/` | ROM から取り出したデータ（共有しないでください）と、1台筐体として起動するための初期設定 |
| `settings.txt` | 設定（CPU クロック）。最初はすべての行がコメントになっています |

`game` のオプション：

| オプション | 内容 |
| --- | --- |
| `--jobs N` | PSP 側で同時に行うコンパイルの数（標準は CPU の数。PC 側は最大2） |
| `--draw-every N` | 最初に何フレームに1回描画するか（1〜4、標準は2）。処理が間に合わないときは、ゲームが自動で描画の回数を減らします |
| `--overlay PNG` | 画面の左右に表示する画像（筐体のベゼルなど。下の説明を見てください） |
| `--deploy DIR` | できたフォルダを `DIR/DAYTONA` にコピーします。メモリースティックの `PSP/GAME` や、PPSSPP のフォルダ（WSL では `/mnt/c/Users/<ユーザー名>/Documents/PPSSPP/PSP/GAME`）を指定します |
| `--ppsspp EXE` | `--deploy` と一緒に使います。PPSSPP を起動して、ゲームのログを表示します（WSL では `"/mnt/c/Program Files/PPSSPP/PPSSPPWindows64.exe"`） |

`PROFILE_TGP=1 python3 platform/psp/build_psp.py game` でビルドすると、
CPU のハードウェアカウンタと処理ごとの時間をログに出す計測用のビルドになります。
約2〜3fps 遅くなるので、計測のときだけ使ってください。

PSP 側は `build_psp.py` が Makefile（`platform/psp/Makefile`）を実行してビルドします。
フロントエンド（`platform/psp/game/`）だけを変更したときは、もう一度 `game` を実行すれば、
変更した部分だけがビルドし直されます。

## インストール

1. `build-daytona/psp/DAYTONA` を、メモリースティックの `PSP/GAME/` にコピーします。
   `PSP/GAME/DAYTONA/EBOOT.PBP` となるように置いてください。
2. XMB から「Daytona USA」を起動します。

ゲームは `EBOOT.PBP` と同じフォルダに `daytona.log` を書き出し、設定と記録を `save/` に保存します。
更新するときは、`EBOOT.PBP` と `rom/` を上書きしてください。`save/` はそのまま残ります。
`settings.txt` は自分で書き換えた内容が消えるので、ひな形に戻したいときだけコピーしてください。

## CPU クロック

標準では、PSP の通常の最高クロックである 333MHz で動きます。
カスタムファームウェアのオーバークロックで、すでにそれより高いクロックになっている場合は、そのまま使います。
クロックを指定するには、`PSP/GAME/DAYTONA/settings.txt` に次の行を書きます。

```
cpu 333
```

222〜444 の値を指定できます。バスのクロックは CPU の半分になります。
333MHz を超えるクロックはカスタムファームウェアに設定を頼みます（`sctrlHENSetSpeed`）が、
設定されないこともあります。PSP-3000 の ARK-5 では、440 を指定しても 333MHz のままでした。ARK-5 自身のオーバークロック設定を使うと、実測で約450MHz で動きました（電源の報告上は 333MHz のまま）。
実際に動いたクロックとその理由は、`daytona.log` の1行目に出ます。

## メディアエンジン（ME）

音声のミキシングは、PSP の2つ目のプロセッサであるメディアエンジンが行い、メイン CPU の負担を減らします。
このときの音声は 44.1kHz です。ME が応答しないとき（PPSSPP など）は、自動でメイン CPU が 32kHz でミキシングします。
ME を使わないようにするには、`settings.txt` に次の行を書きます。

```
me 0
```

ME の起動手順は、mcidclan さんの [psp-media-engine-custom-core](https://github.com/mcidclan/psp-media-engine-custom-core)（MIT ライセンス）を参考にしています。
ME を使うとシステム側の ME の処理が止まるので、ゲーム中に PSP をスリープさせないでください。

## オーバーレイ（ベゼル）

ゲームの画面は、PSP の 480×272 の画面のうち中央の 351×272 を使います。
左右の黒い部分に、好きな画像を表示できます。

```sh
python3 platform/psp/build_psp.py game --overlay my_bezel.png
```

`EBOOT.PBP` と同じフォルダに `overlay.bin` が作られ、このファイルがあるとゲームが表示します。
ファイルを消すと、元の黒い余白に戻ります。メモリースティックには、ほかのファイルと一緒に `overlay.bin` もコピーしてください。

* 画像は、1チャンネル8ビットの PNG にしてください。ほかの形式（WebP、JPEG など）は、先に PNG に変換してください。
* 画像の透明な部分、透明な部分がなければ白い部分が、ゲーム画面の位置になります。
  どちらでもうまくいかないときは、`platform/psp/scripts/make_overlay.py` に `--window X0,Y0,X1,Y1` で位置を指定してください。
* 画像は、ゲーム画面の位置が画面の高さいっぱいになるように拡大・縮小して、中央に置きます。
  画面からはみ出した部分は切れます。16:9 の画像に 4:3 の窓があるものがちょうど合います。
* 画像は起動時に一度だけ画面に書き込むので、ゲーム中の処理は重くなりません。

## 操作

| PSP | アーケード |
| --- | --- |
| アナログスティック（左右のみ） | ハンドル |
| × / □ | アクセル / ブレーキ |
| R / L | シフトアップ / ダウン |
| 十字キー | 視点切り替え |
| START | スタート（クレジットがなければコインも入れます） |
| SELECT | コイン |
| SELECT + L / SELECT + R | テストモード / サービス |

## 困ったとき

* **`put your own Revision A ROM set at roms/daytona.zip` と出る**：ROM セットがないか、名前が違います。
  MAME の `daytona` セットを使ってください。
* **`psp-g++: not found` と出る**：pspdev が `PATH` に入っていないか、`PSPDEV` が設定されていません（上の手順を見てください）。
* **`host` の途中で WSL が止まる**：WSL のメモリが足りません。
  `%UserProfile%\.wslconfig` に `[wsl2]` と書き、次の行に `memory=8GB` と書いて割り当てを増やしてから、
  もう一度 `host` を実行してください。止まったところから続きます。
* **`build-daytona/psp` から消したファイルが作り直されない**：
  Makefile は途中のファイルを追跡しない設定（`.SECONDARY`）になっています。
  元のソースファイルを更新する（touch する）か、`build-daytona/psp/game` を消して PSP 側をすべてビルドし直してください。
* **PPSSPP で遅すぎる、または速すぎる**：PPSSPP の速度は実機とは違います（PSP のキャッシュを再現していません）。
  速度は実機で確かめてください。
