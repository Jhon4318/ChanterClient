# JhonClient

独立型 Minecraft Bedrock Edition クライアント（Windows x64）。

JhonClient は Tsukuyomi など他のクライアントに依存せず、必要な機能を独立したモジュールとして追加していく個人用プロジェクトです。

## 対象環境

- Minecraft Bedrock Edition for Windows
- 開発ターゲット: 26.60
- Windows 10/11 x64
- Visual Studio 2022 (MSVC v143)
- CMake 3.23+
- C++20

## 初期方針

- DLL としてロードされる独立クライアント
- Insert キーで ClickGUI を表示
- End キーでフックを解除してアンロード
- DirectX オーバーレイと Minecraft の描画イベントを分離
- モジュールの有効/無効、キー割り当て、JSON プロファイル
- バージョン不一致や必須シグネチャ未検出時は初期化を中止
- 既存クライアントのサンプル戦闘/移動機能は初期構成に含めない

## ビルド

Visual Studio 2022 でリポジトリをフォルダーとして開き、x64-Release を選んでビルドします。

または PowerShell から:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

出力 DLL: `build/Release/JhonClient.dll`

## 操作

| キー | 操作 |
| --- | --- |
| Insert | ClickGUI の表示/非表示 |
| Escape | ClickGUI を閉じる |
| End | フック解除と DLL アンロード |

## 設定とログ

設定とログは `%APPDATA%\\JhonClient\\` 以下に保存します。

## 技術的な土台とライセンス

初期基盤の一部は [MCBE_DX12_ModBase](https://github.com/b01dev/MCBE_DX12_ModBase) を参考・改変しています。元プロジェクトおよび同梱ライブラリの著作権・ライセンス表記を保持します。詳細は `THIRD_PARTY_NOTICES.md` を参照してください。

## 注意

本プロジェクトは個人の検証・学習用です。自分が管理する環境、または利用が許可された環境でのみ使用してください。ゲームの利用規約やサーバー独自ルールに従ってください。JhonClient はアンチチート回避、権限回避、保護機構の無効化を目的としません。
