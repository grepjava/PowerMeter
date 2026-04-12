Add-Type -AssemblyName System.Drawing

function New-PMBitmap([int]$sz) {
    $bmp = New-Object System.Drawing.Bitmap($sz, $sz, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode   = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $g.Clear([System.Drawing.Color]::Transparent)

    # ── rounded background ─────────────────────────────────────────────────
    $cr  = [int]($sz * 0.18)
    $pad = 1
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($pad,           $pad,           $cr*2, $cr*2, 180, 90)
    $path.AddArc($sz-$pad-$cr*2, $pad,           $cr*2, $cr*2, 270, 90)
    $path.AddArc($sz-$pad-$cr*2, $sz-$pad-$cr*2, $cr*2, $cr*2,   0, 90)
    $path.AddArc($pad,           $sz-$pad-$cr*2, $cr*2, $cr*2,  90, 90)
    $path.CloseFigure()
    $bgBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(248, 13, 13, 19))
    $g.FillPath($bgBrush, $path)

    # ── pin (map-pin / teardrop) ────────────────────────────────────────────
    # Shape: large circle head with two lines meeting at a sharp tip below.
    # Arc runs clockwise from lower-left (125 deg) over the top to lower-right (55 deg).
    [double]$cx    = $sz / 2.0
    [double]$pinR  = $sz * 0.30
    [double]$pinCY = $sz * 0.35
    [double]$tipY  = $sz * 0.87

    [float]$startA = 125.0
    [float]$sweepA = 290.0
    [float]$lx = $cx + $pinR * [Math]::Cos($startA * [Math]::PI / 180.0)
    [float]$ly = $pinCY + $pinR * [Math]::Sin($startA * [Math]::PI / 180.0)
    [float]$ex = $cx + $pinR * [Math]::Cos(($startA + $sweepA) * [Math]::PI / 180.0)
    [float]$ey = $pinCY + $pinR * [Math]::Sin(($startA + $sweepA) * [Math]::PI / 180.0)

    $pinPath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $arcRect = New-Object System.Drawing.RectangleF(
        [float]($cx - $pinR), [float]($pinCY - $pinR),
        [float]($pinR * 2.0), [float]($pinR * 2.0))
    $pinPath.AddArc($arcRect, $startA, $sweepA)
    $pinPath.AddLine($ex, $ey, [float]$cx, [float]$tipY)
    $pinPath.AddLine([float]$cx, [float]$tipY, $lx, $ly)
    $pinPath.CloseFigure()

    # Gradient: bright crimson top -> deep dark-red at tip
    $gradRect = New-Object System.Drawing.RectangleF(
        [float]($cx - $pinR), [float]($pinCY - $pinR),
        [float]($pinR * 2.0), [float]($tipY - ($pinCY - $pinR)))
    if ($gradRect.Height -gt 0) {
        $pinBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
            $gradRect,
            [System.Drawing.Color]::FromArgb(255, 218, 55, 45),
            [System.Drawing.Color]::FromArgb(255, 110, 12, 10),
            [System.Drawing.Drawing2D.LinearGradientMode]::Vertical)
        $g.FillPath($pinBrush, $pinPath)
        $pinBrush.Dispose()
    }

    # Inner circle (shadow / hole on pin head for depth)
    [float]$holeR = $pinR * 0.36
    if ($holeR -ge 1.5) {
        $holeBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(160, 6, 6, 12))
        $g.FillEllipse($holeBrush, [float]($cx - $holeR), [float]($pinCY - $holeR), $holeR * 2.0, $holeR * 2.0)
        $holeBrush.Dispose()
    }

    # Highlight top-left of head
    [float]$hlR = $pinR * 0.20
    if ($hlR -ge 1.0) {
        $hlBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(90, 255, 215, 205))
        $g.FillEllipse($hlBrush, [float]($cx - $hlR - $pinR * 0.14), [float]($pinCY - $pinR * 0.68), $hlR * 2.0, $hlR * 2.0)
        $hlBrush.Dispose()
    }

    $bgBrush.Dispose(); $path.Dispose(); $pinPath.Dispose(); $g.Dispose()
    return $bmp
}

$sizes = @(16, 32, 48, 256)
$pngs  = @()
foreach ($s in $sizes) {
    $bmp = New-PMBitmap $s
    $ms  = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += ,($s, $ms.ToArray())
    $ms.Dispose(); $bmp.Dispose()
}

# Build PNG-in-ICO (Windows Vista+ compatible)
$ico    = New-Object System.IO.MemoryStream
$cnt    = $pngs.Count
$ico.Write([byte[]](0,0,1,0), 0, 4)
$ico.Write([BitConverter]::GetBytes([uint16]$cnt), 0, 2)

$dataOff = 6 + 16 * $cnt
foreach ($e in $pngs) {
    $w = if ($e[0] -eq 256) { [byte]0 } else { [byte]$e[0] }
    $ico.WriteByte($w); $ico.WriteByte($w)
    $ico.WriteByte(0);  $ico.WriteByte(0)
    $ico.Write([BitConverter]::GetBytes([uint16]1),  0, 2)
    $ico.Write([BitConverter]::GetBytes([uint16]32), 0, 2)
    $ico.Write([BitConverter]::GetBytes([uint32]$e[1].Length), 0, 4)
    $ico.Write([BitConverter]::GetBytes([uint32]$dataOff),     0, 4)
    $dataOff += $e[1].Length
}
foreach ($e in $pngs) { $ico.Write($e[1], 0, $e[1].Length) }

$bytes = $ico.ToArray(); $ico.Dispose()
[IO.File]::WriteAllBytes('C:\code\powermeter\PowerMeter\PowerMeter.ico', $bytes)
[IO.File]::WriteAllBytes('C:\code\powermeter\PowerMeter\small.ico', $bytes)
Write-Host "Icon written: $($bytes.Length) bytes, $cnt sizes ($(($sizes) -join ', ')px)"
