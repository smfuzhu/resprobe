# ===========================================================================
#  ResProbe - icon generator
#
#  Produces five variants of the tray icon:
#      res\resprobe.ico        default   (screen: cyan -> deep blue gradient)
#      res\resprobe-green.ico  monitor online          (screen: light -> deep green)
#      res\resprobe-blue.ico   display output off      (screen: dim solid blue)
#      res\resprobe-red.ico    no fallback available   (screen: light -> deep red)
#      res\resprobe-yellow.ico fallback active         (screen: light -> deep amber)
#
#  The three files are committed to the repository so the build does NOT
#  require this script; it only needs to be re-run when the artwork changes:
#
#      make icon
#      (or) powershell -NoProfile -ExecutionPolicy Bypass -File res\make-icon.ps1
#
#  ENCODING: this file is pure ASCII on purpose. Windows PowerShell 5.1 reads
#  a .ps1 without a BOM as ANSI, so non-ASCII content here would be fragile.
#  (Batch scripts have the same rule for a different reason - see DEVELOPMENT.md)
#
#  WHY THE "OFF" ICON IS A DARKER BLUE
#    The default icon already reads as blue (cyan -> deep blue screen), so a
#    second bright blue would be indistinguishable from it at 16x16. The
#    "display output off" variant therefore uses a dim, low-contrast blue that
#    also reads as "the screen is dark". Only the screen colours differ between
#    variants - the monitor shell stays identical so the family stays coherent.
#
#  FORMAT NOTES
#    * sizes <= 64 are written as 32bpp DIB entries (BITMAPINFOHEADER + BGRA
#      rows bottom-up + a zeroed AND mask). This is the most compatible form
#      and is what shell32 expects for small tray/context-menu sizes.
#    * sizes >= 128 are written as PNG entries. PNG-in-ICO is supported since
#      Vista and keeps the file small (a 256x256 DIB alone is 256 KB).
# ===========================================================================

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$sizes   = @(16, 24, 32, 48, 64, 128, 256)
$pngFrom = 128          # >= this size -> PNG entry, otherwise DIB entry

$script:colShell = [System.Drawing.Color]::FromArgb(255, 32, 42, 54)

function New-RoundedPath {
    param([double]$X, [double]$Y, [double]$W, [double]$H, [double]$R)
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    if ($W -le 0 -or $H -le 0) { return $p }
    $d = [Math]::Min($R * 2.0, [Math]::Min($W, $H))
    if ($d -le 0.2) {
        $p.AddRectangle((New-Object System.Drawing.RectangleF($X, $Y, $W, $H)))
        $p.CloseFigure()
        return $p
    }
    $p.AddArc($X,             $Y,             $d, $d, 180.0, 90.0)
    $p.AddArc($X + $W - $d,   $Y,             $d, $d, 270.0, 90.0)
    $p.AddArc($X + $W - $d,   $Y + $H - $d,   $d, $d,   0.0, 90.0)
    $p.AddArc($X,             $Y + $H - $d,   $d, $d,  90.0, 90.0)
    $p.CloseFigure()
    return $p
}

function New-IconBitmap {
    param([int]$S,
          [System.Drawing.Color]$Screen,
          [System.Drawing.Color]$Deep)

    $bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $f = [double]$S

    # ---- monitor shell -----------------------------------------------------
    $bodyX = $f * 0.06
    $bodyY = $f * 0.10
    $bodyW = $f * 0.88
    $bodyH = $f * 0.58
    $rad   = [Math]::Max(1.0, $f * 0.075)

    $shellBrush = New-Object System.Drawing.SolidBrush($script:colShell)
    $shellPath  = New-RoundedPath $bodyX $bodyY $bodyW $bodyH $rad
    $g.FillPath($shellBrush, $shellPath)
    $shellPath.Dispose()

    # ---- screen (vertical gradient) ----------------------------------------
    $inset = [Math]::Max(1.0, $f * 0.055)
    $sx = $bodyX + $inset
    $sy = $bodyY + $inset
    $sw = $bodyW - 2.0 * $inset
    $sh = $bodyH - 2.0 * $inset

    if ($sw -gt 0.5 -and $sh -gt 0.5) {
        $rect = New-Object System.Drawing.RectangleF($sx, $sy, $sw, $sh)
        $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
                    $rect, $Screen, $Deep, 90.0)
        $screenPath = New-RoundedPath $sx $sy $sw $sh ([Math]::Max(0.5, $rad * 0.55))
        $g.FillPath($grad, $screenPath)
        $screenPath.Dispose()
        $grad.Dispose()
    }

    # ---- stand -------------------------------------------------------------
    $neckX = $f * 0.44
    $neckY = $bodyY + $bodyH
    $neckW = $f * 0.12
    $neckH = $f * 0.09
    $g.FillRectangle($shellBrush, [single]$neckX, [single]$neckY, [single]$neckW, [single]$neckH)

    $baseX = $f * 0.25
    $baseY = $neckY + $neckH - $f * 0.01
    $baseW = $f * 0.50
    $baseH = $f * 0.10
    $basePath = New-RoundedPath $baseX $baseY $baseW $baseH ([Math]::Max(0.5, $f * 0.03))
    $g.FillPath($shellBrush, $basePath)
    $basePath.Dispose()

    $shellBrush.Dispose()
    $g.Dispose()
    return $bmp
}

function Get-DibBytes {
    param([System.Drawing.Bitmap]$Bmp)

    $s    = $Bmp.Width
    $rect = New-Object System.Drawing.Rectangle(0, 0, $s, $s)
    $data = $Bmp.LockBits($rect,
                [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $data.Stride
    $raw    = New-Object byte[] ($stride * $s)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $raw, 0, $raw.Length)
    $Bmp.UnlockBits($data)

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)

    # BITMAPINFOHEADER - biHeight is doubled (XOR bitmap + AND mask)
    $bw.Write([UInt32]40)
    $bw.Write([Int32]$s)
    $bw.Write([Int32]($s * 2))
    $bw.Write([UInt16]1)
    $bw.Write([UInt16]32)
    $bw.Write([UInt32]0)                 # BI_RGB
    $bw.Write([UInt32]($s * $s * 4))
    $bw.Write([Int32]0); $bw.Write([Int32]0)
    $bw.Write([UInt32]0); $bw.Write([UInt32]0)

    # XOR bitmap: bottom-up rows (DIB is bottom-up, GDI+ gives us top-down)
    for ($y = $s - 1; $y -ge 0; $y--) {
        $bw.Write($raw, $y * $stride, $s * 4)
    }

    # AND mask: 1bpp, each row padded to a 32-bit boundary, all zero
    # (alpha is carried by the XOR bitmap, so the mask is unused)
    # NOTE: the 3-argument overload is used deliberately. PowerShell resolves
    # Write() to the single-byte overload when handed a byte[] from an
    # expression, silently writing 1 byte instead of the whole array.
    $maskRow   = [int]([Math]::Floor(($s + 31) / 32) * 4)
    $maskBytes = New-Object byte[] ($maskRow * $s)
    $bw.Write($maskBytes, 0, $maskBytes.Length)

    $bw.Flush()
    $bytes = $ms.ToArray()
    $bw.Dispose()
    $ms.Dispose()
    return $bytes
}

function Get-PngBytes {
    param([System.Drawing.Bitmap]$Bmp)
    $ms = New-Object System.IO.MemoryStream
    $Bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $ms.ToArray()
    $ms.Dispose()
    return $bytes
}

function Write-IconFile {
    param([string]$Path,
          [System.Drawing.Color]$Screen,
          [System.Drawing.Color]$Deep)

    $entries = @()
    foreach ($s in $sizes) {
        $bmp = New-IconBitmap $s $Screen $Deep
        if ($s -ge $pngFrom) {
            $bytes = Get-PngBytes $bmp
            $kind  = 'PNG'
        } else {
            $bytes = Get-DibBytes $bmp
            $kind  = 'DIB'
        }
        $bmp.Dispose()
        $entries += [pscustomobject]@{ Size = $s; Data = $bytes; Kind = $kind }
    }

    $fs = [System.IO.File]::Create($Path)
    $bw = New-Object System.IO.BinaryWriter($fs)

    # ICONDIR
    $bw.Write([UInt16]0)
    $bw.Write([UInt16]1)
    $bw.Write([UInt16]$entries.Count)

    # ICONDIRENTRY per image
    $offset = 6 + (16 * $entries.Count)
    foreach ($e in $entries) {
        $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }   # 0 means 256
        $bw.Write([byte]$dim)
        $bw.Write([byte]$dim)
        $bw.Write([byte]0)      # color count (0 = truecolor)
        $bw.Write([byte]0)      # reserved
        $bw.Write([UInt16]1)    # planes
        $bw.Write([UInt16]32)   # bit count
        $bw.Write([UInt32]$e.Data.Length)
        $bw.Write([UInt32]$offset)
        $offset += $e.Data.Length
    }

    # NOTE: 3-argument overload again - see the comment in Get-DibBytes.
    foreach ($e in $entries) { $bw.Write($e.Data, 0, $e.Data.Length) }

    $bw.Flush()
    $bw.Dispose()
    $fs.Dispose()

    $name = Split-Path $Path -Leaf
    Write-Output ("wrote {0} ({1} bytes, {2} sizes)" -f $name, (Get-Item $Path).Length, $entries.Count)

    # sanity check: can the shell's own decoder read it back?
    try {
        $icon = New-Object System.Drawing.Icon($Path)
        Write-Output ("  verified: loads as Icon {0}x{1}" -f $icon.Width, $icon.Height)
        $icon.Dispose()
    } catch {
        Write-Output ("  WARNING: System.Drawing.Icon could not load it: " + $_.Exception.Message)
        exit 1
    }
}

# ---------------------------------------------------------------------------
# the three variants
# ---------------------------------------------------------------------------
$palettes = @(
    [pscustomobject]@{
        File   = 'resprobe.ico'
        Screen = [System.Drawing.Color]::FromArgb(255,  96, 208, 255)   # cyan
        Deep   = [System.Drawing.Color]::FromArgb(255,  21, 101, 192)   # deep blue
    },
    [pscustomobject]@{
        File   = 'resprobe-green.ico'
        Screen = [System.Drawing.Color]::FromArgb(255, 134, 239, 172)   # light green
        Deep   = [System.Drawing.Color]::FromArgb(255,  20, 122,  71)   # deep green
    },
    [pscustomobject]@{
        File   = 'resprobe-blue.ico'
        Screen = [System.Drawing.Color]::FromArgb(255,  56, 110, 180)   # dim blue
        Deep   = [System.Drawing.Color]::FromArgb(255,  14,  30,  62)   # navy
    },
    [pscustomobject]@{
        File   = 'resprobe-red.ico'
        Screen = [System.Drawing.Color]::FromArgb(255, 248, 113, 113)   # light red
        Deep   = [System.Drawing.Color]::FromArgb(255, 180,  35,  24)   # deep red
    },
    [pscustomobject]@{
        File   = 'resprobe-yellow.ico'
        Screen = [System.Drawing.Color]::FromArgb(255, 255, 214, 102)   # light amber
        Deep   = [System.Drawing.Color]::FromArgb(255, 199, 126,  10)   # deep amber
    }
)

foreach ($p in $palettes) {
    Write-IconFile -Path (Join-Path $PSScriptRoot $p.File) -Screen $p.Screen -Deep $p.Deep
}
