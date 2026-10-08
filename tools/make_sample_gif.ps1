# README 用のサンプルアニメーション GIF (docs\sample.gif) を生成する
# WPF の GifBitmapEncoder を使い、回転するカラーホイールと跳ねるボールを描く。
param([string]$Out = (Join-Path $PSScriptRoot "..\docs\sample.gif"), [int]$Frames = 30, [byte]$DelayCs = 5)
Add-Type -AssemblyName PresentationCore, WindowsBase
$ErrorActionPreference = "Stop"
$W = 480; $H = 300

$enc = New-Object Windows.Media.Imaging.GifBitmapEncoder
for ($i = 0; $i -lt $Frames; $i++) {
    $t = $i / $Frames
    $dv = New-Object Windows.Media.DrawingVisual
    $dc = $dv.RenderOpen()
    $bg = New-Object Windows.Media.SolidColorBrush ([Windows.Media.Color]::FromRgb(28, 32, 72))   # 単色にしてディザを避け、ファイルを小さくする
    $dc.DrawRectangle($bg, $null, (New-Object Windows.Rect 0, 0, $W, $H))

    # 回転するカラーホイール (12 分割)
    $cx = 140; $cy = 150; $r = 95
    for ($k = 0; $k -lt 12; $k++) {
        $a0 = ($k / 12 + $t) * 2 * [Math]::PI; $a1 = (($k + 1) / 12 + $t) * 2 * [Math]::PI
        $hue = $k / 12
        $h6 = $hue * 6; $f = $h6 - [Math]::Floor($h6); $q = 1 - $f
        $rgb = switch ([int][Math]::Floor($h6) % 6) { 0 { 1, $f, 0 } 1 { $q, 1, 0 } 2 { 0, 1, $f } 3 { 0, $q, 1 } 4 { $f, 0, 1 } 5 { 1, 0, $q } }
        $col = [Windows.Media.Color]::FromRgb([byte](255 * $rgb[0]), [byte](255 * $rgb[1]), [byte](255 * $rgb[2]))
        $geo = New-Object Windows.Media.StreamGeometry
        $ctx = $geo.Open()
        $ctx.BeginFigure((New-Object Windows.Point $cx, $cy), $true, $true)
        $ctx.LineTo((New-Object Windows.Point ($cx + $r * [Math]::Cos($a0)), ($cy + $r * [Math]::Sin($a0))), $true, $false)
        $ctx.ArcTo((New-Object Windows.Point ($cx + $r * [Math]::Cos($a1)), ($cy + $r * [Math]::Sin($a1))), (New-Object Windows.Size $r, $r), 0, $false, [Windows.Media.SweepDirection]::Clockwise, $true, $false)
        $ctx.Close()
        $dc.DrawGeometry((New-Object Windows.Media.SolidColorBrush $col), $null, $geo)
    }
    $dc.DrawEllipse([Windows.Media.Brushes]::White, $null, (New-Object Windows.Point $cx, $cy), 28, 28)

    # 跳ねるボール
    for ($b = 0; $b -lt 3; $b++) {
        $ph = ($t * 2 + $b / 3) % 1
        $y = 250 - 170 * 4 * $ph * (1 - $ph)
        $x = 300 + $b * 60
        $c = @([Windows.Media.Colors]::Orange, [Windows.Media.Colors]::DeepPink, [Windows.Media.Colors]::LimeGreen)[$b]
        $dc.DrawEllipse((New-Object Windows.Media.SolidColorBrush $c), $null, (New-Object Windows.Point $x, $y), 20, 20)
    }
    $dc.DrawRectangle((New-Object Windows.Media.SolidColorBrush ([Windows.Media.Color]::FromArgb(90, 255, 255, 255))), $null, (New-Object Windows.Rect 270, 272, 190, 3))

    $tf = New-Object Windows.Media.Typeface "Segoe UI Semibold"
    $ft = New-Object Windows.Media.FormattedText "FastGif", ([Globalization.CultureInfo]::InvariantCulture), ([Windows.FlowDirection]::LeftToRight), $tf, 28.0, ([Windows.Media.Brushes]::White), 1.0
    $dc.DrawText($ft, (New-Object Windows.Point 300, 18))
    $dc.Close()

    $bmp = New-Object Windows.Media.Imaging.RenderTargetBitmap $W, $H, 96, 96, ([Windows.Media.PixelFormats]::Pbgra32)
    $bmp.Render($dv)
    $enc.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bmp))
}
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$ms = New-Object IO.MemoryStream
$enc.Save($ms)
$b = $ms.ToArray()

# WPF は遅延とループ指定を書き出さないので、バイナリを直接修正する
$p = 13
if ($b[10] -band 0x80) { $p += 3 * (2 -shl ($b[10] -band 7)) }
$head = $p
function Skip-Sub([int]$q) { while ($b[$q] -ne 0) { $q += $b[$q] + 1 }; return $q + 1 }
$hasLoop = $false
while ($p -lt $b.Length -and $b[$p] -ne 0x3B) {
    if ($b[$p] -eq 0x21) {
        if ($b[$p + 1] -eq 0xF9) { $b[$p + 4] = $DelayCs; $b[$p + 5] = 0 }     # GCE の遅延 (1/100 秒)
        if ($b[$p + 1] -eq 0xFF) { $hasLoop = $true }
        $p = Skip-Sub ($p + 2)
    } elseif ($b[$p] -eq 0x2C) {
        $fl = $b[$p + 9]; $p += 10
        if ($fl -band 0x80) { $p += 3 * (2 -shl ($fl -band 7)) }
        $p = Skip-Sub ($p + 1)
    } else { throw "unexpected block 0x{0:X2} at {1}" -f $b[$p], $p }
}
$buf = New-Object IO.MemoryStream
$buf.Write($b, 0, $head)
if (-not $hasLoop) {   # NETSCAPE2.0 拡張: 無限ループ
    $loop = [byte[]](0x21, 0xFF, 0x0B) + [Text.Encoding]::ASCII.GetBytes("NETSCAPE2.0") + [byte[]](3, 1, 0, 0, 0)
    $buf.Write($loop, 0, $loop.Length)
}
$buf.Write($b, $head, $b.Length - $head)
[IO.File]::WriteAllBytes($Out, $buf.ToArray())
"wrote $Out ($((Get-Item $Out).Length) bytes)"
