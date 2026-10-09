# ===========================================================================
#  ResProbe - manifest validator (runs as part of the build)
#
#  WHY THIS EXISTS
#  A broken application manifest does not fail the build. It fails at PROCESS
#  START with a message that says nothing about the real cause:
#
#      The application has failed to start because its side by side
#      configuration is incorrect.
#
#  Two ways to get there, both already hit once in this project:
#    1. two consecutive hyphens inside an XML comment (illegal XML;
#       so you cannot even quote the sequence while documenting it)
#    2. a UTF-8 BOM before the XML declaration
#
#  This script turns those into a build error instead of a runtime mystery.
#
#  ENCODING: pure ASCII on purpose - PowerShell 5.1 reads a .ps1 without a BOM
#  as ANSI, so non-ASCII content here would be fragile.
#
#  Exit code: 0 = ok, 1 = invalid (make stops)
# ===========================================================================

$ErrorActionPreference = 'Stop'

$path = Join-Path $PSScriptRoot 'resprobe.manifest'
$problems = @()

if (-not (Test-Path -LiteralPath $path)) {
    Write-Output "[manifest] FAIL - not found: $path"
    exit 1
}

$bytes = [IO.File]::ReadAllBytes($path)

# ---- 1. no UTF-8 BOM ------------------------------------------------------
$hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and
           $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
if ($hasBom) {
    $problems += 'file starts with a UTF-8 BOM'
}

# ---- 2. no non-ASCII bytes (keep the manifest ASCII so windres and the ----
#         SxS loader both see exactly the same bytes)                      ---
$nonAscii = 0
foreach ($b in $bytes) { if ($b -gt 127) { $nonAscii++ } }
if ($nonAscii -gt 0) {
    $problems += "$nonAscii non-ASCII byte(s); keep this file ASCII only"
}

$text = [Text.Encoding]::UTF8.GetString($bytes)

# ---- 3. no "--" inside comments ------------------------------------------
# Scan comment bodies only, so a hyphen pair in ordinary text stays allowed.
$commentRe = [regex]'(?s)<!--(.*?)-->'
foreach ($m in $commentRe.Matches($text)) {
    $body = $m.Groups[1].Value
    if ($body -match '--') {
        $problems += 'an XML comment contains a double hyphen (illegal XML)'
        break
    }
    if ($body.TrimEnd().EndsWith('-')) {
        $problems += 'an XML comment body ends with a hyphen (illegal XML)'
        break
    }
}

# ---- 4. no unterminated comment ------------------------------------------
$open  = ([regex]::Matches($text, '<!--')).Count
$close = ([regex]::Matches($text, '-->')).Count
if ($open -ne $close) {
    $problems += "unbalanced XML comments (open=$open close=$close)"
}

# ---- 5. must actually parse as XML ---------------------------------------
if ($problems.Count -eq 0) {
    try {
        $doc = New-Object System.Xml.XmlDocument
        $doc.LoadXml($text)
        $root = $doc.DocumentElement

        if ($root.Name -ne 'assembly') {
            $problems += "root element is <$($root.Name)>, expected <assembly>"
        }

        $lvl = $root.SelectSingleNode(
            '/*[local-name()="assembly"]/*[local-name()="trustInfo"]' +
            '/*[local-name()="security"]/*[local-name()="requestedPrivileges"]' +
            '/*[local-name()="requestedExecutionLevel"]')
        if ($null -eq $lvl) {
            $problems += 'requestedExecutionLevel is missing'
        } else {
            $value = $lvl.GetAttribute('level')
            if ($value -notin @('asInvoker', 'highestAvailable', 'requireAdministrator')) {
                $problems += "unexpected requestedExecutionLevel level: '$value'"
            } else {
                Write-Output "[manifest] requestedExecutionLevel = $value"
            }
        }
    } catch {
        $problems += ('XML parse failed: ' + $_.Exception.Message)
    }
}

if ($problems.Count -gt 0) {
    Write-Output '[manifest] FAIL'
    foreach ($p in $problems) { Write-Output ('  - ' + $p) }
    Write-Output '  (a broken manifest shows up only at process start as'
    Write-Output '   "side by side configuration is incorrect")'
    exit 1
}

Write-Output ("[manifest] OK ({0} bytes, ASCII, no BOM)" -f $bytes.Length)
exit 0
