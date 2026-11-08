# ContainerTransfer 実装メモ

## 目的

Minecraft Bedrock 1.26.60 / ChanterClient に、プレイヤーがコンテナブロックを手動でクリックしたときだけ動作する一括アイテム移動機能を追加する。

- **取り出し元**: 指定したブロック種類をクリックすると、指定アイテムをコンテナからプレイヤーのインベントリへ回収する。
- **詰め込み先**: 指定したブロック種類をクリックすると、指定アイテムをプレイヤーのインベントリからコンテナへ収納する。
- コンテナ画面は表示しない。内部のコントローラーはゲームスレッド上で処理・tickする。
- アイテム移動はゲーム本来のコンテナ操作経路を使い、インベントリメモリを直接書き換えない。

## 参考実装の確認結果

アップロードされた Tsukuyomi の `ShulkerStore.cpp` / `ShulkerStore.h` / `ContainerUi.cpp` / `Detours.cpp` を確認した。

参考実装から再利用する考え方:

1. コンテナ画面の生成を検出し、コントローラーを取得する。
2. 対象画面の scene stack push だけを抑止し、画面を stack 外に保持する。
3. 保持した画面の controller tick をゲームスレッド上で呼び続ける。
4. コンテナ内スロットとプレイヤーインベントリのスロットを列挙する。
5. native の state-machine click 経路で shift-click 相当の操作を要求する。
6. ItemStackNetManager の予測タグが解消するまで待ち、移動結果を確認してから画面を閉じる。

注意: 参考 `ShulkerStore.cpp` は、登録した座標のシュルカーへ、プレイヤーインベントリ内の対象アイテムを自動収納する一方向機能である。今回必要な手動クリック起動・取り出し・収納先切り替えは別途実装する。

## ChanterClient 現状の不足

現行 ChanterClient には DirectX / ImGui のモジュール基盤、ClientInstance / LocalPlayer / GameMode の一部 SDK、MinHook がある。一方で以下が未実装:

- GameMode の block-use interaction hook
- Block / BlockType の安全な識別
- ContainerScreenController の生成・破棄検出
- SceneStackPush / ContainerScreenTick のフック
- off-stack screen の保持・tick・release
- ContainerManager / collection / slot state-machine のラッパー
- コンテナ間 ItemStackRequest の予測状態監視

そのため、単に既存の SkinStealer のフックへ処理を足すだけでは実現できない。

## クリック検出

LeviLamina の Bedrock SDK 定義では、GameMode の virtual function 順で `useItemOn` は slot 13。メソッド引数は ItemStack、BlockPos、face、hit Vec3、HandSlot、対象 Block、first-event flag。戻り値 `InteractionResult` は success / swing の bit field である。

ChanterClient の GameMode 宣言は古い簡略形なので、フックを追加する前に vtable と呼び出し規約を 1.26.60 実行ファイルで照合する。first-event の手動入力のみを対象にし、simulation tick による重複処理はしない。

Block SDK の公開定義では Block の `mBlockType` が `mComponents` の後にある。実装では型名の取得に未検証のオフセットを無条件使用せず、メモリ検査・対象 build 固有の layout 検証を通す。失敗時は通常のゲーム操作を妨げず、ContainerTransfer を実行しない。

## 処理ステート

`Idle -> AwaitingContainer -> HiddenOpen -> Moving -> AwaitingServerConfirm -> Closing -> Idle`

- クリック対象が source / destination のいずれにも一致しなければ何もしない。
- 画面コントローラーが確認できない場合、画面スタックへの操作を抑止しない。
- cursor stack にアイテムがある場合は開始しない。
- 取り出しでは、指定アイテムの source container slot を対象に、インベントリ空き容量を考慮して一括回収する。
- 収納では、指定アイテムの player inventory slot を対象に、destination container の空き容量・既存 stack 上限を考慮する。
- 1 tick あたりの操作数を制限し、クリック済み slot を各 pass で記録する。
- server response / prediction tag の解消を待つ。タイムアウト時はそれ以上の操作を行わず閉じる。
- 途中で別画面が開かれた場合、world が切り替わった場合、controller の検査に失敗した場合は安全に中止する。
- 通常のチェスト UI、村人 UI、クラフト UI など無関係の画面を隠さない。対象コンテナ画面と確認できない限り original scene push を実行する。

## 設定 UI

Tools カテゴリに独立した `ContainerTransfer` module を追加する。

- 取り出し元ブロック ID（複数。例: `minecraft:chest,minecraft:barrel`）
- 詰め込み先ブロック ID（複数。例: `minecraft:shulker_box,minecraft:white_shulker_box`）
- 移動アイテム ID（複数。例: `minecraft:iron_ingot,minecraft:gold_ingot`）
- 空欄は全アイテム指定として解釈しない。誤操作防止のため対象アイテムが 1 件以上設定されるまで動作させない。

設定は既存 ConfigManager の JSON 保存・読み込みを利用する。

## 実装順序

1. 1.26.60 GameMode::useItemOn の呼び出し規約と block identity を検証する。
2. 手動クリック対象の capture hook を追加する。
3. ContainerScreen ctor / tick / scene stack の exact signature と呼び出し経路を特定する。
4. 対象 screen だけ off-stack に保持し、controller を tick できるようにする。
5. ContainerUi の必要最小 API（collections, slots, cursor, click, prediction status）を ChanterClient に実装する。
6. source withdrawal / destination deposit を別々の pass として実装する。
7. offline compile、Actions build、Minecraft 1.26.60 の実機テストで source / destination / full inventory / full container / timeout / unrelated screen を確認する。

## 未完了・安全上の注意

このメモは調査と実装設計であり、ContainerTransfer の動作コードや実機検証を完了したという意味ではない。特に SceneStackPush / ContainerScreenCtor / ContainerScreenTick の exact signature とコントローラー ABI が未検証のまま、画面を抑止するコードを入れないこと。誤ったフックはメニュー表示停止やクラッシュにつながる。
