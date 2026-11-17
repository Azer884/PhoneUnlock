# Generates windows/src/app.ico (phone + lock glyph) with PNG-compressed frames.
param([string]$Out = (Join-Path $PSScriptRoot '..\src\app.ico'))
Add-Type -AssemblyName System.Drawing

function New-Frame([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)

    # Rounded blue tile
    $r = [single]($s * 0.22)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, $r, $r, 180, 90); $path.AddArc($s - $r - 1, 0, $r, $r, 270, 90)
    $path.AddArc($s - $r - 1, $s - $r - 1, $r, $r, 0, 90); $path.AddArc(0, $s - $r - 1, $r, $r, 90, 90)
    $path.CloseFigure()
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0, 99, 177))), $path)

    # Phone outline
    $w = [single]([Math]::Max(1.5, $s * 0.07))
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), $w
    $pw = $s * 0.42; $ph = $s * 0.70; $px = ($s - $pw) / 2; $py = ($s - $ph) / 2
    $g.DrawRectangle($pen, [single]$px, [single]$py, [single]$pw, [single]$ph)

    # Padlock inside the phone
    $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
    $lw = $pw * 0.52; $lh = $ph * 0.24; $lx = ($s - $lw) / 2; $ly = $py + $ph * 0.50
    $g.FillRectangle($white, [single]$lx, [single]$ly, [single]$lw, [single]$lh)
    $sw = $lw * 0.64; $sh = $lh * 1.1
    $pen2 = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([single]([Math]::Max(1, $s * 0.045)))
    $g.DrawArc($pen2, [single](($s - $sw) / 2), [single]($ly - $sh * 0.75), [single]$sw, [single]$sh, 180, 180)
    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return , $ms.ToArray()
}

$sizes = 16, 24, 32, 48, 64, 256
$frames = $sizes | ForEach-Object { , (New-Frame $_) }

$fs = [System.IO.File]::Create($Out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $frames[$i].Length
    $dim = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$len); $bw.Write([uint32]$offset)
    $offset += $len
}
foreach ($f in $frames) { $bw.Write($f) }
$bw.Close()
Write-Host "Wrote $Out"
