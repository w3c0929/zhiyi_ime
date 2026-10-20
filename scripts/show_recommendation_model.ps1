# 知语输入法：看看本机「上文推荐模型」（Laya）现在装的是哪一版、引擎有没有真的用它。
# 只读，不改任何东西：  pwsh -File scripts\show_recommendation_model.ps1
#
# 判定依据（docs/development.md 的模型一节）：
#   1) 安装目录 laya\laya.int8g.onnx 的 sha256 与大小就是模型的指纹：
#        1.4V2-Long-L12（1.7.69 起）  84,400,754 字节  sha256 82d0fd05…b529
#        1.4V2-Long-L18（1.7.59–1.7.68） 96,155,512 字节 sha256 4aa1e0a3…b714
#        1.5.4-1.5.8 的解码器模型      83,485,782 字节  且旁边多三个 laya_decoder*.onnx
#   2) %LOCALAPPDATA%\zhiyu\laya-cache\<键>\ 是引擎给「它打开过的模型」建的缓存，
#      键由模型/分词器文件的**大小与修改时间**算出来 —— 换了包里的模型，就会出现一个新的键目录，
#      它的时间就是你第一次用它的时候；旧模型的那个目录不再被碰。
$ErrorActionPreference = "Continue"
$expect_sha = "82D0FD05A5C2BFF7898BE518101D23DD03D0A5C4ECB8ABAECB397D42BEBB529"
$expect_size = 84400754

$dir = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ZhiyuIME' -ErrorAction SilentlyContinue).InstallLocation
if (-not $dir) { Write-Output "找不到安装信息（HKLM\...\Uninstall\ZhiyuIME）——知语输入法装了吗？"; exit 1 }
Write-Output "安装目录（活动版本）: $dir"

$srv = Get-Process zhiyu-server -ErrorAction SilentlyContinue
if ($srv) {
    $sv = (Get-Item $srv.Path -ErrorAction SilentlyContinue).VersionInfo.FileVersion
    Write-Output "后台服务在跑        : $($srv.Path)  内嵌版本 $sv"
} else {
    Write-Output "后台服务            : （现在没在跑）"
}

Write-Output ""
Write-Output "[1] 模型文件"
Get-ChildItem "$dir\laya" | ForEach-Object { "    {0,-26} {1,12:N0} 字节" -f $_.Name, $_.Length }
$onnx = "$dir\laya\laya.int8g.onnx"
if (-not (Test-Path $onnx)) { Write-Output "    !! 没有 laya.int8g.onnx —— 推荐的模型没有装上"; exit 1 }
$item = Get-Item $onnx
$sha = (Get-FileHash $onnx -Algorithm SHA256).Hash
$dec = @(Get-ChildItem "$dir\laya" -Filter "laya_decoder*.onnx" -ErrorAction SilentlyContinue).Count
$vj = Get-Content "$dir\laya\unified_vocab.json" -Raw | ConvertFrom-Json
Write-Output "    sha256 = $sha"
Write-Output ("    新模型（1.4V2-Long-L12）？ {0}   （大小 {1}，应为 {2}）" -f ($sha -eq $expect_sha), $item.Length, $expect_size)
Write-Output "    旧的解码器图 laya_decoder*.onnx 个数 = $dec  （新模型应为 0）"
Write-Output "    unified_vocab: choice_format=$($vj.choice_format)  decoder=[$($vj.decoder)]  （新模型：ctx-first、decoder 为空）"

Write-Output ""
Write-Output "[2] 引擎真的打开过它没有（每个模型一份缓存，键含模型文件的大小/时间）"
$cache = "$env:LOCALAPPDATA\zhiyu\laya-cache"
if (Test-Path $cache) {
    Get-ChildItem $cache | Sort-Object LastWriteTime -Descending | Select-Object -First 6 | ForEach-Object {
        "    {0}  最后写入 {1}" -f $_.Name, $_.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss")
    }
    Write-Output "    ↑ 最新的那个目录名就是你升级后第一次用推荐模型时新建的；旧模型的那个目录时间停在升级前"
} else {
    Write-Output "    （还没有 $cache：说明这个模型还没被用过——打着字用一次就有了）"
}

Write-Output ""
Write-Output "[3] 引擎版本（应与刚装的版本号一致）"
foreach ($f in @("zhiyu-server.exe", "zhiyu-settings.exe", "zhiyu_tsf_x64.dll")) {
    $p = Join-Path $dir $f
    if (Test-Path $p) { "    {0,-22} 内嵌版本 {1}" -f $f, (Get-Item $p).VersionInfo.FileVersion }
}
Write-Output ""
Write-Output "结论怎么读：[1] 的 sha256/size 说明「装的是哪个模型」，[2] 里出现新的键目录说明「引擎确实用了它」；"
Write-Output "两条都满足 = 现在用的就是合并后的 1.4V2-Long-L12。另外打字时候选第一页仍会按上文重排（推荐的那个带星标）、"
Write-Output "打完长拼音停顿后仍会组句，就是它在工作的表现。"
