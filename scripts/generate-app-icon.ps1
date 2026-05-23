param(
    [string]$SourcePath,
    [string]$TargetPath,
    [string]$TrayTargetPath,
    [string]$PreviewDirectory
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$scriptDirectory = Split-Path -Parent $PSCommandPath
$repoRoot = Split-Path -Parent $scriptDirectory

if (-not $PSBoundParameters.ContainsKey("SourcePath")) {
    $SourcePath = Join-Path $repoRoot "assets\VoxInsertIcon.png"
}

if (-not $PSBoundParameters.ContainsKey("TargetPath")) {
    $TargetPath = Join-Path $repoRoot "assets\VoxInsertIcon.ico"
}

if (-not $PSBoundParameters.ContainsKey("TrayTargetPath")) {
    $TrayTargetPath = Join-Path $repoRoot "assets\VoxInsertTray.ico"
}

Add-Type -AssemblyName System.Drawing

function Test-IsBackgroundPixel {
    param(
        [byte[]]$Buffer,
        [int]$Offset,
        [bool]$Strict
    )

    $blue = [int]$Buffer[$Offset]
    $green = [int]$Buffer[$Offset + 1]
    $red = [int]$Buffer[$Offset + 2]
    $alpha = [int]$Buffer[$Offset + 3]

    $maxChannel = [Math]::Max($red, [Math]::Max($green, $blue))
    $minChannel = [Math]::Min($red, [Math]::Min($green, $blue))
    $delta = $maxChannel - $minChannel

    $maxThreshold = if ($Strict) { 235 } else { 210 }
    $deltaThreshold = if ($Strict) { 18 } else { 28 }

    return ($alpha -gt 240 -and $maxChannel -ge $maxThreshold -and $delta -le $deltaThreshold)
}

function Set-TransparentPixel {
    param(
        [byte[]]$Buffer,
        [int]$Offset
    )

    $Buffer[$Offset] = 0
    $Buffer[$Offset + 1] = 0
    $Buffer[$Offset + 2] = 0
    $Buffer[$Offset + 3] = 0
}

function New-CleanedBitmap {
    param(
        [System.Drawing.Bitmap]$SourceBitmap
    )

    $cleanedBitmap = [System.Drawing.Bitmap]::new(
        $SourceBitmap.Width,
        $SourceBitmap.Height,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)

    $graphics = [System.Drawing.Graphics]::FromImage($cleanedBitmap)
    try {
        $graphics.DrawImage($SourceBitmap, 0, 0, $SourceBitmap.Width, $SourceBitmap.Height)
    }
    finally {
        $graphics.Dispose()
    }

    $rectangle = [System.Drawing.Rectangle]::new(0, 0, $cleanedBitmap.Width, $cleanedBitmap.Height)
    $bitmapData = $cleanedBitmap.LockBits(
        $rectangle,
        [System.Drawing.Imaging.ImageLockMode]::ReadWrite,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)

    try {
        if ($bitmapData.Stride -lt 0) {
            throw "Negative bitmap stride is not supported by this script."
        }

        $byteCount = $bitmapData.Stride * $cleanedBitmap.Height
        $buffer = [byte[]]::new($byteCount)
        [System.Runtime.InteropServices.Marshal]::Copy($bitmapData.Scan0, $buffer, 0, $byteCount)

        $visited = [bool[]]::new($cleanedBitmap.Width * $cleanedBitmap.Height)
        $queue = [System.Collections.Generic.Queue[System.Drawing.Point]]::new()

        $tryEnqueue = {
            param(
                [int]$X,
                [int]$Y
            )

            if ($X -lt 0 -or $Y -lt 0 -or $X -ge $cleanedBitmap.Width -or $Y -ge $cleanedBitmap.Height) {
                return
            }

            $visitedIndex = ($Y * $cleanedBitmap.Width) + $X
            if ($visited[$visitedIndex]) {
                return
            }

            $offset = ($Y * $bitmapData.Stride) + ($X * 4)
            if (-not (Test-IsBackgroundPixel -Buffer $buffer -Offset $offset -Strict $true)) {
                return
            }

            $visited[$visitedIndex] = $true
            $queue.Enqueue([System.Drawing.Point]::new($X, $Y))
        }

        for ($x = 0; $x -lt $cleanedBitmap.Width; ++$x) {
            & $tryEnqueue $x 0
            & $tryEnqueue $x ($cleanedBitmap.Height - 1)
        }

        for ($y = 0; $y -lt $cleanedBitmap.Height; ++$y) {
            & $tryEnqueue 0 $y
            & $tryEnqueue ($cleanedBitmap.Width - 1) $y
        }

        while ($queue.Count -gt 0) {
            $point = $queue.Dequeue()
            $offset = ($point.Y * $bitmapData.Stride) + ($point.X * 4)
            Set-TransparentPixel -Buffer $buffer -Offset $offset

            & $tryEnqueue ($point.X - 1) $point.Y
            & $tryEnqueue ($point.X + 1) $point.Y
            & $tryEnqueue $point.X ($point.Y - 1)
            & $tryEnqueue $point.X ($point.Y + 1)
        }

        for ($y = 1; $y -lt ($cleanedBitmap.Height - 1); ++$y) {
            for ($x = 1; $x -lt ($cleanedBitmap.Width - 1); ++$x) {
                $offset = ($y * $bitmapData.Stride) + ($x * 4)
                if ($buffer[$offset + 3] -eq 0) {
                    continue
                }

                if (-not (Test-IsBackgroundPixel -Buffer $buffer -Offset $offset -Strict $false)) {
                    continue
                }

                $touchesTransparentPixel = $false
                for ($neighborY = $y - 1; $neighborY -le ($y + 1) -and -not $touchesTransparentPixel; ++$neighborY) {
                    for ($neighborX = $x - 1; $neighborX -le ($x + 1); ++$neighborX) {
                        if ($neighborX -eq $x -and $neighborY -eq $y) {
                            continue
                        }

                        $neighborOffset = ($neighborY * $bitmapData.Stride) + ($neighborX * 4)
                        if ($buffer[$neighborOffset + 3] -eq 0) {
                            $touchesTransparentPixel = $true
                            break
                        }
                    }
                }

                if ($touchesTransparentPixel) {
                    Set-TransparentPixel -Buffer $buffer -Offset $offset
                }
            }
        }

        [System.Runtime.InteropServices.Marshal]::Copy($buffer, 0, $bitmapData.Scan0, $byteCount)
    }
    finally {
        $cleanedBitmap.UnlockBits($bitmapData)
    }

    return $cleanedBitmap
}

function Get-SourceRectangle {
    param(
        [System.Drawing.Bitmap]$SourceBitmap,
        [int]$Size
    )

    if ($Size -le 40) {
        return [System.Drawing.Rectangle]::new(
            [int][Math]::Round($SourceBitmap.Width * 0.18),
            [int][Math]::Round($SourceBitmap.Height * 0.14),
            [int][Math]::Round($SourceBitmap.Width * 0.64),
            [int][Math]::Round($SourceBitmap.Height * 0.58))
    }

    return [System.Drawing.Rectangle]::new(0, 0, $SourceBitmap.Width, $SourceBitmap.Height)
}

function New-RenderedFrame {
    param(
        [System.Drawing.Bitmap]$SourceBitmap,
        [int]$Size
    )

    $frameBitmap = [System.Drawing.Bitmap]::new(
        $Size,
        $Size,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)

    $graphics = [System.Drawing.Graphics]::FromImage($frameBitmap)
    try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality

        $sourceRectangle = Get-SourceRectangle -SourceBitmap $SourceBitmap -Size $Size
        $destinationRectangle = [System.Drawing.Rectangle]::new(0, 0, $Size, $Size)

        $graphics.DrawImage(
            $SourceBitmap,
            $destinationRectangle,
            $sourceRectangle,
            [System.Drawing.GraphicsUnit]::Pixel)
    }
    finally {
        $graphics.Dispose()
    }

    return $frameBitmap
}

function New-RoundedRectanglePath {
    param(
        [float]$X,
        [float]$Y,
        [float]$Width,
        [float]$Height,
        [float]$Radius
    )

    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $diameter = [Math]::Min($Radius * 2.0, [Math]::Min($Width, $Height))

    if ($diameter -le 0.0) {
        $path.AddRectangle([System.Drawing.RectangleF]::new($X, $Y, $Width, $Height))
        return $path
    }

    $path.AddArc($X, $Y, $diameter, $diameter, 180, 90)
    $path.AddArc($X + $Width - $diameter, $Y, $diameter, $diameter, 270, 90)
    $path.AddArc($X + $Width - $diameter, $Y + $Height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($X, $Y + $Height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-TrafficLightColor {
    param(
        [int]$Red,
        [int]$Green,
        [int]$Blue
    )

    return [System.Drawing.Color]::FromArgb(255, $Red, $Green, $Blue)
}

function New-RenderedTrayFrame {
    param(
        [int]$Size
    )

    $frameBitmap = [System.Drawing.Bitmap]::new(
        $Size,
        $Size,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)

    $graphics = [System.Drawing.Graphics]::FromImage($frameBitmap)
    try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias

        $micColor = New-TrafficLightColor -Red 220 -Green 229 -Blue 240
        $waveColor = New-TrafficLightColor -Red 61 -Green 223 -Blue 247
        $strokeWidth = [float][Math]::Max(1.35, $Size * 0.085)

        $capsuleX = [float]($Size * 0.39)
        $capsuleY = [float]($Size * 0.15)
        $capsuleWidth = [float]($Size * 0.22)
        $capsuleHeight = [float]($Size * 0.35)
        $capsuleRadius = [float][Math]::Max(1.8, $capsuleWidth * 0.48)

        $supportArcX = [float]($Size * 0.31)
        $supportArcY = [float]($Size * 0.41)
        $supportArcWidth = [float]($Size * 0.38)
        $supportArcHeight = [float]($Size * 0.26)
        $centerX = [float]($Size * 0.50)
        $standTopY = [float]($Size * 0.59)
        $standBottomY = [float]($Size * 0.72)
        $baseY = [float]($Size * 0.78)
        $baseLeftX = [float]($Size * 0.30)
        $baseRightX = [float]($Size * 0.70)

        $outerWaveXLeft = [float]($Size * 0.10)
        $outerWaveY = [float]($Size * 0.31)
        $outerWaveWidth = [float][Math]::Max(1.7, $Size * 0.07)
        $outerWaveHeight = [float]($Size * 0.27)
        $innerWaveXLeft = [float]($Size * 0.18)
        $innerWaveY = [float]($Size * 0.36)
        $innerWaveWidth = [float][Math]::Max(1.6, $Size * 0.065)
        $innerWaveHeight = [float]($Size * 0.18)
        $rightOuterWaveX = [float]($Size - $outerWaveXLeft - $outerWaveWidth)
        $rightInnerWaveX = [float]($Size - $innerWaveXLeft - $innerWaveWidth)

        $micPen = [System.Drawing.Pen]::new($micColor, $strokeWidth)
        $micPen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
        $micPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
        $micPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round

        $waveBrush = [System.Drawing.SolidBrush]::new($waveColor)
        try {
            $capsulePath = New-RoundedRectanglePath -X $capsuleX -Y $capsuleY -Width $capsuleWidth -Height $capsuleHeight -Radius $capsuleRadius
            try {
                $graphics.DrawPath($micPen, $capsulePath)
            }
            finally {
                $capsulePath.Dispose()
            }

            $graphics.DrawArc($micPen, $supportArcX, $supportArcY, $supportArcWidth, $supportArcHeight, 20.0, 140.0)
            $graphics.DrawLine($micPen, $centerX, $standTopY, $centerX, $standBottomY)
            $graphics.DrawLine($micPen, $baseLeftX, $baseY, $baseRightX, $baseY)

            foreach ($rect in @(
                [System.Drawing.RectangleF]::new($outerWaveXLeft, $outerWaveY, $outerWaveWidth, $outerWaveHeight),
                [System.Drawing.RectangleF]::new($innerWaveXLeft, $innerWaveY, $innerWaveWidth, $innerWaveHeight),
                [System.Drawing.RectangleF]::new($rightInnerWaveX, $innerWaveY, $innerWaveWidth, $innerWaveHeight),
                [System.Drawing.RectangleF]::new($rightOuterWaveX, $outerWaveY, $outerWaveWidth, $outerWaveHeight)
            )) {
                $wavePath = New-RoundedRectanglePath -X $rect.X -Y $rect.Y -Width $rect.Width -Height $rect.Height -Radius ([float]($rect.Width * 1.3))
                try {
                    $graphics.FillPath($waveBrush, $wavePath)
                }
                finally {
                    $wavePath.Dispose()
                }
            }
        }
        finally {
            $micPen.Dispose()
            $waveBrush.Dispose()
        }
    }
    finally {
        $graphics.Dispose()
    }

    return $frameBitmap
}

function Write-IcoFile {
    param(
        [System.Collections.Generic.List[System.Drawing.Bitmap]]$Frames,
        [string]$OutputPath
    )

    $framePayloads = @()
    foreach ($frame in $Frames) {
        $stream = [System.IO.MemoryStream]::new()
        try {
            $frame.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
            $framePayloads += [PSCustomObject]@{
                Width = $frame.Width
                Height = $frame.Height
                Bytes = $stream.ToArray()
            }
        }
        finally {
            $stream.Dispose()
        }
    }

    $outputDirectory = Split-Path -Path $OutputPath -Parent
    if ($outputDirectory) {
        [System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
    }

    $fileStream = [System.IO.File]::Open($OutputPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try {
        $writer = [System.IO.BinaryWriter]::new($fileStream)
        try {
            $writer.Write([UInt16]0)
            $writer.Write([UInt16]1)
            $writer.Write([UInt16]$framePayloads.Count)

            $offset = 6 + (16 * $framePayloads.Count)
            foreach ($framePayload in $framePayloads) {
                $widthByte = if ($framePayload.Width -ge 256) { [byte]0 } else { [byte]$framePayload.Width }
                $heightByte = if ($framePayload.Height -ge 256) { [byte]0 } else { [byte]$framePayload.Height }

                $writer.Write($widthByte)
                $writer.Write($heightByte)
                $writer.Write([byte]0)
                $writer.Write([byte]0)
                $writer.Write([UInt16]1)
                $writer.Write([UInt16]32)
                $writer.Write([UInt32]$framePayload.Bytes.Length)
                $writer.Write([UInt32]$offset)

                $offset += $framePayload.Bytes.Length
            }

            foreach ($framePayload in $framePayloads) {
                $writer.Write($framePayload.Bytes)
            }
        }
        finally {
            $writer.Dispose()
        }
    }
    finally {
        $fileStream.Dispose()
    }
}

function New-UpscaledPreview {
    param(
        [System.Drawing.Bitmap]$SourceBitmap,
        [int]$ScaleFactor
    )

    $previewBitmap = [System.Drawing.Bitmap]::new(
        $SourceBitmap.Width * $ScaleFactor,
        $SourceBitmap.Height * $ScaleFactor,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)

    $graphics = [System.Drawing.Graphics]::FromImage($previewBitmap)
    try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half

        $graphics.DrawImage(
            $SourceBitmap,
            [System.Drawing.Rectangle]::new(0, 0, $previewBitmap.Width, $previewBitmap.Height),
            [System.Drawing.Rectangle]::new(0, 0, $SourceBitmap.Width, $SourceBitmap.Height),
            [System.Drawing.GraphicsUnit]::Pixel)
    }
    finally {
        $graphics.Dispose()
    }

    return $previewBitmap
}

function Get-NormalizedPath {
    param(
        [string]$PathValue
    )

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return [System.IO.Path]::GetFullPath($PathValue)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $PWD $PathValue))
}

$sourceFullPath = Get-NormalizedPath -PathValue $SourcePath
$targetFullPath = Get-NormalizedPath -PathValue $TargetPath
$trayTargetFullPath = Get-NormalizedPath -PathValue $TrayTargetPath

$appFrameSizes = @(16, 20, 24, 32, 40, 48, 64, 256)
$trayFrameSizes = @(16, 20, 24, 32, 40, 48, 64)
$appFrames = [System.Collections.Generic.List[System.Drawing.Bitmap]]::new()
$trayFrames = [System.Collections.Generic.List[System.Drawing.Bitmap]]::new()
$cleanedBitmap = $null

$sourceBitmap = [System.Drawing.Bitmap]::FromFile($sourceFullPath)
try {
    $cleanedBitmap = New-CleanedBitmap -SourceBitmap $sourceBitmap

    foreach ($frameSize in $appFrameSizes) {
        $appFrames.Add((New-RenderedFrame -SourceBitmap $cleanedBitmap -Size $frameSize))
    }

    foreach ($frameSize in $trayFrameSizes) {
        $trayFrames.Add((New-RenderedTrayFrame -Size $frameSize))
    }

    Write-IcoFile -Frames $appFrames -OutputPath $targetFullPath
    Write-IcoFile -Frames $trayFrames -OutputPath $trayTargetFullPath

    if ($PreviewDirectory) {
        $previewFullPath = Get-NormalizedPath -PathValue $PreviewDirectory
        [System.IO.Directory]::CreateDirectory($previewFullPath) | Out-Null

        foreach ($previewSize in @(16, 20, 24, 32)) {
            foreach ($previewSet in @(
                [PSCustomObject]@{ Prefix = 'app'; Frames = $appFrames },
                [PSCustomObject]@{ Prefix = 'tray'; Frames = $trayFrames }
            )) {
                $frame = $previewSet.Frames | Where-Object { $_.Width -eq $previewSize } | Select-Object -First 1
                if ($null -eq $frame) {
                    continue
                }

                $preview = New-UpscaledPreview -SourceBitmap $frame -ScaleFactor 10
                try {
                    $previewPath = Join-Path $previewFullPath ("{0}-frame-{1}@10x.png" -f $previewSet.Prefix, $previewSize)
                    $preview.Save($previewPath, [System.Drawing.Imaging.ImageFormat]::Png)
                }
                finally {
                    $preview.Dispose()
                }
            }
        }
    }

    "source=$sourceFullPath"
    "app_target=$targetFullPath"
    "tray_target=$trayTargetFullPath"
    "app_frames=$($appFrameSizes -join ',')"
    "tray_frames=$($trayFrameSizes -join ',')"
}
finally {
    foreach ($frame in $appFrames) {
        $frame.Dispose()
    }

    foreach ($frame in $trayFrames) {
        $frame.Dispose()
    }

    if ($null -ne $cleanedBitmap) {
        $cleanedBitmap.Dispose()
    }

    $sourceBitmap.Dispose()
}