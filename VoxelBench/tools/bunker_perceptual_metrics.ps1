param(
  [string]$Ref = "results\images\ref_bunker_post.ppm",
  [string[]]$Images,
  [switch]$Csv
)

if(-not $Images -or $Images.Count -eq 0){
  $Images = @(
    "results\images\bunker_PTAI_b0.45x_f149.ppm",
    "results\images\bunker_PTBG_b0.35x_f149.ppm",
    "results\images\bunker_PTBQ_b0.35x_f149.ppm",
    "results\images\bunker_PTBR_b0.35x_f149.ppm",
    "results\images\bunker_PTBR_b0.40x_f149.ppm",
    "results\images\bunker_PTCI_b0.35x_f149.ppm",
    "results\images\bunker_PTCJ_b0.35x_f149.ppm",
    "results\images\bunker_PTCK_b0.35x_f149.ppm",
    "results\images\bunker_PTCL_b0.35x_f149.ppm",
    "results\images\bunker_PTCM_b0.35x_f149.ppm",
    "results\images\bunker_PTCN_b0.35x_f149.ppm",
    "results\images\bunker_PTCO_b0.35x_f149.ppm",
    "results\images\bunker_PTCP_b0.35x_f149.ppm",
    "results\images\bunker_PTCQ_b0.35x_f149.ppm",
    "results\images\bunker_PTCR_post_b0.35x_f149.ppm",
    "results\images\bunker_PTCS_post_b0.35x_f149.ppm",
    "results\images\bunker_PTCT_post_b0.35x_f149.ppm",
    "results\images\bunker_PTCU_post_b0.35x_f149.ppm",
    "results\images\bunker_PTCV_post_b0.35x_f149.ppm",
    "results\images\bunker_PTCW_post_b0.35x_f149.ppm",
    "results\images\bunker_PTCX_post_b0.35x_f149.ppm",
    "results\images\bunker_PTCY_post_b0.35x_f149.ppm",
    "results\images\bunker_PTCZ_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDA_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDB_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDC_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDD_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDE_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDF_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDG_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDH_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDI_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDJ_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDK_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDL_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDM_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDN_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDO_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDP_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDQ_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDR_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDS_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDT_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDU_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDV_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDW_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDX_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDY_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZ_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZA_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZB_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZC_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZD_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZE_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZF_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZG_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZH_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZI_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZJ_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZK_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZL_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZM_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZN_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZO_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZP_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZQ_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZR_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZS_post_b0.35x_f149.ppm",
    "results\images\bunker_PTDZT_post_b0.35x_f149.ppm",
    "results\images\bunker_PTBX_b0.35x_f149.ppm",
    "results\images\bunker_PTCH_b0.35x_f149.ppm"
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

function Read-PpmRgb([string]$Path){
  $bytes = [System.IO.File]::ReadAllBytes($Path)
  $idx = 0
  $magic = Read-Token $bytes ([ref]$idx)
  if($magic -ne "P6"){ throw "Only binary P6 PPM is supported: $Path" }
  $w = [int](Read-Token $bytes ([ref]$idx))
  $h = [int](Read-Token $bytes ([ref]$idx))
  $max = [int](Read-Token $bytes ([ref]$idx))
  while($idx -lt $bytes.Length -and [char]$bytes[$idx] -match '\s'){ $idx++ }

  $r = New-Object double[] ($w*$h)
  $g = New-Object double[] ($w*$h)
  $b = New-Object double[] ($w*$h)
  $lum = New-Object double[] ($w*$h)
  for($i=0; $i -lt $w*$h; $i++){
    $rr = [double]$bytes[$idx++] / $max
    $gg = [double]$bytes[$idx++] / $max
    $bb = [double]$bytes[$idx++] / $max
    $r[$i] = $rr
    $g[$i] = $gg
    $b[$i] = $bb
    $lum[$i] = 0.2126*$rr + 0.7152*$gg + 0.0722*$bb
  }
  [pscustomobject]@{ w=$w; h=$h; r=$r; g=$g; b=$b; lum=$lum; path=$Path }
}

function Solve-Plane([double]$s00,[double]$sx,[double]$sy,[double]$sxx,[double]$sxy,[double]$syy,[double]$b0,[double]$bx,[double]$by){
  $det = $s00*($sxx*$syy - $sxy*$sxy) - $sx*($sx*$syy - $sxy*$sy) + $sy*($sx*$sxy - $sxx*$sy)
  if([math]::Abs($det) -lt 1e-12){ return $null }
  $detA = $b0*($sxx*$syy - $sxy*$sxy) - $sx*($bx*$syy - $sxy*$by) + $sy*($bx*$sxy - $sxx*$by)
  $detB = $s00*($bx*$syy - $sxy*$by) - $b0*($sx*$syy - $sxy*$sy) + $sy*($sx*$by - $bx*$sy)
  $detC = $s00*($sxx*$by - $bx*$sxy) - $sx*($sx*$by - $bx*$sy) + $b0*($sx*$sxy - $sxx*$sy)
  @(($detA / $det), ($detB / $det), ($detC / $det))
}

function Get-Percentile([double[]]$Values, [double]$P){
  if($Values.Count -eq 0){ return 0.0 }
  $sorted = @($Values | Sort-Object)
  $idx = [int][math]::Floor(([math]::Max(0.0,[math]::Min(1.0,$P))) * ($sorted.Count - 1))
  [double]$sorted[$idx]
}

function Clamp-Div([double]$Num, [double]$Den){
  if([math]::Abs($Den) -lt 1e-12){ return [double]::NaN }
  $Num / $Den
}

function Saturation-Proxy([double]$R, [double]$G, [double]$B, [double]$L){
  [math]::Sqrt((($R-$L)*($R-$L) + ($G-$L)*($G-$L) + ($B-$L)*($B-$L)) / 3.0)
}

function Green-Opponent([double]$R, [double]$G, [double]$B){
  $G - 0.5*($R+$B)
}

function Crop-PerceptualMetrics($RefImg, $Img, $Crop){
  $x0=$Crop.x; $y0=$Crop.y; $w=$Crop.w; $h=$Crop.h

  $n=0
  $sumErr=0.0; $sumAbs=0.0; $sumSq=0.0
  $sumSat=0.0; $sumRefSat=0.0
  $greenDelta=0.0; $greenSq=0.0
  $chromaSq=0.0
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
      $sumErr += $d
      $sumAbs += [math]::Abs($d)
      $sumSq += $d*$d

      $sat = Saturation-Proxy $Img.r[$idx] $Img.g[$idx] $Img.b[$idx] $v
      $refSat = Saturation-Proxy $RefImg.r[$idx] $RefImg.g[$idx] $RefImg.b[$idx] $rv
      $sumSat += $sat
      $sumRefSat += $refSat

      $go = Green-Opponent $Img.r[$idx] $Img.g[$idx] $Img.b[$idx]
      $rgo = Green-Opponent $RefImg.r[$idx] $RefImg.g[$idx] $RefImg.b[$idx]
      $gd = $go - $rgo
      $greenDelta += $gd
      $greenSq += $gd*$gd

      $dr = $Img.r[$idx] - $RefImg.r[$idx]
      $dg = $Img.g[$idx] - $RefImg.g[$idx]
      $db = $Img.b[$idx] - $RefImg.b[$idx]
      $mean = ($dr+$dg+$db)/3.0
      $chromaSq += (($dr-$mean)*($dr-$mean) + ($dg-$mean)*($dg-$mean) + ($db-$mean)*($db-$mean)) / 3.0

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
  $res=0.0; $rres=0.0
  $imgPlaneResidual = New-Object double[] ($Img.w*$Img.h)
  $refPlaneResidual = New-Object double[] ($Img.w*$Img.h)
  if($null -ne $fit -and $null -ne $rfit){
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
        $imgPlaneResidual[$idx]=$dl
        $refPlaneResidual[$idx]=$dr
        $res+=$dl*$dl
        $rres+=$dr*$dr
      }
    }
  }

  $lowFreqErrSq=0.0; $lowFreqImgSq=0.0; $lowFreqRefSq=0.0; $lowFreqN=0
  if($null -ne $fit -and $null -ne $rfit){
    $lfRadius=4
    for($yy=$y0; $yy -lt $y0+$h; $yy++){
      for($xx=$x0; $xx -lt $x0+$w; $xx++){
        if($xx -lt 0 -or $yy -lt 0 -or $xx -ge $Img.w -or $yy -ge $Img.h){ continue }
        $sumI=0.0; $sumR=0.0; $cntLf=0
        for($dy=-$lfRadius; $dy -le $lfRadius; $dy++){
          for($dx=-$lfRadius; $dx -le $lfRadius; $dx++){
            $px=$xx+$dx; $py=$yy+$dy
            if($px -lt $x0 -or $py -lt $y0 -or $px -ge $x0+$w -or $py -ge $y0+$h){ continue }
            if($px -lt 0 -or $py -lt 0 -or $px -ge $Img.w -or $py -ge $Img.h){ continue }
            $pidx=$py*$Img.w+$px
            $sumI += $imgPlaneResidual[$pidx]
            $sumR += $refPlaneResidual[$pidx]
            $cntLf++
          }
        }
        if($cntLf -le 0){ continue }
        $li=$sumI/[double]$cntLf
        $lr=$sumR/[double]$cntLf
        $ld=$li-$lr
        $lowFreqErrSq += $ld*$ld
        $lowFreqImgSq += $li*$li
        $lowFreqRefSq += $lr*$lr
        $lowFreqN++
      }
    }
  }

  $gradMags = New-Object 'System.Collections.Generic.List[double]'
  for($yy=$y0; $yy -lt $y0+$h-1; $yy++){
    for($xx=$x0; $xx -lt $x0+$w-1; $xx++){
      if($xx -lt 0 -or $yy -lt 0 -or $xx+1 -ge $Img.w -or $yy+1 -ge $Img.h){ continue }
      $idx=$yy*$Img.w+$xx
      $idxX=$yy*$Img.w+$xx+1
      $idxY=($yy+1)*$Img.w+$xx
      $rdx=$RefImg.lum[$idxX]-$RefImg.lum[$idx]
      $rdy=$RefImg.lum[$idxY]-$RefImg.lum[$idx]
      $gradMags.Add([math]::Sqrt($rdx*$rdx+$rdy*$rdy))
    }
  }
  $edgeThreshold = Get-Percentile $gradMags.ToArray() 0.85

  $gn=0; $edgeN=0
  $refGradSq=0.0; $imgGradSq=0.0; $gradErrSq=0.0; $gradDot=0.0
  $edgeAbs=0.0; $edgeSigned=0.0; $edgeGradErrSq=0.0; $edgeBright=0
  for($yy=$y0; $yy -lt $y0+$h-1; $yy++){
    for($xx=$x0; $xx -lt $x0+$w-1; $xx++){
      if($xx -lt 0 -or $yy -lt 0 -or $xx+1 -ge $Img.w -or $yy+1 -ge $Img.h){ continue }
      $idx=$yy*$Img.w+$xx
      $idxX=$yy*$Img.w+$xx+1
      $idxY=($yy+1)*$Img.w+$xx

      $rdx=$RefImg.lum[$idxX]-$RefImg.lum[$idx]
      $rdy=$RefImg.lum[$idxY]-$RefImg.lum[$idx]
      $idxg=$Img.lum[$idxX]-$Img.lum[$idx]
      $idyg=$Img.lum[$idxY]-$Img.lum[$idx]
      $rmag=[math]::Sqrt($rdx*$rdx+$rdy*$rdy)
      $refGradSq += $rdx*$rdx+$rdy*$rdy
      $imgGradSq += $idxg*$idxg+$idyg*$idyg
      $gradErrSq += (($idxg-$rdx)*($idxg-$rdx) + ($idyg-$rdy)*($idyg-$rdy))
      $gradDot += $idxg*$rdx + $idyg*$rdy
      $gn++

      if($rmag -ge $edgeThreshold){
        $de = $Img.lum[$idx] - $RefImg.lum[$idx]
        $edgeAbs += [math]::Abs($de)
        $edgeSigned += $de
        $edgeGradErrSq += (($idxg-$rdx)*($idxg-$rdx) + ($idyg-$rdy)*($idyg-$rdy))
        if($de -gt 0.005){ $edgeBright++ }
        $edgeN++
      }
    }
  }

  $refGradRms = [math]::Sqrt($refGradSq/[math]::Max(1,$gn))
  $imgGradRms = [math]::Sqrt($imgGradSq/[math]::Max(1,$gn))
  $gradDen = [math]::Sqrt($refGradSq*$imgGradSq)
  $refPlaneRms = [math]::Sqrt($rres/[math]::Max(1,$n))
  $imgPlaneRms = [math]::Sqrt($res/[math]::Max(1,$n))

  [pscustomobject]@{
    image = [System.IO.Path]::GetFileName($Img.path)
    crop = $Crop.name
    mean_luma_err = [math]::Round($sumErr/[math]::Max(1,$n), 5)
    rms_luma_err = [math]::Round([math]::Sqrt($sumSq/[math]::Max(1,$n)), 5)
    grad_ratio = [math]::Round((Clamp-Div $imgGradRms $refGradRms), 4)
    grad_err_rms = [math]::Round([math]::Sqrt($gradErrSq/[math]::Max(1,$gn)), 5)
    grad_corr = [math]::Round((Clamp-Div $gradDot $gradDen), 4)
    plane_res_ratio = [math]::Round((Clamp-Div $imgPlaneRms $refPlaneRms), 4)
    lowfreq_res_ratio = [math]::Round((Clamp-Div ([math]::Sqrt($lowFreqImgSq/[math]::Max(1,$lowFreqN))) ([math]::Sqrt($lowFreqRefSq/[math]::Max(1,$lowFreqN)))), 4)
    lowfreq_err_rms = [math]::Round([math]::Sqrt($lowFreqErrSq/[math]::Max(1,$lowFreqN)), 5)
    edge_luma_mae = [math]::Round($edgeAbs/[math]::Max(1,$edgeN), 5)
    edge_signed_luma_err = [math]::Round($edgeSigned/[math]::Max(1,$edgeN), 5)
    edge_grad_err_rms = [math]::Round([math]::Sqrt($edgeGradErrSq/[math]::Max(1,$edgeN)), 5)
    edge_bright_frac = [math]::Round($edgeBright/[double][math]::Max(1,$edgeN), 4)
    mean_green_opp_delta = [math]::Round($greenDelta/[math]::Max(1,$n), 5)
    green_opp_rmse = [math]::Round([math]::Sqrt($greenSq/[math]::Max(1,$n)), 5)
    chroma_rmse = [math]::Round([math]::Sqrt($chromaSq/[math]::Max(1,$n)), 5)
    sat_ratio = [math]::Round((Clamp-Div ($sumSat/[math]::Max(1,$n)) ($sumRefSat/[math]::Max(1,$n))), 4)
  }
}

foreach($path in @($Ref) + $Images){
  if(-not (Test-Path $path)){ throw "Missing image: $path" }
}

$refImg = Read-PpmRgb $Ref
$rows = @()
foreach($path in $Images){
  $img = Read-PpmRgb $path
  if($img.w -ne $refImg.w -or $img.h -ne $refImg.h){ throw "Dimension mismatch: $path" }
  foreach($crop in $Crops){
    $rows += Crop-PerceptualMetrics $refImg $img $crop
  }
}

if($Csv){
  $rows | ConvertTo-Csv -NoTypeInformation
} else {
  $rows | Format-Table -AutoSize
}
