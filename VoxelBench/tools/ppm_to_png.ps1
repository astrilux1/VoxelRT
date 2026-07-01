param(
  [Parameter(Mandatory=$true)]
  [string[]]$Images
)

Add-Type -AssemblyName System.Drawing

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

foreach($src in $Images){
  $bytes = [System.IO.File]::ReadAllBytes($src)
  $idx = 0
  $magic = Read-Token $bytes ([ref]$idx)
  if($magic -ne "P6"){ throw "Only binary P6 PPM is supported: $src" }
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
  $dst = [System.IO.Path]::ChangeExtension($src, ".png")
  $bmp.Save($dst,[System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Output $dst
}
