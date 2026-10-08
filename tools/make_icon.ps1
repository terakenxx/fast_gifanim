# FastGif のアイコン (res\fastgif.ico) を生成する
# 角丸の青紫グラデーション + 白い "GIF" 文字 + 再生マーク。各サイズを PNG で ICO に格納する。
param([string]$Out = (Join-Path $PSScriptRoot "..\res\fastgif.ico"), [string]$PreviewDir = "")
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = "Stop"

function New-RoundRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-IconPng([int]$s) {
    $bmp = New-Object Drawing.Bitmap $s, $s, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = "AntiAlias"
    $g.TextRenderingHint = "AntiAliasGridFit"
    $g.Clear([Drawing.Color]::Transparent)

    $m = [single]([Math]::Max(1, [Math]::Round($s / 32)))      # 外周の余白
    $bw = [single]($s - 2 * $m)
    $bg = New-RoundRect $m $m $bw $bw ([single]($s * 0.2))
    $rect = New-Object Drawing.RectangleF $m, $m, $bw, $bw
    $grad = New-Object Drawing.Drawing2D.LinearGradientBrush $rect, ([Drawing.Color]::FromArgb(255, 124, 77, 255)), ([Drawing.Color]::FromArgb(255, 0, 176, 255)), ([single]45)
    $g.FillPath($grad, $bg)

    $white = New-Object Drawing.SolidBrush ([Drawing.Color]::White)
    if ($s -ge 32) {
        # "GIF" の文字 (上寄せ) と、右下の再生マーク
        $font = New-Object Drawing.Font "Segoe UI Black", ([single]($s * 0.34)), ([Drawing.FontStyle]::Bold), ([Drawing.GraphicsUnit]::Pixel)
        $fmt = New-Object Drawing.StringFormat
        $fmt.Alignment = "Center"
        $fmt.LineAlignment = "Center"
        $tr = New-Object Drawing.RectangleF 0, ([single]($s * 0.08)), ([single]$s), ([single]($s * 0.5))
        $g.DrawString("GIF", $font, $white, $tr, $fmt)
        $cx = $s * 0.5; $cy = $s * 0.68; $r = $s * 0.17
        $pts = [Drawing.PointF[]]@(
            (New-Object Drawing.PointF ([single]($cx - $r * 0.7)), ([single]($cy - $r))),
            (New-Object Drawing.PointF ([single]($cx - $r * 0.7)), ([single]($cy + $r))),
            (New-Object Drawing.PointF ([single]($cx + $r * 1.1)), ([single]$cy)))
        $g.FillPolygon($white, $pts)
        $font.Dispose()
    } else {
        # 小さいサイズでは文字が潰れるので再生マークだけ
        $cx = $s * 0.52; $cy = $s * 0.5; $r = $s * 0.3
        $pts = [Drawing.PointF[]]@(
            (New-Object Drawing.PointF ([single]($cx - $r * 0.8)), ([single]($cy - $r))),
            (New-Object Drawing.PointF ([single]($cx - $r * 0.8)), ([single]($cy + $r))),
            (New-Object Drawing.PointF ([single]($cx + $r)), ([single]$cy)))
        $g.FillPolygon($white, $pts)
    }
    $g.Dispose()
    $ms = New-Object IO.MemoryStream
    $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return , $ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$pngs = foreach ($s in $sizes) { , (New-IconPng $s) }
if ($PreviewDir) { for ($i = 0; $i -lt $sizes.Count; $i++) { [IO.File]::WriteAllBytes((Join-Path $PreviewDir "icon_$($sizes[$i]).png"), $pngs[$i]) } }

# ICO ファイル: ICONDIR + ICONDIRENTRY x n + PNG データ
$ms = New-Object IO.MemoryStream
$bw = New-Object IO.BinaryWriter $ms
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $pngs[$i].Length
    $bw.Write([byte]($s % 256)); $bw.Write([byte]($s % 256))      # 256 は 0 で表す
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$len); $bw.Write([uint32]$offset)
    $offset += $len
}
foreach ($p in $pngs) { $bw.Write([byte[]]$p) }
$bw.Flush()
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[IO.File]::WriteAllBytes($Out, $ms.ToArray())
"wrote $Out ($($ms.Length) bytes)"
