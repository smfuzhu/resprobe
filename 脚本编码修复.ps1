# ===========================================================================
#  ResProbe - script encoding normalizer / checker
#
#  RUN THIS WHENEVER the batch scripts start behaving strangely, e.g.:
#      'xxx' is not recognized as an internal or external command
#  where xxx looks like the tail of a `rem` comment line.
#
#  WHAT IT DOES - for every *.cmd / *.bat next to this file it:
#    1. strips a UTF-8 BOM if present
#    2. decodes the text (UTF-8 first, GBK fallback for legacy files)
#    3. removes a stray `chcp 65001 >nul` line (no longer needed)
#    4. writes the file back as UTF-8 without BOM
#    5. REPORTS every line that still contains a non-ASCII character
#
#  ENCODING MODEL OF THIS PROJECT - the rule is strict and simple:
#
#    * Script FILE NAMES may be Chinese (the file system handles that fine).
#    * Script CONTENT must be PURE ASCII.
#
#  Why: cmd.exe parses batch content using the OEM code page (936 here) and
#  re-reads the file by BYTE OFFSET. A multi-byte character desynchronises that
#  offset, so the second half of a comment line gets executed as a command.
#  `chcp 65001` on line 2 does NOT fix this reliably - it only changes how
#  already-read bytes are interpreted.
#
#  Consequence: an ASCII-only script cannot call a Chinese-named script, since
#  the file name itself would be non-ASCII. That is why the shared config block
#  is duplicated inside each script instead of living in one included file.
#
#  USAGE
#    powershell -NoProfile -ExecutionPolicy Bypass -File 脚本编码修复.ps1
#
#  Exit code: 0 = all clean, 1 = non-ASCII content found (needs manual fix)
#
#  This .ps1 itself is pure ASCII on purpose: Windows PowerShell 5.1 reads .ps1
#  files as ANSI when there is no BOM, so non-ASCII here would be fragile too.
# ===========================================================================

$ErrorActionPreference = 'Stop'

$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$utf8 = New-Object System.Text.UTF8Encoding($false)        # no BOM
$strictUtf8 = New-Object System.Text.UTF8Encoding($false, $true)
$gbk = [System.Text.Encoding]::GetEncoding(936)

$targets = @(Get-ChildItem -Path $dir -File |
             Where-Object { $_.Extension -eq '.cmd' -or $_.Extension -eq '.bat' })

if ($targets.Count -eq 0) {
    Write-Output "no .cmd/.bat found in $dir"
    exit 0
}

$dirty = 0

foreach ($f in $targets) {
    $bytes = [IO.File]::ReadAllBytes($f.FullName)
    $notes = @()

    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $bytes = $bytes[3..($bytes.Length - 1)]
        $notes += 'bom-removed'
    }

    $text = ''
    if ($bytes.Length -gt 0) {
        try { $text = $strictUtf8.GetString($bytes) }
        catch {
            $text = $gbk.GetString($bytes)
            $notes += 'gbk->utf8'
        }
    }

    $text = $text -replace "`r`n", "`n"
    $lines = $text -split "`n"

    # drop a stray chcp line (line 2 only, never a legitimate command)
    $kept = New-Object System.Collections.Generic.List[string]
    foreach ($ln in $lines) {
        if ($ln.Trim() -eq 'chcp 65001 >nul') { $notes += 'chcp-removed'; continue }
        $kept.Add($ln)
    }

    $final = ($kept.ToArray() -join "`r`n")
    [IO.File]::WriteAllText($f.FullName, $final, $utf8)

    if ($notes.Count -eq 0) { $notes += 'normalized' }
    Write-Output ("{0,-24} {1}" -f $f.Name, ($notes -join ','))

    # ---- report non-ASCII content --------------------------------------------
    $bad = @()
    for ($i = 0; $i -lt $final.Length; $i++) {
        $c = [int][char]$final[$i]
        if ($c -gt 126 -or ($c -lt 32 -and $c -ne 13 -and $c -ne 10 -and $c -ne 9)) {
            $s = [Math]::Max(0, $i - 40)
            $len = [Math]::Min(90, $final.Length - $s)
            $snippet = ($final.Substring($s, $len) -replace "[`r`n]", ' ')
            $bad += ("U+{0:X4}" -f $c)
        }
    }
    if ($bad.Count -gt 0) {
        $dirty++
        Write-Output ("        !! {0} non-ASCII char(s): {1}" -f $bad.Count, (($bad | Select-Object -Unique) -join ' '))
        Write-Output  "        !! rewrite this file in pure ASCII (see header of this script)"
    }
}

Write-Output ""
if ($dirty -eq 0) {
    Write-Output ("OK - " + $targets.Count + " script(s) are pure ASCII, UTF-8, no BOM")
    exit 0
} else {
    Write-Output ("FAIL - " + $dirty + " script(s) contain non-ASCII content")
    exit 1
}
