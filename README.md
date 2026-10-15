# TimeAcquisition (AviUtl2用 フレーム・時間取得プラグイン)

AviUtl2のタイムラインやオブジェクト上で右クリックした際に、現在のフレーム数や秒数をクリップボードに素早くコピーできる汎用プラグイン（`.aux2`）です。

---

## 主な機能

1. **タイムライン全体／オブジェクト基準の選択**
   - **タイムライン全体**: 現在のカーソル位置のフレーム数・秒数・タイムコードを取得します。
   - **オブジェクト基準**: 選択中または右クリックしたオブジェクトの開始位置からの相対フレーム数・相対秒数を取得します。
   - **オブジェクトの長さ**: オブジェクトの総フレーム数や総秒数もワンクリックでコピー可能です。

2. **秒数／フレーム数の選択**
   - **フレーム数**: フレーム番号をそのまま数値でコピーします（設定により1始まり/0始まりの切り替え可能）。
   - **秒数**: フレームレート（FPS）から正確に算出した秒数をコピーします（小数桁数の指定や "s" 単位の付与が可能）。
   - **タイムコード**: `00:01:23.456`（時:分:秒.ミリ秒）形式でのコピーもサポートしています。

3. **整理された右クリックメニュー**
   - **位置・時間コピー サブメニュー**:
     - `タイムライン全体: フレーム数`
     - `タイムライン全体: 秒数`
     - `タイムライン全体: タイムコード`
     - `オブジェクト基準: 相対フレーム数`
     - `オブジェクト基準: 相対秒数`
     - `オブジェクト: 長さ(フレーム数)`
     - `オブジェクト: 長さ(秒数)`
     - `設定...`

4. **専用設定ダイアログ**
   - メニュー内の「設定...」またはAviUtl2のシステムメニューから、UI上で直感的に設定を変更・保存できます（設定は `TimeAcquisition.ini` に自動保存されます）。

---

## 設定項目

| 項目 | 選択肢 / 説明 | 初期値 |
| :--- | :--- | :--- |
| **フレーム数の起算点** | **1始まり** (AviUtlのUI表示基準) / **0始まり** (プログラム内部基準) | 1始まり |
| **秒数の小数桁数** | 2桁 (例: `1.23`) / 3桁 (ミリ秒: `1.234`) / 4桁 (例: `1.2345`) | 3桁 |
| **秒数の単位付与** | 秒数の末尾に `"s"` を付けるか（例: `1.234s`） | なし (数値のみ) |
| **コピー時の通知** | **通知なし** (静かにクリップボードに格納) / メッセージボックス表示 | 通知なし |

---

## ファイル構成

- [TimeAcquisition.cpp](file:///c:/ProgramData/aviutl2/Plugin/自作/TimeAcquisition/TimeAcquisition.cpp): プラグインのソースコード
- [TimeAcquisition.def](file:///c:/ProgramData/aviutl2/Plugin/自作/TimeAcquisition/TimeAcquisition.def): DLLエクスポート定義ファイル
- [TimeAcquisition.aux2](file:///c:/ProgramData/aviutl2/Plugin/自作/TimeAcquisition/TimeAcquisition.aux2): ビルド済みプラグインバイナリ
- [build.bat](file:///c:/ProgramData/aviutl2/Plugin/自作/TimeAcquisition/build.bat): MSVC自動検出・ワンクリックビルドスクリプト
- `TimeAcquisition.ini`: 設定ファイル (設定変更時に自動生成)

---

## インストール方法

1. `TimeAcquisition.aux2` を AviUtl2 の `Plugin` フォルダ（または `Plugin\自作` などのサブフォルダ）に配置します。
2. AviUtl2 を起動すると自動的に読み込まれます。
3. タイムライン上で右クリックするとメニュー項目「位置・時間コピー」が表示されます。

---

## 再ビルド方法

`build.bat` をダブルクリックして実行するか、Visual Studio の「x64 Native Tools Command Prompt」で以下を実行します：

```cmd
cl /nologo /O2 /LD /std:c++17 /utf-8 /EHsc TimeAcquisition.cpp /I"." /link /DEF:TimeAcquisition.def /OUT:TimeAcquisition.aux2 user32.lib gdi32.lib comctl32.lib
```
