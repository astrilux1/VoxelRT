param(
  [string]$Out = "results\images\bunker_review_sheet.png",
  [int]$Scale = 4,
  [object[]]$Images
)

Add-Type -AssemblyName System.Drawing

if(-not $Images -or $Images.Count -eq 0){
  $Images = @(
    @{ label="REF"; path="results\images\ref_bunker_post.ppm" },
    @{ label="FCGIX b1.00"; path="results\images\bunker_FCGIX_b1.0x_f149.ppm" },
    @{ label="PTAA b0.60"; path="results\images\bunker_PTAA_b0.6x_f149.ppm" },
    @{ label="PTAI b0.45"; path="results\images\bunker_PTAI_b0.45x_f149.ppm" },
    @{ label="PTBG b0.35"; path="results\images\bunker_PTBG_b0.35x_f149.ppm" },
    @{ label="PTBH b0.35"; path="results\images\bunker_PTBH_b0.35x_f149.ppm" },
    @{ label="PTBI b0.35"; path="results\images\bunker_PTBI_b0.35x_f149.ppm" },
    @{ label="PTBJ b0.35"; path="results\images\bunker_PTBJ_b0.35x_f149.ppm" },
    @{ label="PTBK b0.35"; path="results\images\bunker_PTBK_b0.35x_f149.ppm" },
    @{ label="PTBN b0.35"; path="results\images\bunker_PTBN_b0.35x_f149.ppm" },
    @{ label="PTBO b0.35"; path="results\images\bunker_PTBO_b0.35x_f149.ppm" },
    @{ label="PTBQ b0.35"; path="results\images\bunker_PTBQ_b0.35x_f149.ppm" },
    @{ label="PTBR b0.35"; path="results\images\bunker_PTBR_b0.35x_f149.ppm" },
    @{ label="PTBR b0.40"; path="results\images\bunker_PTBR_b0.40x_f149.ppm" },
    @{ label="PTCI b0.35"; path="results\images\bunker_PTCI_b0.35x_f149.ppm" },
    @{ label="PTCJ b0.35"; path="results\images\bunker_PTCJ_b0.35x_f149.ppm" },
    @{ label="PTCK b0.35"; path="results\images\bunker_PTCK_b0.35x_f149.ppm" },
    @{ label="PTCL b0.35"; path="results\images\bunker_PTCL_b0.35x_f149.ppm" },
    @{ label="PTCM b0.35"; path="results\images\bunker_PTCM_b0.35x_f149.ppm" },
    @{ label="PTCN b0.35"; path="results\images\bunker_PTCN_b0.35x_f149.ppm" },
    @{ label="PTCO b0.35"; path="results\images\bunker_PTCO_b0.35x_f149.ppm" },
    @{ label="PTCP b0.35"; path="results\images\bunker_PTCP_b0.35x_f149.ppm" },
    @{ label="PTCQ b0.35"; path="results\images\bunker_PTCQ_b0.35x_f149.ppm" },
    @{ label="PTCR b0.35"; path="results\images\bunker_PTCR_post_b0.35x_f149.ppm" },
    @{ label="PTCS b0.35"; path="results\images\bunker_PTCS_post_b0.35x_f149.ppm" },
    @{ label="PTCT b0.35"; path="results\images\bunker_PTCT_post_b0.35x_f149.ppm" },
    @{ label="PTCU b0.35"; path="results\images\bunker_PTCU_post_b0.35x_f149.ppm" },
    @{ label="PTCV b0.35"; path="results\images\bunker_PTCV_post_b0.35x_f149.ppm" },
    @{ label="PTCW b0.35"; path="results\images\bunker_PTCW_post_b0.35x_f149.ppm" },
    @{ label="PTCX b0.35"; path="results\images\bunker_PTCX_post_b0.35x_f149.ppm" },
    @{ label="PTCY b0.35"; path="results\images\bunker_PTCY_post_b0.35x_f149.ppm" },
    @{ label="PTCZ b0.35"; path="results\images\bunker_PTCZ_post_b0.35x_f149.ppm" },
    @{ label="PTDA b0.35"; path="results\images\bunker_PTDA_post_b0.35x_f149.ppm" },
    @{ label="PTDB b0.35"; path="results\images\bunker_PTDB_post_b0.35x_f149.ppm" },
    @{ label="PTDC b0.35"; path="results\images\bunker_PTDC_post_b0.35x_f149.ppm" },
    @{ label="PTDD b0.35"; path="results\images\bunker_PTDD_post_b0.35x_f149.ppm" },
    @{ label="PTDE b0.35"; path="results\images\bunker_PTDE_post_b0.35x_f149.ppm" },
    @{ label="PTDF b0.35"; path="results\images\bunker_PTDF_post_b0.35x_f149.ppm" },
    @{ label="PTDG b0.35"; path="results\images\bunker_PTDG_post_b0.35x_f149.ppm" },
    @{ label="PTDH b0.35"; path="results\images\bunker_PTDH_post_b0.35x_f149.ppm" },
    @{ label="PTDI b0.35"; path="results\images\bunker_PTDI_post_b0.35x_f149.ppm" },
    @{ label="PTDJ b0.35"; path="results\images\bunker_PTDJ_post_b0.35x_f149.ppm" },
    @{ label="PTDK b0.35"; path="results\images\bunker_PTDK_post_b0.35x_f149.ppm" },
    @{ label="PTDL b0.35"; path="results\images\bunker_PTDL_post_b0.35x_f149.ppm" },
    @{ label="PTDM b0.35"; path="results\images\bunker_PTDM_post_b0.35x_f149.ppm" },
    @{ label="PTDN b0.35"; path="results\images\bunker_PTDN_post_b0.35x_f149.ppm" },
    @{ label="PTDO b0.35"; path="results\images\bunker_PTDO_post_b0.35x_f149.ppm" },
    @{ label="PTDP b0.35"; path="results\images\bunker_PTDP_post_b0.35x_f149.ppm" },
    @{ label="PTDQ b0.35"; path="results\images\bunker_PTDQ_post_b0.35x_f149.ppm" },
    @{ label="PTDR b0.35"; path="results\images\bunker_PTDR_post_b0.35x_f149.ppm" },
    @{ label="PTDS b0.35"; path="results\images\bunker_PTDS_post_b0.35x_f149.ppm" },
    @{ label="PTDT b0.35"; path="results\images\bunker_PTDT_post_b0.35x_f149.ppm" },
    @{ label="PTDU b0.35"; path="results\images\bunker_PTDU_post_b0.35x_f149.ppm" },
    @{ label="PTDV b0.35"; path="results\images\bunker_PTDV_post_b0.35x_f149.ppm" },
    @{ label="PTDW b0.35"; path="results\images\bunker_PTDW_post_b0.35x_f149.ppm" },
    @{ label="PTDX b0.35"; path="results\images\bunker_PTDX_post_b0.35x_f149.ppm" },
    @{ label="PTDY b0.35"; path="results\images\bunker_PTDY_post_b0.35x_f149.ppm" },
    @{ label="PTDZ b0.35"; path="results\images\bunker_PTDZ_post_b0.35x_f149.ppm" },
    @{ label="PTDZA b0.35"; path="results\images\bunker_PTDZA_post_b0.35x_f149.ppm" },
    @{ label="PTDZB b0.35"; path="results\images\bunker_PTDZB_post_b0.35x_f149.ppm" },
    @{ label="PTDZC b0.35"; path="results\images\bunker_PTDZC_post_b0.35x_f149.ppm" },
    @{ label="PTDZD b0.35"; path="results\images\bunker_PTDZD_post_b0.35x_f149.ppm" },
    @{ label="PTDZE b0.35"; path="results\images\bunker_PTDZE_post_b0.35x_f149.ppm" },
    @{ label="PTDZF b0.35"; path="results\images\bunker_PTDZF_post_b0.35x_f149.ppm" },
    @{ label="PTDZG b0.35"; path="results\images\bunker_PTDZG_post_b0.35x_f149.ppm" },
    @{ label="PTDZH b0.35"; path="results\images\bunker_PTDZH_post_b0.35x_f149.ppm" },
    @{ label="PTDZI b0.35"; path="results\images\bunker_PTDZI_post_b0.35x_f149.ppm" },
    @{ label="PTDZJ b0.35"; path="results\images\bunker_PTDZJ_post_b0.35x_f149.ppm" },
    @{ label="PTDZK b0.35"; path="results\images\bunker_PTDZK_post_b0.35x_f149.ppm" },
    @{ label="PTDZL b0.35"; path="results\images\bunker_PTDZL_post_b0.35x_f149.ppm" },
    @{ label="PTDZM b0.35"; path="results\images\bunker_PTDZM_post_b0.35x_f149.ppm" },
    @{ label="PTDZN b0.35"; path="results\images\bunker_PTDZN_post_b0.35x_f149.ppm" },
    @{ label="PTDZO b0.35"; path="results\images\bunker_PTDZO_post_b0.35x_f149.ppm" },
    @{ label="PTDZP b0.35"; path="results\images\bunker_PTDZP_post_b0.35x_f149.ppm" },
    @{ label="PTDZQ b0.35"; path="results\images\bunker_PTDZQ_post_b0.35x_f149.ppm" },
    @{ label="PTDZR b0.35"; path="results\images\bunker_PTDZR_post_b0.35x_f149.ppm" },
    @{ label="PTDZS b0.35"; path="results\images\bunker_PTDZS_post_b0.35x_f149.ppm" },
    @{ label="PTDZT b0.35"; path="results\images\bunker_PTDZT_post_b0.35x_f149.ppm" }
  )
}

$Crops = @(
  @{ label="circle wall"; x=28; y=12; w=58; h=58 },
  @{ label="ceiling wall"; x=58; y=14; w=70; h=60 },
  @{ label="doorway slab"; x=78; y=52; w=58; h=74 },
  @{ label="green wall"; x=130; y=28; w=46; h=98 }
)

function Read-Token([byte[]]$Bytes, [ref]$Index){
  while($Index.Value -lt $Bytes.Length -and [char]$Bytes[$Index.Value] -match '\s'){
    $Index.Value++
  }
  if($Index.Value -lt $Bytes.Length -and $Bytes[$Index.Value] -eq 35){
    while($Index.Value -lt $Bytes.Length -and $Bytes[$Index.Value] -ne 10){
      $Index.Value++
    }
    return Read-Token $Bytes $Index
  }
  $s = ""
  while($Index.Value -lt $Bytes.Length -and -not ([char]$Bytes[$Index.Value] -match '\s')){
    $s += [char]$Bytes[$Index.Value]
    $Index.Value++
  }
  return $s
}

function Read-PpmBitmap([string]$Path){
  $bytes = [System.IO.File]::ReadAllBytes($Path)
  $idx = 0
  $magic = Read-Token $bytes ([ref]$idx)
  if($magic -ne "P6"){ throw "Only binary P6 PPM is supported: $Path" }
  $w = [int](Read-Token $bytes ([ref]$idx))
  $h = [int](Read-Token $bytes ([ref]$idx))
  $max = [int](Read-Token $bytes ([ref]$idx))
  while($idx -lt $bytes.Length -and [char]$bytes[$idx] -match '\s'){ $idx++ }
  $bmp = [System.Drawing.Bitmap]::new($w,$h,[System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
  for($y=0; $y -lt $h; $y++){
    for($x=0; $x -lt $w; $x++){
      $r = [int]([math]::Round(([double]$bytes[$idx++] * 255.0) / $max))
      $g = [int]([math]::Round(([double]$bytes[$idx++] * 255.0) / $max))
      $b = [int]([math]::Round(([double]$bytes[$idx++] * 255.0) / $max))
      $bmp.SetPixel($x,$y,[System.Drawing.Color]::FromArgb($r,$g,$b))
    }
  }
  return $bmp
}

foreach($entry in $Images){
  if(-not (Test-Path $entry.path)){
    throw "Missing image: $($entry.path)"
  }
}

$maxCropW = ($Crops | ForEach-Object { $_.w } | Measure-Object -Maximum).Maximum
$maxCropH = ($Crops | ForEach-Object { $_.h } | Measure-Object -Maximum).Maximum
$labelH = 32
$pad = 14
$cellW = $maxCropW * $Scale + 2 * $pad
$cellH = $maxCropH * $Scale + $labelH + 2 * $pad
$leftLabelW = 140
$headerH = 44
$sheetW = $leftLabelW + $cellW * $Images.Count
$sheetH = $headerH + $cellH * $Crops.Count

$sheet = [System.Drawing.Bitmap]::new($sheetW,$sheetH,[System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
$g.Clear([System.Drawing.Color]::FromArgb(28,28,28))

$font = [System.Drawing.Font]::new("Arial", 13, [System.Drawing.FontStyle]::Regular)
$smallFont = [System.Drawing.Font]::new("Arial", 11, [System.Drawing.FontStyle]::Regular)
$white = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::White)
$muted = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(200,200,200))
$gridPen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(72,72,72), 1)

for($col=0; $col -lt $Images.Count; $col++){
  $x = $leftLabelW + $col * $cellW + $pad
  $g.DrawString($Images[$col].label, $font, $white, $x, 12)
}

$bitmaps = @{}
foreach($entry in $Images){
  $bitmaps[$entry.path] = Read-PpmBitmap $entry.path
}

for($row=0; $row -lt $Crops.Count; $row++){
  $crop = $Crops[$row]
  $rowY = $headerH + $row * $cellH
  $g.DrawString($crop.label, $font, $white, 12, $rowY + 16)
  $g.DrawString(("x={0} y={1} {2}x{3}" -f $crop.x,$crop.y,$crop.w,$crop.h), $smallFont, $muted, 12, $rowY + 42)
  for($col=0; $col -lt $Images.Count; $col++){
    $entry = $Images[$col]
    $cellX = $leftLabelW + $col * $cellW
    $g.DrawRectangle($gridPen, $cellX, $rowY, $cellW, $cellH)
    $srcRect = [System.Drawing.Rectangle]::new($crop.x,$crop.y,$crop.w,$crop.h)
    $dstW = $crop.w * $Scale
    $dstH = $crop.h * $Scale
    $dstX = $cellX + [int](($cellW - $dstW) / 2)
    $dstY = $rowY + $labelH + [int](($cellH - $labelH - $dstH) / 2)
    $dstRect = [System.Drawing.Rectangle]::new($dstX,$dstY,$dstW,$dstH)
    $g.DrawImage($bitmaps[$entry.path], $dstRect, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)
  }
}

$outDir = [System.IO.Path]::GetDirectoryName($Out)
if($outDir -and -not (Test-Path $outDir)){
  New-Item -ItemType Directory -Path $outDir | Out-Null
}
$sheet.Save($Out,[System.Drawing.Imaging.ImageFormat]::Png)

foreach($bmp in $bitmaps.Values){ $bmp.Dispose() }
$gridPen.Dispose()
$white.Dispose()
$muted.Dispose()
$font.Dispose()
$smallFont.Dispose()
$g.Dispose()
$sheet.Dispose()

Write-Output $Out
