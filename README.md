# JhonClient

独立型 Minecraft Bedrock Edition クライアント（Windows x64）。

JhonClient は Tsukuyomi など他のクライアントに依存せず、必要な機能を独立したモジュールとして追加していく個人用プロジェクトです。

## 対象環境

- Minecraft Bedrock Edition for Windows
- 開発ターゲット: 26.60
- Windows 10/11 x64
- Visual Studio 2022 (MSVC v143)
- CMake 3.23 以上
- C++20

## 現在の構成

- 独立した DLL クライアント
- Insert キーで ClickGUI を表示
- Escape キーで ClickGUI を閉じる
- End キーでフック解除と DLL アンロード
- DirectX 11/12 オーバーレイ
- Minecraft 描画イベントと 2D HUD の分離
- モジュール登録・有効/無効・キーバインド
- JSON プロファイル
- 必須ゲームシグネチャが見つからない場合は初期化を中止
- 初期モジュール: JhonClient Watermark

戦闘・移動など、元テンプレートに含まれていたサンプルモジュールは初期構成から除外しています。

## ビルド

Visual Studio 2022 でリポジトリをフォルダーとして開き、x64-Release を選んでビルドします。

PowerShell から実行する場合:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

初回の CMake 構成時に、EnTT、libhat、MinHook、nlohmann/json を固定バージョンで取得します。初回ビルドにはインターネット接続が必要です。

出力 DLL: `build/Release/JhonClient.dll`

GitHub Actions でも Windows x64 ビルドを実行し、DLL を成果物として保存します。

## 操作

| キー | 操作 |
| --- | --- |
| Insert | ClickGUI の表示/非表示 |
| Escape | ClickGUI を閉じる |
| End | フック解除と DLL アンロード |

## 設定とログ

設定とログは `%APPDATA%\JhonClient\` 以下に保存します。

## 技術的な土台とライセンス

初期基盤の一部は [MCBE_DX12_ModBase](https://github.com/b01dev/MCBE_DX12_ModBase) を参考・改変しています。元プロジェクトの著作権表示を保持し、同梱・取得するライブラリのライセンス情報も記録しています。詳細は `LICENSE` と `THIRD_PARTY_NOTICES.md` を参照してください。

## 注意

本プロジェクトは個人の検証・学習用です。自分が管理する環境、または利用が許可された環境でのみ使用してください。ゲームやサーバーの規約・ルールに従ってください。JhonClient はアンチチート回避、権限回避、保護機構の無効化を目的としません。
