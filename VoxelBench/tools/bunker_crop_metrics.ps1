param(
  [string]$Ref = "results\images\ref_bunker_post.ppm",
  [string[]]$Images,
  [switch]$Csv
)

if(-not $Images -or $Images.Count -eq 0){
  $Images = @(
    "results\images\bunker_FCGIX_b1.0x_f149.ppm",
    "results\images\bunker_PTD_b0.6x_f149.ppm",
    "results\images\bunker_PTAA_b0.6x_f149.ppm",
    "results\images\bunker_PTAB_b0.4x_f149.ppm",
    "results\images\bunker_PTAD_b0.4x_f149.ppm",
    "results\images\bunker_PTAF_b0.4x_f149.ppm",
    "results\images\bunker_PTAG_b0.4x_f149.ppm",
    "results\images\bunker_PTAH_b0.4x_f149.ppm",
    "results\images\bunker_PTAI_b0.4x_f149.ppm",
    "results\images\bunker_PTAJ_b0.4x_f149.ppm",
    "results\images\bunker_PTAK_b0.4x_f149.ppm",
    "results\images\bunker_PTAL_b0.4x_f149.ppm",
    "results\images\bunker_PTAM_b0.4x_f149.ppm",
    "results\images\bunker_PTAN_b0.4x_f149.ppm",
    "results\images\bunker_PTAO_b0.4x_f149.ppm",
    "results\images\bunker_PTAP_b0.4x_f149.ppm",
    "results\images\bunker_PTAQ_b0.4x_f149.ppm",
    "results\images\bunker_PTAR_b0.4x_f149.ppm",
    "results\images\bunker_PTAS_b0.4x_f149.ppm",
    "results\images\bunker_PTAT_b0.4x_f149.ppm"
  )
}

$Crops = @(
  @{ name="circle_wall"; x=28; y=12; w=58; h=58 },
  @{ name="ceiling_wall"; x=58; y=14; w=70; h=60 },
  @{ name="doorway_slab"; x=78; y=52; w=58; h=74 },
  @{ name="green_wall"; x=130; y=28; w=46; h=98 }
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

function Read-PpmLuma([string]$Path){
  $bytes = [System.IO.File]::ReadAllBytes($Path)
  $idx = 0
  $magic = Read-Token $bytes ([ref]$idx)
  if($magic -ne "P6"){ throw "Only binary P6 PPM is supported: $Path" }
  $w = [int](Read-Token $bytes ([ref]$idx))
  $h = [int](Read-Token $bytes ([ref]$idx))
  $max = [int](Read-Token $bytes ([ref]$idx))
  while($idx -lt $bytes.Length -and [char]$bytes[$idx] -match '\s'){ $idx++ }
  $lum = New-Object double[] ($w*$h)
  for($i=0; $i -lt $w*$h; $i++){
    $r = [double]$bytes[$idx++] / $max
    $g = [double]$bytes[$idx++] / $max
    $b = [double]$bytes[$idx++] / $max
    $lum[$i] = 0.2126*$r + 0.7152*$g + 0.0722*$b
  }
  [pscustomobject]@{ w=$w; h=$h; lum=$lum; path=$Path }
}

function Solve-Plane([double]$s00,[double]$sx,[double]$sy,[double]$sxx,[double]$sxy,[double]$syy,[double]$b0,[double]$bx,[double]$by){
  $det = $s00*($sxx*$syy - $sxy*$sxy) - $sx*($sx*$syy - $sxy*$sy) + $sy*($sx*$sxy - $sxx*$sy)
  if([math]::Abs($det) -lt 1e-12){ return $null }
  $detA = $b0*($sxx*$syy - $sxy*$sxy) - $sx*($bx*$syy - $sxy*$by) + $sy*($bx*$sxy - $sxx*$by)
  $detB = $s00*($bx*$syy - $sxy*$by) - $b0*($sx*$syy - $sxy*$sy) + $sy*($sx*$by - $bx*$sy)
  $detC = $s00*($sxx*$by - $bx*$sxy) - $sx*($sx*$by - $bx*$sy) + $b0*($sx*$sxy - $sxx*$sy)
  @(($detA / $det), ($detB / $det), ($detC / $det))
}

function Crop-Metrics($RefImg, $Img, $Crop){
  $x0=$Crop.x; $y0=$Crop.y; $w=$Crop.w; $h=$Crop.h
  $n=0; $mse=0.0
  $s00=0.0; $sx=0.0; $sy=0.0; $sxx=0.0; $sxy=0.0; $syy=0.0
  $b0=0.0; $bx=0.0; $by=0.0
  $r0=0.0; $rx=0.0; $ry=0.0
  for($yy=$y0; $yy -lt $y0+$h; $yy++){
    for($xx=$x0; $xx -lt $x0+$w; $xx++){
      if($xx -lt 0 -or $yy -lt 0 -or $xx -ge $Img.w -or $yy -ge $Img.h){ continue }
      $idx=$yy*$Img.w+$xx
      $v=$Img.lum[$idx]
      $rv=$RefImg.lum[$idx]
      $d=$v-$rv
      $mse += $d*$d
      $fx=[double]($xx-$x0)
      $fy=[double]($yy-$y0)
      $s00+=1; $sx+=$fx; $sy+=$fy; $sxx+=$fx*$fx; $sxy+=$fx*$fy; $syy+=$fy*$fy
      $b0+=$v; $bx+=$v*$fx; $by+=$v*$fy
      $r0+=$rv; $rx+=$rv*$fx; $ry+=$rv*$fy
      $n++
    }
  }
  $fit = Solve-Plane $s00 $sx $sy $sxx $sxy $syy $b0 $bx $by
  $rfit = Solve-Plane $s00 $sx $sy $sxx $sxy $syy $r0 $rx $ry
  if($null -eq $fit -or $null -eq $rfit){
    return [pscustomobject]@{
      image = [System.IO.Path]::GetFileName($Img.path)
      crop = $Crop.name
      psnr = [double]::NaN
      plane_rms = [double]::NaN
      ref_plane_rms = [double]::NaN
      excess_rms = [double]::NaN
    }
  }
  $res=0.0; $rres=0.0
  for($yy=$y0; $yy -lt $y0+$h; $yy++){
    for($xx=$x0; $xx -lt $x0+$w; $xx++){
      if($xx -lt 0 -or $yy -lt 0 -or $xx -ge $Img.w -or $yy -ge $Img.h){ continue }
      $idx=$yy*$Img.w+$xx
      $fx=[double]($xx-$x0)
      $fy=[double]($yy-$y0)
      $pv=$fit[0]+$fit[1]*$fx+$fit[2]*$fy
      $pr=$rfit[0]+$rfit[1]*$fx+$rfit[2]*$fy
      $dl=$Img.lum[$idx]-$pv
      $dr=$RefImg.lum[$idx]-$pr
      $res+=$dl*$dl
      $rres+=$dr*$dr
    }
  }
  $mse /= [math]::Max(1,$n)
  $psnr = if($mse -le 1e-15){ 99.0 } else { -10.0*[math]::Log10($mse) }
  $rms = [math]::Sqrt($res/[math]::Max(1,$n))
  $rrms = [math]::Sqrt($rres/[math]::Max(1,$n))
  [pscustomobject]@{
    image = [System.IO.Path]::GetFileName($Img.path)
    crop = $Crop.name
    psnr = [math]::Round($psnr,2)
    plane_rms = [math]::Round($rms,5)
    ref_plane_rms = [math]::Round($rrms,5)
    excess_rms = [math]::Round($rms-$rrms,5)
  }
}

$refImg = Read-PpmLuma $Ref
$rows = @()
foreach($path in $Images){
  $img = Read-PpmLuma $path
  if($img.w -ne $refImg.w -or $img.h -ne $refImg.h){ throw "Dimension mismatch: $path" }
  foreach($crop in $Crops){
    $rows += Crop-Metrics $refImg $img $crop
  }
}

if($Csv){
  $rows | ConvertTo-Csv -NoTypeInformation
} else {
  $rows | Format-Table -AutoSize
}
