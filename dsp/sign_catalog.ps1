# Writes libpxquake_skel.cat next to libpxquake_skel.so: a version-2 file catalog signed with a code-signing
# certificate of YOUR OWN from CurrentUser\My (the subject is matched against $env:PQ_CERT_SUBJECT, default
# "*PulseX*"; it must have its private key and be valid today).
# It creates nothing but the .cat in this folder, installs no certificate and changes no trust setting. Whether
# Windows then loads the module depends on whether the machine trusts that certificate - see README.md.
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot; if (-not $dir) { $dir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$skel = Join-Path $dir 'libpxquake_skel.so'; $cat = Join-Path $dir 'libpxquake_skel.cat'
if (-not (Test-Path $skel)) { throw "missing $skel - build it first" }
$subject = $env:PQ_CERT_SUBJECT; if (-not $subject) { $subject = '*PulseX*' }
$now = Get-Date
$s = New-Object System.Security.Cryptography.X509Certificates.X509Store 'My', 'CurrentUser'; $s.Open('ReadOnly')
$c = @($s.Certificates); $s.Close()
$cert = $c | Where-Object { $_.Subject -like $subject -and $_.HasPrivateKey -and $_.NotAfter -gt $now -and $_.NotBefore -le $now } |
        Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert) { throw "no valid certificate matching '$subject' in CurrentUser\My (set PQ_CERT_SUBJECT)" }
Write-Output ("using cert: " + $cert.Subject + "  valid until " + $cert.NotAfter)
if (Test-Path $cat) { Remove-Item $cat -Force }
New-FileCatalog -Path $skel -CatalogFilePath $cat -CatalogVersion 2 | Out-Null
$r = Set-AuthenticodeSignature -FilePath $cat -Certificate $cert -HashAlgorithm SHA256
Write-Output ("sign: " + $r.Status)
$t = Test-FileCatalog -CatalogFilePath $cat -Path $skel -Detailed
Write-Output ("Test-FileCatalog: " + $t.Status)
if ($t.Status -ne 'Valid') { throw "the catalog did not verify" }
