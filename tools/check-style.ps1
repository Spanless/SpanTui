# QML design-token lint.
#
# Enforces the design-system contract (docs/ui/design-system.md):
#   * retired token names are gone for good;
#   * spacing/radius/icon-size/geometry/font must come from the Theme ladders
#     (spaceSm…spaceLg, sizeSm…sizeLg, iconSm…iconLg, radiusXs…radiusXl,
#     dialogSm…dialogLg, uiSm…uiLg) — bare numeric values are violations;
#   * shadows come from the Theme elevation recipes (CrispSurface.elevation
#     or Theme.*Shadow* tokens) — no bare blur/offset/strength literals;
#   * translucency uses fadeSm/fadeMd/fadeLg or opacityDisabled; animation
#     uses durFast/durMid/durSlow (or a named property); stacking uses the
#     z* tier constants or a named layer property;
#   * tooltip delay/timeout come from tipDelay/tipDelayQuick/tipTimeout;
#   * text renders through UiText/MonoText/TerminalText/RenderText(Edit),
#     never a bare Text{}; business code and window roots never declare a
#     raw ToolTip{}/Popup{} (Tip/AppPopup family owns them) or a raw
#     TextField{}/TextInput{}/TextArea{} (UiField sizes itself from the font);
#   * common/ and base/ read preferences only through Appearance, and every
#     sized Appearance font call passes Appearance.fontRevision;
#   * chrome colors come from Theme roles — no hex literals outside the
#     palette tables and the color-picker domain files;
#   * ChromeButton density is `size: "sm"|"md"`, never `compact:`.
#
# Legitimate exceptions carry a `// style-ok: <reason>` marker on the same
# line (named constants like `gutterWidth: 42` are compliant by construction —
# the number lives on a named property, not inline). Whole files may be
# exempted below (test fixtures only); a rule may also name files in Except
# where that dimension is the file's own domain (the color picker's gradient
# stops). A rule with Scope applies only to files whose repo-relative path
# (forward slashes) matches it; CaseSensitive rules use -cmatch.
#
# Usage:  tools\check-style.ps1            (scans qml/)
# Exit:   0 clean, 1 violations found.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path $PSScriptRoot
$qmlDir = Join-Path $repoRoot "qml"

# Test fixtures whose geometry is deliberately off-grid.
$exemptFiles = @("BoundaryGallery.qml")
# Files that define the ladder itself.
$exemptFiles += @("Theme.qml")

$rules = @(
    @{
        Name    = "retired-token"
        Pattern = 'Theme\.(gapTight|gapSmall|gapItem|gapWide|gapSection|padItem|padPane|paneInset|treeRowInset|controlCompactH|controlH|inputH|rowSm|rowMd|rowLg|sessionRowH|spaceMicro|spaceXs|spaceXl|sizeXs|iconHero|dialogXs|treePad|treeGap|treeChevron|treeIcon)\b'
        Message = "retired token — use the three-tier space*/size*/icon*/dialog* ladders (design-system.md §5)"
        Always  = $true    # applies even in exempt files
    },
    @{
        Name    = "compact-bool"
        Pattern = '\bcompact:\s*(true|false)\b'
        Message = "ChromeButton density is size: `"sm`"|`"md`" — no compact: bool"
    },
    @{
        Name    = "bare-spacing"
        Pattern = '(?<![\w.])(?:anchors\.|Layout\.)?spacing:\s*([1-9]\d*)\b'
        Message = "bare numeric spacing — use Theme.spaceMicro…spaceXl"
    },
    @{
        Name    = "bare-radius"
        Pattern = '(?<![\w.])(?:anchors\.|Layout\.)?radius:\s*([2-9]\d*)\b'
        Message = "bare numeric radius — use the Theme.radiusXs…radiusXl ladder (design-system.md §5)"
    },
    @{
        # CrispSurface declares the channels; everyone else takes a recipe.
        Name    = "bare-shadow"
        Pattern = '\bshadow(?:BlurPx|OffsetYPx|Strength)\s*:(?>\s*)(?!0\s*$)[^/]*\d'
        Message = "hand-mixed shadow — use CrispSurface.elevation or a Theme elevation/focus token"
        Except  = @("CrispSurface.qml")
    },
    @{
        Name    = "bare-icon-size"
        Pattern = '(?<![\w.])size:\s*(\d+)'
        Message = "bare numeric size — Icon uses Theme.iconSm…iconHero, UiText uses `"sm`"|`"md`"|`"lg`""
    },
    @{
        Name    = "bare-font-size"
        Pattern = 'font\.pixelSize:\s*(\d+)'
        Message = "bare numeric font size — use Theme.uiSm/uiMd/uiLg or Appearance font functions"
    },
    @{
        Name    = "bare-geometry"
        Pattern = '(?<![\w.])(?:anchors\.|Layout\.)?(?:implicitWidth|implicitHeight|preferredWidth|preferredHeight|minimumWidth|minimumHeight|maximumWidth|maximumHeight|width|height|leftMargin|rightMargin|topMargin|bottomMargin|margins|padding|leftPadding|rightPadding|topPadding|bottomPadding)\s*:\s*([2-9]\d*)'
        Message = "bare numeric geometry — use a Theme ladder token, or name the constant with // style-ok: <reason>"
    },
    @{
        Name    = "bare-opacity"
        Pattern = '\bopacity\s*:[^\n]*\b0?\.\d+'
        Message = "bare opacity literal — use Theme.fadeSm/fadeMd/fadeLg or opacityDisabled"
    },
    @{
        Name    = "bare-duration"
        Pattern = '\bduration\s*:\s*\d+'
        Message = "bare animation duration — use Theme.durFast/durMid/durSlow or a named ms property"
    },
    @{
        Name    = "bare-z"
        Pattern = '(?<![\w.])z\s*:\s*-?\d+'
        Message = "bare numeric z — use the Theme.z* tiers or a named layer property"
    },
    @{
        Name    = "tooltip-timing"
        Pattern = '\b(delay|timeout)\s*:\s*\d+'
        Message = "bare tooltip timing — use Theme.tipDelay/tipDelayQuick/tipTimeout"
    },
    @{
        Name    = "raw-text"
        Pattern = '\bText\s*\{'
        Message = "bare Text{} — use UiText, MonoText, TerminalText, or RenderText"
        Except  = @("RenderText.qml")
    },
    @{
        # Popups and tooltips are owned by the common AppPopup/Tip family;
        # business code and window roots only consume them.
        Name          = "raw-control"
        Pattern       = '^\s*(ToolTip|Popup)\s*\{'
        CaseSensitive = $true
        Scope         = '^qml/(components/business/|[^/]+\.qml$)'
        Message       = "raw ToolTip/Popup — use Tip / AppPopup / AnchorPopup / AppDialog from common/"
    },
    @{
        # Text inputs size themselves from the font lane (Theme.fitText in
        # UiField); a raw input in business code reintroduces fixed heights.
        Name          = "raw-text-input"
        Pattern       = '^\s*(TextField|TextInput|TextArea)\s*\{'
        CaseSensitive = $true
        Scope         = '^qml/(components/business/|[^/]+\.qml$)'
        Message       = "raw TextField/TextInput/TextArea — use UiField (or another common/ input) so the height follows the font"
    },
    @{
        Name          = "shared-preferences"
        Pattern       = '\b(TerminalSettings|WorkspaceSettings)\b'
        CaseSensitive = $true
        Scope         = '^qml/components/(common|base)/'
        Message       = "common/base read preferences only through the Appearance singleton (components/overview.md)"
    },
    @{
        # QML cannot see what a function reads; the revision argument is the
        # binding's only dependency on typography changes.
        Name    = "font-revision"
        Pattern = 'Appearance\.(uiFont|uiAuxFont|formHintFont|editorFont)\((?![^)]*fontRevision)'
        Message = "sized Appearance font call without Appearance.fontRevision — the binding would never refresh"
    },
    @{
        Name    = "raw-color"
        Pattern = '"#[0-9a-fA-F]{3,8}"'
        Message = "hex literal outside the palette — use a Theme role or mark // style-ok: <reason>"
        Except  = @("ColorPickerPopup.qml", "ColorSchemeEditor.qml")
    }
)

$violations = 0
Get-ChildItem $qmlDir -Recurse -Filter *.qml | ForEach-Object {
    $file = $_
    $rel = $file.FullName.Substring($repoRoot.Length + 1)
    $relPosix = $rel -replace '\\', '/'
    $fileExempt = $exemptFiles -contains $file.Name
    $lineNo = 0
    foreach ($line in (Get-Content $file.FullName)) {
        $lineNo++
        $hasMarker = $line -match '//\s*style-ok'
        foreach ($rule in $rules) {
            if ($fileExempt -and -not $rule.Always) { continue }
            if ($rule.ContainsKey("Except") -and $rule.Except -contains $file.Name) { continue }
            if ($hasMarker) { continue }
            if ($rule.ContainsKey("Scope") -and $relPosix -notmatch $rule.Scope) { continue }
            $hit = if ($rule.ContainsKey("CaseSensitive") -and $rule.CaseSensitive) {
                $line -cmatch $rule.Pattern
            } else {
                $line -match $rule.Pattern
            }
            if ($hit) {
                $violations++
                Write-Output ("{0}:{1}  [{2}] {3}`n    {4}" -f $rel, $lineNo, $rule.Name, $rule.Message, $line.Trim())
            }
        }
    }
}

if ($violations -gt 0) {
    Write-Output "`nStyle check FAILED: $violations violation(s)."
    exit 1
}
Write-Output "Style check passed."
exit 0
