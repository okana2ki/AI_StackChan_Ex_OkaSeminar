# ============================================================
#  WebSocket の切断理由を Serial に出すようにする修正パッチ
# ============================================================
#
#  【なぜ必要か】
#  WebSocket ライブラリは、サーバから切断された理由（クローズコード）を
#  受け取っているのに、既定では Serial に出力しません。
#  この理由が見えないと「モデル名が違う」「権限がない」といった
#  致命的な原因を特定できません。実際、これを出すようにして初めて
#  原因が判明したことが複数回あります。
#
#  【なぜスクリプトなのか】
#  修正箇所は .pio/libdeps という「自動ダウンロードされる部品置き場」に
#  あるため、ビルドし直すと元に戻ります。
#  消えたら再実行できるよう、スクリプトにしてあります。
#
#  【使い方】
#  1回目のビルドが終わったあとに、PowerShell で実行してください。
#      powershell -ExecutionPolicy Bypass -File my_script\patch_websockets_close_log.ps1
#  そのあと、もう一度ビルドすると反映されます。
#
#  何度実行しても安全です（既に修正済みなら何もしません）。
# ============================================================

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$sep  = [char]92
$libdeps = Join-Path $root 'firmware\.pio\libdeps'

if (-not (Test-Path $libdeps)) {
    Write-Host "部品置き場がまだありません。先に1回ビルドしてください。" -ForegroundColor Yellow
    exit 1
}

$targets = Get-ChildItem -Path $libdeps -Filter 'WebSockets.cpp' -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -like ('*' + $sep + 'WebSockets' + $sep + 'src' + $sep + '*') }

if (-not $targets) {
    Write-Host "対象が見つかりません。先に1回ビルドしてください。" -ForegroundColor Yellow
    exit 1
}

# 元のコードでは reasonCode の宣言が #ifndef NODEBUG_WEBSOCKETS で囲まれている。
# デバッグ出力は無効なので、囲みを外さないと reasonCode が存在せずコンパイルできない。
$guarded = @"
#ifndef NODEBUG_WEBSOCKETS
                uint16_t reasonCode = 1000;
                if(header->payloadLen >= 2) {
                    reasonCode = payload[0] << 8 | payload[1];
                }
#endif
"@
$unguarded = @"
                uint16_t reasonCode = 1000;
                if(header->payloadLen >= 2) {
                    reasonCode = payload[0] << 8 | payload[1];
                }
"@

$anchor = '                clientDisconnect(client, 1000);'
$inject  = '                // [DIAG] サーバからのクローズコード・理由を Serial に出力（切断原因の特定用）' + "`r`n"
$inject += '                if(header->payloadLen > 2) {' + "`r`n"
$inject += '                    Serial.printf("[WSc][CLOSE] code=%d reason=%.*s\n", reasonCode, (int)(header->payloadLen - 2), (char*)(payload + 2));' + "`r`n"
$inject += '                } else {' + "`r`n"
$inject += '                    Serial.printf("[WSc][CLOSE] code=%d (no reason)\n", reasonCode);' + "`r`n"
$inject += '                }' + "`r`n"
$inject += $anchor

foreach ($f in $targets) {
    $text = Get-Content -Raw -Encoding UTF8 $f.FullName

    if ($text -match '\[WSc\]\[CLOSE\]') {
        Write-Host "修正済み: $($f.FullName)" -ForegroundColor DarkGray
        continue
    }

    $idx = $text.IndexOf('case WSop_close:')
    if ($idx -lt 0) {
        Write-Host "対象箇所なし（版が違う可能性）: $($f.FullName)" -ForegroundColor Yellow
        continue
    }

    $head = $text.Substring(0, $idx)
    $tail = $text.Substring($idx)

    # ① reasonCode 宣言の #ifndef 囲みを外す
    $g = $guarded -replace "`r`n", "`n"
    $t = $tail    -replace "`r`n", "`n"
    $u = $unguarded -replace "`r`n", "`n"
    if ($t.Contains($g)) {
        $pos = $t.IndexOf($g)
        $t = $t.Remove($pos, $g.Length).Insert($pos, $u)
    }

    # ② 切断理由の出力を差し込む
    $a = $anchor
    $pos2 = $t.IndexOf($a)
    if ($pos2 -lt 0) {
        Write-Host "差し込み位置が見つかりません: $($f.FullName)" -ForegroundColor Yellow
        continue
    }
    $t = $t.Remove($pos2, $a.Length).Insert($pos2, ($inject -replace "`r`n", "`n"))

    Set-Content -Path $f.FullName -Value ($head + $t) -Encoding UTF8 -NoNewline
    Write-Host "修正しました: $($f.FullName)" -ForegroundColor Green
}

Write-Host ""
Write-Host "完了です。もう一度ビルドすると反映されます。" -ForegroundColor Cyan
