# Icon asset lint.
#
# UiIconCatalog loads every SVG under assets/icons at startup and fails closed
# (no icon at all) on a duplicate name, a broken SVG or a broken motion file;
# a name nothing provides shows an empty icon. Checks (docs/ui/icons.md):
#   * stem   - every SVG file name (the icon name) is unique;
#   * svg    - every SVG is well-formed XML rooted at <svg> with at least one
#              <path d> outside <defs>;
#   * qml    - every literal `icon: "name"` / `iconName: "name"` in qml/ names
#              an SVG;
#   * files  - every icon file-icons.json maps a file, folder, extension or
#              file name to names an SVG;
#   * motion - every *.motion.json sits beside the SVG of its name, parses as
#              a JSON object with parts, and its parts' path/morph name path
#              ids of that SVG.
#
# Usage:  tools\check-icons.ps1
# Exit:   0 clean, 1 violations found.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot
$iconRoot = Join-Path $repoRoot 'assets/icons'
$violations = [System.Collections.Generic.List[string]]::new()
$utf8 = [System.Text.UTF8Encoding]::new($false)

function Add-Violation([string]$Rule, [string]$Location, [string]$Message) {
    $script:violations.Add(("{0}  [{1}] {2}" -f $Location, $Rule, $Message))
}

function Get-RelativePath([string]$Path) {
    return $Path.Substring($repoRoot.Length + 1).Replace('\', '/')
}

function Get-LineNumber([string]$Text, [int]$Offset) {
    return ([regex]::Matches($Text.Substring(0, $Offset), "`n")).Count + 1
}

# The value of a JSON object's property, or $null when it has none.
function Get-JsonValue($Object, [string]$Name) {
    if ($null -eq $Object -or $Object -isnot [System.Management.Automation.PSCustomObject]) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

# Icon name -> repo-relative SVG path, and icon name -> its path ids.
$svgFiles = [System.Collections.Generic.Dictionary[string, string]]::new([System.StringComparer]::Ordinal)
$pathIds = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
$svgs = Get-ChildItem -LiteralPath $iconRoot -Recurse -File -Filter '*.svg' |
    Where-Object { $_.Extension -eq '.svg' } | Sort-Object FullName
foreach ($svg in $svgs) {
    $relative = Get-RelativePath $svg.FullName
    $name = [System.IO.Path]::GetFileNameWithoutExtension($svg.Name)
    if ($svgFiles.ContainsKey($name)) {
        Add-Violation 'stem' $relative "icon name '$name' is also $($svgFiles[$name])"
        continue
    }
    $svgFiles[$name] = $relative

    $document = [System.Xml.XmlDocument]::new()
    $document.XmlResolver = $null
    try {
        $document.Load($svg.FullName)
    } catch {
        Add-Violation 'svg' $relative "not well-formed XML: $($_.Exception.Message)"
        continue
    }
    if ($document.DocumentElement.LocalName -ne 'svg') {
        Add-Violation 'svg' $relative "root element is <$($document.DocumentElement.LocalName)>, not <svg>"
        continue
    }
    $ids = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    $visible = 0
    foreach ($path in $document.GetElementsByTagName('path')) {
        if (-not $path.GetAttribute('d').Trim()) { continue }
        $id = $path.GetAttribute('id')
        if ($id) { [void]$ids.Add($id) }
        $inDefs = $false
        for ($node = $path.ParentNode; $null -ne $node; $node = $node.ParentNode) {
            if ($node.LocalName -eq 'defs') { $inDefs = $true; break }
        }
        if (-not $inDefs) { $visible++ }
    }
    if ($visible -eq 0) { Add-Violation 'svg' $relative 'no <path d> outside <defs>' }
    $pathIds[$name] = $ids
}

$qmlReference = [regex]'(?:iconName|\bicon):\s*"([a-z0-9-]+)"'
foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $repoRoot 'qml') -Recurse -File -Filter '*.qml')) {
    $text = [System.IO.File]::ReadAllText($file.FullName, $utf8)
    foreach ($match in $qmlReference.Matches($text)) {
        $name = $match.Groups[1].Value
        if (-not $svgFiles.ContainsKey($name)) {
            $location = '{0}:{1}' -f (Get-RelativePath $file.FullName), (Get-LineNumber $text $match.Index)
            Add-Violation 'qml' $location "no SVG named '$name' under assets/icons"
        }
    }
}

$fileIconsPath = Join-Path $iconRoot 'file-icons.json'
$fileIconsRelative = Get-RelativePath $fileIconsPath
try {
    $fileIcons = [System.IO.File]::ReadAllText($fileIconsPath, $utf8) | ConvertFrom-Json
} catch {
    Add-Violation 'files' $fileIconsRelative "not valid JSON: $($_.Exception.Message)"
    $fileIcons = $null
}
if ($null -ne $fileIcons) {
    $mapped = [System.Collections.Generic.List[object]]::new()
    foreach ($key in @('file', 'folder', 'folderExpanded', 'rootFolder', 'rootFolderExpanded')) {
        $mapped.Add(@($key, (Get-JsonValue $fileIcons $key)))
    }
    foreach ($mapName in @('fileExtensions', 'fileNames')) {
        $map = Get-JsonValue $fileIcons $mapName
        if ($null -eq $map) { continue }
        foreach ($entry in $map.PSObject.Properties) { $mapped.Add(@("$mapName.$($entry.Name)", $entry.Value)) }
    }
    foreach ($pair in $mapped) {
        $name = [string]$pair[1]
        if ($name -and -not $svgFiles.ContainsKey($name)) {
            Add-Violation 'files' $fileIconsRelative "$($pair[0]) maps to '$name', which no SVG provides"
        }
    }
}

foreach ($motion in (Get-ChildItem -LiteralPath $iconRoot -Recurse -File -Filter '*.motion.json')) {
    $relative = Get-RelativePath $motion.FullName
    $name = $motion.Name.Substring(0, $motion.Name.Length - '.motion.json'.Length)
    $beside = Join-Path $motion.DirectoryName "$name.svg"
    if (-not (Test-Path -LiteralPath $beside -PathType Leaf)) {
        Add-Violation 'motion' $relative "no $name.svg beside it: the catalog never loads it"
        continue
    }
    try {
        $spec = [System.IO.File]::ReadAllText($motion.FullName, $utf8) | ConvertFrom-Json
    } catch {
        Add-Violation 'motion' $relative "not valid JSON: $($_.Exception.Message)"
        continue
    }
    $parts = @(Get-JsonValue $spec 'parts' | Where-Object { $null -ne $_ })
    if ($parts.Count -eq 0) {
        Add-Violation 'motion' $relative 'no parts'
        continue
    }
    if (-not $pathIds.ContainsKey($name)) { continue }   # the SVG itself is already reported
    foreach ($part in $parts) {
        foreach ($key in @('path', 'morph')) {
            $id = [string](Get-JsonValue $part $key)
            if ($id -and -not $pathIds[$name].Contains($id)) {
                Add-Violation 'motion' $relative "part '$(Get-JsonValue $part 'id')' $key '$id' is no path id of $name.svg"
            }
        }
    }
}

if ($violations.Count -gt 0) {
    $violations | ForEach-Object { Write-Output $_ }
    Write-Output "`nIcon check FAILED: $($violations.Count) violation(s)."
    exit 1
}
Write-Output "Icon check passed ($($svgFiles.Count) icons)."
exit 0
