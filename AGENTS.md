# AGENTS.md（岡ゼミ版）

このリポジトリを AI コーディングエージェント（Codex 等）で扱うときの前提です。
**作業を始める前に必ず読んでください。**

`firmware/AGENTS.md` に本家のコーディング規約があります。本ファイルはそれに**優先する**、
岡ゼミ版固有の制約と、実機で確認済みの「触ってはいけない箇所」をまとめたものです。

---

## プロジェクトの目的と制約

- **目的**: M5Stackchan（M5Stack CoreS3）で音声対話。話しかけると Gemini が音声で返答する。
- **構成**: Gemini Live API（WebSocket）。音声 in/out を直接やりとりし、STT/TTS は使わない。
  - PlatformIO env: `m5stack-cores3-realtime`
  - SD の `SC_ExConfig.yaml` で `llm.type: 3`
- **制約（厳守）**
  - **有料サービス禁止。** Gemini の無料枠のみで完結させる。課金の有効化は選択肢に入れない。
  - **追加ハードの購入禁止。**
  - 開発者は**プログラミング初学者**。変更理由を日本語で説明する。
- Wi-Fi は**スマホのテザリング**。大学 Wi-Fi はキャプティブポータルのため ESP32 では接続できない。

---

## ⚠️ 絶対に変更してはいけない箇所

過去に実機で問題を起こし、原因特定に数日かかった設定です。
**「新しくする」「整理する」目的で触らないでください。**

### 1. ライブラリのバージョン（`firmware/platformio.ini`）

```ini
m5stack/M5Unified @ 0.2.15
M5GFX=https://github.com/m5stack/M5GFX.git#0.2.22
```

> **⚠️ M5GFX は GitHub のタグを直接指定している。レジストリ指定に戻さないこと。**
> **PlatformIO のレジストリからは 0.2.21 も 0.2.22 も既に取得できない**
> （2026年9月時点で残っているのは 0.2.27 / 0.2.28）。
> レジストリ指定に戻すと、**新規環境でビルドが失敗する**（実際に失敗を確認済み）。
> 0.2.22 は実機で動作確認済みの版で、**GitHub のタグからのみ入手できる。**

本家は M5Unified 0.1.17 かつ M5GFX 無指定。CoreS3 では、この不整合により
**「画面が極端に暗い」「タッチが全く効かない」「首が動かない」が同時に発生**する。

新しい M5GFX は本機を `board_M5StackChan` と検出するが、古い M5Unified はその board を
知らないため `M5.Touch` と `M5.Power`（バックライト = AXP2101 BLDO1）の初期化が噛み合わず、
**パネル描画（SPI）だけ動いてタッチとバックライト（I2C）が死ぬ。**

> **「最新版にアップデートしましょう」は禁止。** この組み合わせは総当たりで確定させたもの。

### 2. 書き込み速度（`firmware/platformio.ini`）

```ini
upload_speed = 460800
```

本家の `1500000` は、**書き込み途中で USB が再列挙されて本体が起動しなくなる**事故を
実際に起こした値。復旧には手動 Download Mode が必要。**上げないこと。**

### 3. Gemini Live のリクエスト形式（`firmware/src/llm/Gemini/GeminiLive.cpp`）

| 箇所 | 正しい形 | 間違えるとどうなるか |
|---|---|---|
| `responseModalities` | **指定しない** | ネイティブ音声モデルでは `code=1007` で切断される |
| 音声の mime | `"mimeType": "audio/pcm;rate=16000"` | `mime_type`（snake_case）だと**返事が返らない** |
| 認証 | URL クエリ `?key=` | ヘッダ `x-goog-api-key` だと接続直後に切断される |
| `realtimeInputConfig` | **追加しない** | 内蔵マイクでは発話開始が検知されず `turnComplete` が返らなくなる |
| `tools` の `googleSearch` | **残す** | 消すと動作実績のある構成が崩れる |

### 4. 音声再生（`firmware/src/llm/RealtimeLLMBase.cpp` / `.h`）

```cpp
while (M5.Speaker.isPlaying(0) > 1) { vTaskDelay(1); }
M5.Speaker.playRaw((int16_t*)buf, len/2, 24000, false, 1, 0);   // ch0 固定
```

- `playRaw()` の第6引数 `channel` の既定は **-1（空きチャンネルへ自動割り当て）**。
  省略すると音声チャンクが別々のチャンネルで**同時に鳴り**、早口かつギザギザに聞こえる。
- `AUDIO_BUF_NUM = 3`。`playRaw()` は**バッファをコピーせずポインタのまま再生する**ため、
  2枚だと再生中の面を上書きして音が飛ぶ（M5Unified のヘッダにも3枚使用が明記されている）。

> **この2つは両方必要。** バッファだけ3枚にしても、チャンネルが -1 のままでは症状は変わらない。

### 5. `Avatar::setSpeechText()` に渡す文字列

**文字列をコピーせずポインタを保持する実装**。渡してよいのは文字列リテラルか static バッファのみ。
`String::c_str()` を渡すとダングリングする。
また吹き出しは**全角8文字が上限**（efontJA_16 を TEXT_SIZE=2 で描画、全角1文字 = 32px）。

---

## ⚠️ 不具合調査の手順（最重要）

**症状からコードを推測して直す前に、必ず無料枠を確認すること。**

https://aistudio.google.com/app/usage

過去に**丸一日を溶かした罠**がある。Gemini Live は**無料枠を使い切っても「枠切れ」と分かる
エラーを返さない。** 実際に出るのは次のような、設定バグにしか見えないメッセージ。

| 実際に出た表示 | 本当の意味 |
|---|---|
| `code=1007 The audio content type (CONTENT_TYPE_AUDIO) is not supported for this model configuration` | **枠切れ**（設定は正しい） |
| `code=1011 Internal error occurred.` | **枠切れ** |
| `[WSc] modelTurn without audio` の連発 | **枠切れ** |
| 1ターン目だけ成功し2ターン目から無応答 | **枠切れ** |

判定方法：使用状況ページで **429 / 500** が出ていれば枠切れ。
また `setupComplete` が出ているなら、接続・キー・モデル名は正しい。

> この罠で、VAD 設定・関数呼び出し・プロンプト長・mime 形式・googleSearch の有無を
> 順に疑って**すべて外した**。同じ回り道をしないこと。

---

## 使えるモデル

`bidiGenerateContent` に対応しているのは以下のみ。他を指定すると `code=1008` で切断される。

```
models/gemini-2.5-flash-native-audio-latest          ← 現在の設定
models/gemini-2.5-flash-native-audio-preview-09-2025
models/gemini-2.5-flash-native-audio-preview-12-2025
models/gemini-3.1-flash-live-preview
```

---

## ビルドと書き込み

`pio` は PATH に無いことが多いのでフルパスで呼ぶ。**COM ポートは挿し直すと変わる。**

```powershell
# COMポートの確認（Bluetooth ではなく「USB シリアル デバイス」が本機）
Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'COM\d+' } | Select-Object Name

# ビルド＆書き込み（パスは各自の環境に読み替える）
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e m5stack-cores3-realtime -t upload --upload-port COM5
```

- シリアルモニターが COM を掴んでいると**書き込みが失敗する**。事前に閉じる。
- `firmware/.pio/` は自動生成物（約1GB）。`.gitignore` 済み。**コミットしないこと。**
### ⚠️ 初回ビルド後にパッチを当てること

`[WSc][CLOSE]` の切断理由ログは **WebSockets ライブラリ側の改変**で出している。
ライブラリは `.pio/libdeps`（自動ダウンロード領域）にあるため配布物に含まれず、
**ビルドのたびに元へ戻る。** 初回ビルド後に次を実行すること。

```powershell
powershell -ExecutionPolicy Bypass -File my_script\patch_websockets_close_log.ps1
```

何度実行しても安全（適用済みなら何もしない）。実行後に再ビルドで反映。
当てないと、**モデル名不正・権限エラー・枠切れの切断理由が一切見えない。**

- **`.pio/libdeps/` 配下を直接編集しても、`pio pkg update` やクリーンビルドで消える。**
  恒久的な修正は `firmware/src/` か `firmware/lib/` に置く。

---

## シリアルログの読み方

このアプリは**アイドル時は何も出力しない**。無反応でも故障とは限らない。

| プレフィックス | 意味 |
|---|---|
| `[ALIVE] loop running (N laps) touchCount=` | 5秒ごと。メインループの生存確認（正常時 約9万周/5秒） |
| `[TOUCH] ...` | タッチ検出とガードの状態 |
| `[T <millis>] Start/Stop realtime recording` / `turnComplete` | 会話の各時刻。応答速度の実測に使う |
| `[AUDIO] chunks= starve= audio= wall= wait=` | 1発話ぶんの再生統計。**`starve` が途切れ回数** |
| `[DIAG] PY32(0x6F) probe: ok=1 ...` | 起動時1回。首が動かないとき PY32 かサーボかの切り分け |
| `[WSc][CLOSE] code=... reason=...` | サーバ都合の切断理由（**下記パッチの適用が必要**） |
| `[WSc] Unknown event` | ハートビートの ping/pong。**異常ではない** |

### ログ取得（PowerShell）

`ReadLine()` は取りこぼす。`ReadBufferSize` を大きくして `ReadExisting()` で読むこと。

```powershell
$p = New-Object System.IO.Ports.SerialPort "COM5",115200
$p.ReadBufferSize = 262144; $p.ReadTimeout = 300; $p.DtrEnable = $false
$p.Open(); $p.RtsEnable = $true; Start-Sleep -Milliseconds 150; $p.RtsEnable = $false  # リセット
$sb = New-Object System.Text.StringBuilder
$end = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $end) { try { [void]$sb.Append($p.ReadExisting()) } catch {}; Start-Sleep -Milliseconds 60 }
$p.Close()
$sb.ToString() -split "`n" | Where-Object { $_ -notmatch 'password|Bearer|sk-|AIza|AQ\.|x-goog|[?]key=|Key:|SSID' }
```

> **⚠️ 生ログには Wi-Fi パスワードと API キーが含まれる。** 上の除外フィルタを必ず通すこと。

### ⚠️ 取得スクリプトを自作しないこと

上のスクリプトを**そのまま使う**こと。自作すると次の理由でほぼ失敗する。

- **`DtrEnable = $false` を明示し、RTS は一瞬だけ立てて戻す。**
  RTS を立てっぱなしにすると **ESP32 がリセット状態で固定され、一切出力しなくなる。**
- **生ログを保存すること。** 「出力がありませんでした」等の要約文をファイルに書いて終わらせない。

**アイドル中でも `[ALIVE]` が5秒ごとに出る。** 無出力になることはまず無い。
「出力がない」と思ったら、本体ではなく**取得側を疑う**こと。
実測値の目安：リセットなし8秒で約400文字、リセット直後12秒で約7,000文字。

---

## SD カードの設定（再ビルド不要で変えられる）

| ファイル | 内容 |
|---|---|
| `/yaml/SC_SecConfig.yaml` | Wi-Fi と API キー。**git 管理外**（`.example` がひな形） |
| `/yaml/SC_BasicConfig.yaml` | `servo_type: "M5_SCS"`（首が動く設定） |
| `/app/AiStackChanEx/SC_ExConfig.yaml` | `llm.type: 3`、使用キャラ、スピーカー音量 |
| `/characters/*.yaml` | 性格設定（`okazemi` / `uranai` を追加済み） |

`servo_type` は起動時に SD から読まれて分岐が決まるため、**値の変更だけなら再ビルド不要**。
首が動かない場合は `"SCS"` にすると PY32 を通らずに起動する（音声対話のみ動作）。

---

## 詳しい記録

`doc/岡ゼミ_技術メモ.md` に、発生した不具合と原因を**判明までの経緯つき**で記録しています。
「症状から原因が推測できなかった」ケースが多いため、同じ問題に当たったときの手がかりとして
残しています。作業前に目を通すことを推奨します。
