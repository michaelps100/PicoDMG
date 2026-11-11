# img_to_565.ps1 - convert PNG/JPG artwork to the bootloader's .565 theme format.
#
# Output layout (little-endian):
#   uint16 width, uint16 height, then width*height RGB565 pixels, row-major.
# Default size 266x240 = the GB_MODE=1 window (x=45..310). The image is scaled
# to cover the target and centre-cropped (aspect is kept, nothing is stretched).
#
# Usage:
#   powershell -File tools\img_to_565.ps1 -InDir docs\images\themes -OutDir I:\emu\assets\themes\0
param(
    [Parameter(Mandatory)] [string] $InDir,
    [Parameter(Mandatory)] [string] $OutDir,
    [int] $Width  = 266,
    [int] $Height = 240
)
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null

$files = Get-ChildItem $InDir -File | Where-Object { $_.Extension -match '^\.(png|jpe?g|bmp)$' }
foreach ($f in $files) {
    $src = [System.Drawing.Image]::FromFile($f.FullName)
    try {
        $scale = [Math]::Max($Width / $src.Width, $Height / $src.Height)
        $sw = $Width  / $scale
        $sh = $Height / $scale
        $sx = ($src.Width  - $sw) / 2
        $sy = ($src.Height - $sh) / 2

        $dst = New-Object System.Drawing.Bitmap $Width, $Height, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
        $g = [System.Drawing.Graphics]::FromImage($dst)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $attr = New-Object System.Drawing.Imaging.ImageAttributes
        $attr.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY)   # no dark fringe at the edges
        $g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, $Width, $Height),
                     [float]$sx, [float]$sy, [float]$sw, [float]$sh,
                     [System.Drawing.GraphicsUnit]::Pixel, $attr)
        $g.Dispose()

        $rect = New-Object System.Drawing.Rectangle 0, 0, $Width, $Height
        $bd = $dst.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                            [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
        $stride = $bd.Stride
        $raw = New-Object byte[] ($stride * $Height)
        [System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $raw, 0, $raw.Length)
        $dst.UnlockBits($bd)
        $dst.Dispose()

        $out = New-Object byte[] (4 + 2 * $Width * $Height)
        $out[0] = $Width -band 0xFF;  $out[1] = $Width -shr 8
        $out[2] = $Height -band 0xFF; $out[3] = $Height -shr 8
        $o = 4
        for ($y = 0; $y -lt $Height; $y++) {
            $row = $y * $stride
            for ($x = 0; $x -lt $Width; $x++) {
                $p = $row + 3 * $x                       # 24bpp = B, G, R
                $b = [int]$raw[$p]; $gg = [int]$raw[$p + 1]; $r = [int]$raw[$p + 2]
                $v = (($r -shr 3) -shl 11) -bor (($gg -shr 2) -shl 5) -bor ($b -shr 3)
                $out[$o] = $v -band 0xFF; $out[$o + 1] = $v -shr 8
                $o += 2
            }
        }
        $target = Join-Path $OutDir ($f.BaseName + ".565")
        [System.IO.File]::WriteAllBytes($target, $out)
        "{0} -> {1} ({2} B)" -f $f.Name, $target, $out.Length
    } finally { $src.Dispose() }
}
