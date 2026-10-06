[CmdletBinding()]
param(
    [string] $OutputDirectory
)

# The geometry and colors mirror assets/app.svg. This script regenerates the
# committed ICO and preview with Windows' built-in System.Drawing only.
$ErrorActionPreference = 'Stop'
if (!$OutputDirectory) { $OutputDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) 'assets' }
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$taskIconSizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)

function New-RoundedPath([single] $X, [single] $Y, [single] $Width, [single] $Height, [single] $Radius) {
    $taskPath = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $taskDiameter = [single] (2 * $Radius)
    $taskPath.AddArc($X, $Y, $taskDiameter, $taskDiameter, 180, 90)
    $taskPath.AddArc(($X + $Width - $taskDiameter), $Y, $taskDiameter, $taskDiameter, 270, 90)
    $taskPath.AddArc(($X + $Width - $taskDiameter), ($Y + $Height - $taskDiameter), $taskDiameter, $taskDiameter, 0, 90)
    $taskPath.AddArc($X, ($Y + $Height - $taskDiameter), $taskDiameter, $taskDiameter, 90, 90)
    $taskPath.CloseFigure()
    return $taskPath
}

function New-IconPng([int] $Size) {
    $taskSupersampling = 4
    $taskCanvas = [System.Drawing.Bitmap]::new($Size * $taskSupersampling, $Size * $taskSupersampling,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $taskGraphics = [System.Drawing.Graphics]::FromImage($taskCanvas)
    $taskOutput = $null
    $taskOutputGraphics = $null
    $taskStream = [System.IO.MemoryStream]::new()
    $taskBackgroundPath = New-RoundedPath 8 8 240 240 52
    $taskBackground = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
        [System.Drawing.PointF]::new(0, 8), [System.Drawing.PointF]::new(0, 248),
        [System.Drawing.ColorTranslator]::FromHtml('#337bef'),
        [System.Drawing.ColorTranslator]::FromHtml('#124cac'))
    $taskBorder = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(140, 104, 160, 255), 1.5)
    $taskWhite = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::White)
    $taskArrow = [System.Drawing.SolidBrush]::new([System.Drawing.ColorTranslator]::FromHtml('#acdfff'))
    try {
        $taskGraphics.Clear([System.Drawing.Color]::Transparent)
        $taskGraphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $taskGraphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $taskFactor = [single] ($Size * $taskSupersampling / 256.0)
        $taskGraphics.ScaleTransform($taskFactor, $taskFactor)
        $taskGraphics.FillPath($taskBackground, $taskBackgroundPath)
        $taskGraphics.DrawPath($taskBorder, $taskBackgroundPath)
        foreach ($taskBlock in @(@(134, 66), @(66, 134), @(134, 134))) {
            $taskBlockPath = New-RoundedPath $taskBlock[0] $taskBlock[1] 56 56 5
            try { $taskGraphics.FillPath($taskWhite, $taskBlockPath) }
            finally { $taskBlockPath.Dispose() }
        }
        $taskArrowPoints = [System.Drawing.PointF[]] @(
            [System.Drawing.PointF]::new(78, 79),
            [System.Drawing.PointF]::new(78, 123),
            [System.Drawing.PointF]::new(116, 101)
        )
        $taskGraphics.FillPolygon($taskArrow, $taskArrowPoints)

        $taskOutput = [System.Drawing.Bitmap]::new($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $taskOutputGraphics = [System.Drawing.Graphics]::FromImage($taskOutput)
        $taskOutputGraphics.Clear([System.Drawing.Color]::Transparent)
        $taskOutputGraphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $taskOutputGraphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $taskOutputGraphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $taskOutputGraphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $taskOutputGraphics.DrawImage($taskCanvas, [System.Drawing.Rectangle]::new(0, 0, $Size, $Size),
            0, 0, $taskCanvas.Width, $taskCanvas.Height, [System.Drawing.GraphicsUnit]::Pixel)
        $taskOutput.Save($taskStream, [System.Drawing.Imaging.ImageFormat]::Png)
        return ,$taskStream.ToArray()
    } finally {
        if ($taskOutputGraphics) { $taskOutputGraphics.Dispose() }
        if ($taskOutput) { $taskOutput.Dispose() }
        $taskGraphics.Dispose()
        $taskCanvas.Dispose()
        $taskStream.Dispose()
        $taskBackgroundPath.Dispose()
        $taskBackground.Dispose()
        $taskBorder.Dispose()
        $taskWhite.Dispose()
        $taskArrow.Dispose()
    }
}

$taskPngs = @()
foreach ($taskSize in $taskIconSizes) { $taskPngs += ,(New-IconPng $taskSize) }
$taskIconPath = Join-Path $OutputDirectory 'app.ico'
$taskIconStream = [System.IO.File]::Create($taskIconPath)
$taskWriter = [System.IO.BinaryWriter]::new($taskIconStream)
try {
    $taskWriter.Write([uint16] 0)
    $taskWriter.Write([uint16] 1)
    $taskWriter.Write([uint16] $taskIconSizes.Length)
    $taskOffset = [uint32] (6 + 16 * $taskIconSizes.Length)
    for ($taskIndex = 0; $taskIndex -lt $taskIconSizes.Length; ++$taskIndex) {
        $taskSizeByte = if ($taskIconSizes[$taskIndex] -eq 256) { [byte] 0 } else { [byte] $taskIconSizes[$taskIndex] }
        $taskWriter.Write($taskSizeByte)
        $taskWriter.Write($taskSizeByte)
        $taskWriter.Write([byte] 0)
        $taskWriter.Write([byte] 0)
        $taskWriter.Write([uint16] 1)
        $taskWriter.Write([uint16] 32)
        $taskWriter.Write([uint32] $taskPngs[$taskIndex].Length)
        $taskWriter.Write($taskOffset)
        $taskOffset += $taskPngs[$taskIndex].Length
    }
    foreach ($taskPng in $taskPngs) { $taskWriter.Write([byte[]] $taskPng) }
} finally {
    $taskWriter.Dispose()
    $taskIconStream.Dispose()
}
[System.IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'app-preview.png'), $taskPngs[-1])
Write-Output "Generated $taskIconPath with sizes $($taskIconSizes -join ', ')."
