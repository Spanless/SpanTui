# Translation catalog lint.
#
# translations/SpanTui_zh_CN.ts must hold exactly the user-visible strings of
# the sources, each under the context Qt looks it up in at runtime
# (docs/workflow/localization.md). Checks:
#   * context     - every C++ tr() resolves to a class with a tr() context;
#   * missing     - every qsTr()/tr()/QCoreApplication::translate() string has
#                   an entry under its context;
#   * stale       - no entry that no source uses (an entry filed under another
#                   context is never shown);
#   * unfinished  - no entry marked unfinished or with an empty translation;
#   * placeholder - a translation keeps its source's %1/%L1/%n placeholders.
#
# Contexts: QML qsTr() takes `pragma Translator: "X"` or the file's base name.
# C++ tr() takes the moc name (namespaces included) of its Q_OBJECT/Q_GADGET
# class, or the Q_DECLARE_TR_FUNCTIONS argument; outside the class body, that
# of the class whose member function is defined around the call. lupdate is
# not the reference: it decodes some escapes differently and misses
# QCoreApplication::translate() contexts.
#
# Usage:  tools\check-translations.ps1
# Exit:   0 clean, 1 violations found.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot
$catalogRelative = 'translations/SpanTui_zh_CN.ts'
$utf8 = [System.Text.UTF8Encoding]::new($false)
# Catalog key: context + U+001F + source. Always split with the char
# overloads: culture-aware string search ignores the control character.
$keySeparator = [char]0x1f
$violations = [System.Collections.Generic.List[string]]::new()

function Add-Violation([string]$Rule, [string]$Location, [string]$Message) {
    $script:violations.Add(("{0}  [{1}] {2}" -f $Location, $Rule, $Message))
}

function Get-RelativePath([string]$Path) {
    return $Path.Substring($repoRoot.Length + 1).Replace('\', '/')
}

function New-Site([string]$File, [string]$Text, [int]$Offset) {
    return [pscustomobject]@{ File = $File; Text = $Text; Offset = $Offset }
}

function Format-Site($Site) {
    $line = ([regex]::Matches($Site.Text.Substring(0, $Site.Offset), "`n")).Count + 1
    return '{0}:{1}' -f $Site.File, $line
}

function Format-Key([string]$Key) {
    $at = $Key.IndexOf($keySeparator)
    $source = $Key.Substring($at + 1).Replace("`r", '\r').Replace("`n", '\n').Replace("`t", '\t')
    return '{0} :: "{1}"' -f $Key.Substring(0, $at), $source
}

# --- string literals -------------------------------------------------------

$stringSequence = '((?:"(?:\\.|[^"\\])*"\s*)+)'
$literal = [regex]'"((?:\\.|[^"\\])*)"'
$escape = [regex]'\\(?:u([0-9A-Fa-f]{4})|([\s\S]))'
$decodeEscape = [System.Text.RegularExpressions.MatchEvaluator]{
    param($match)
    if ($match.Groups[1].Success) { return [string][char][Convert]::ToInt32($match.Groups[1].Value, 16) }
    switch -CaseSensitive ($match.Groups[2].Value) {
        'n' { return "`n" }
        'r' { return "`r" }
        't' { return "`t" }
        'b' { return [string][char]8 }
        'f' { return [string][char]12 }
        'v' { return [string][char]11 }
    }
    return $match.Groups[2].Value
}

# The text of adjacent "..." literals, escapes decoded.
function ConvertFrom-LiteralSequence([string]$Expression) {
    $builder = [System.Text.StringBuilder]::new()
    foreach ($part in $literal.Matches($Expression)) {
        [void]$builder.Append($escape.Replace($part.Groups[1].Value, $decodeEscape))
    }
    return $builder.ToString()
}

# --- source keys -----------------------------------------------------------

$exactKeys = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
$keySites = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
$contextProblems = 0

function Add-Key([string]$Context, [string]$Source, $Site) {
    $key = $Context + $keySeparator + $Source
    if ($exactKeys.Add($key)) { $keySites[$key] = $Site }
}

function Add-ContextProblem($Site, [string]$Source, [string]$Problem) {
    $script:contextProblems++
    Add-Violation 'context' (Format-Site $Site) ('{0} ("{1}")' -f $Problem, $Source)
}

$qmlCall = [regex]('\bqsTr\s*\(\s*' + $stringSequence)
$qmlTranslator = [regex]'\bpragma\s+Translator\s*:\s*"((?:\\.|[^"\\])*)"'
$qmlFiles = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'qml') -Recurse -File |
    Where-Object { $_.Extension -eq '.qml' } | Sort-Object FullName
foreach ($file in $qmlFiles) {
    $text = [System.IO.File]::ReadAllText($file.FullName, $utf8)
    $relative = Get-RelativePath $file.FullName
    $pragma = $qmlTranslator.Match($text)
    $context = if ($pragma.Success) {
        $escape.Replace($pragma.Groups[1].Value, $decodeEscape)
    } else {
        [System.IO.Path]::GetFileNameWithoutExtension($file.Name)
    }
    foreach ($call in $qmlCall.Matches($text)) {
        $source = ConvertFrom-LiteralSequence $call.Groups[1].Value
        if ($source) { Add-Key $context $source (New-Site $relative $text $call.Index) }
    }
}

# C++: the file with comments and the contents of string/char literals
# blanked (quotes and newlines kept, offsets unchanged), so braces and calls
# are found by offset without tripping over text.
$nonCode = [regex](
    '(?<blank>//[^\n]*)' +
    '|(?<blank>/\*[\s\S]*?(?:\*/|\z))' +
    '|(?:(?<!\w)|(?<=[uUL8]))R"(?<blank>(?<delim>[^(]*)\([\s\S]*?\)\k<delim>)"' +
    '|"(?<blank>(?:\\[\s\S]|[^"\\\n])*)"?' +
    "|(?<!\d)'(?<blank>(?:\\[\s\S]|[^'\\\n])*)'?")
$blankOut = [System.Text.RegularExpressions.MatchEvaluator]{
    param($match)
    $group = $match.Groups['blank']
    $head = $group.Index - $match.Index
    return $match.Value.Substring(0, $head) + [regex]::Replace($group.Value, '[^\n]', ' ') +
        $match.Value.Substring($head + $group.Length)
}

$trRelevant = [regex]'\btr\b|\bQ_(?:OBJECT|GADGET)\b|\bQ_DECLARE_TR_FUNCTIONS\b'
$cppCall = [regex]'(?:\b(\w+)::|(?<![\w.>:]))tr\s*\(\s*"'
$literalAt = [regex]('\G' + $stringSequence)
$explicitCall = [regex]('QCoreApplication::translate\s*\(\s*"((?:\\.|[^"\\])*)"\s*,\s*' + $stringSequence)
$scopeToken = [regex]'[;{}]'
$namespaceHeader = [regex]'\bnamespace\s*([\w:]*)\s*$'
$externHeader = [regex]'\bextern\s*"\s*"\s*$'
$enumHeader = [regex]'\benum\b'
$classHeader = [regex]'(?:^|[\s;}>])(?:class|struct)\s+(?:[A-Z][A-Z0-9_]*\s+)?(\w+)(?:\s+final)?\s*(?::[^;{}]*)?$'
$declaredTr = [regex]'\bQ_DECLARE_TR_FUNCTIONS\s*\(\s*(\w+)\s*\)'
$metaObject = [regex]'\bQ_(?:OBJECT|GADGET)\b'
$definition = [regex]'\b(\w+)::~?\w+\s*\('
# At the "(" of a member definition: the balanced parameter list, qualifiers,
# then the body "{" or a constructor initializer ":" (not "::").
$startsDefinition = [regex]'\G\((?>[^()]+|\((?<depth>)|\)(?<-depth>))*(?(depth)(?!))\)\s*(?:(?:const|noexcept|override|final)\b\s*)*(?:\{|:(?!:))'

$scopeNamespace = 0
$scopeClass = 1
$scopeOther = 2
# Class name -> the tr() contexts of the classes so named.
$classContexts = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
# tr() calls named after a class (Class::tr, or inside Class::member), resolved
# once every class is known.
$classCalls = [System.Collections.Generic.List[object]]::new()

$cppFiles = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'src') -Recurse -File |
    Where-Object { ($_.Extension -eq '.cpp' -or $_.Extension -eq '.h') -and $_.FullName -notmatch '[\\/]third_party[\\/]' } |
    Sort-Object FullName
foreach ($file in $cppFiles) {
    $text = [System.IO.File]::ReadAllText($file.FullName, $utf8)
    $relative = Get-RelativePath $file.FullName

    foreach ($call in $explicitCall.Matches($text)) {
        $context = $escape.Replace($call.Groups[1].Value, $decodeEscape)
        $source = ConvertFrom-LiteralSequence $call.Groups[2].Value
        if ($context -and $source) { Add-Key $context $source (New-Site $relative $text $call.Index) }
    }
    if (-not $trRelevant.IsMatch($text)) { continue }
    $code = $nonCode.Replace($text, $blankOut)

    # Brace scopes: namespaces and classes decide a tr() context; function
    # bodies, lambdas and initializers are Other. One pass in offset order over
    # the scope tokens, the tr() calls and the X::member( definitions.
    $kinds = [System.Collections.Generic.List[int]]::new()
    $starts = [System.Collections.Generic.List[int]]::new()
    $ends = [System.Collections.Generic.List[int]]::new()
    $names = [System.Collections.Generic.List[string]]::new()
    $qualifiedNames = [System.Collections.Generic.List[string]]::new()
    $open = [System.Collections.Generic.List[int]]::new()
    $openNonNamespace = 0
    $segmentStart = 0
    $owner = ''           # class of the last member defined at namespace scope
    $fileCalls = [System.Collections.Generic.List[object]]::new()

    $tokens = $scopeToken.Matches($code)
    $calls = $cppCall.Matches($code)
    $definitions = $definition.Matches($code)
    $tokenIndex = 0
    $callIndex = 0
    $definitionIndex = 0
    $none = [int]::MaxValue
    while ($true) {
        $tokenAt = if ($tokenIndex -lt $tokens.Count) { $tokens[$tokenIndex].Index } else { $none }
        $callAt = if ($callIndex -lt $calls.Count) { $calls[$callIndex].Index } else { $none }
        $definitionAt = if ($definitionIndex -lt $definitions.Count) { $definitions[$definitionIndex].Index } else { $none }

        if ($callAt -ne $none -and $callAt -le $definitionAt -and $callAt -lt $tokenAt) {
            $call = $calls[$callIndex]
            $callIndex++
            $sequence = $literalAt.Match($text, $call.Index + $call.Length - 1)
            if (-not $sequence.Success) { continue }
            $source = ConvertFrom-LiteralSequence $sequence.Groups[1].Value
            if (-not $source) { continue }
            # Innermost class up to the nearest namespace.
            $classIndex = -1
            for ($k = $open.Count - 1; $k -ge 0; $k--) {
                $kind = $kinds[$open[$k]]
                if ($kind -eq $scopeClass) { $classIndex = $open[$k]; break }
                if ($kind -eq $scopeNamespace) { break }
            }
            $fileCalls.Add([pscustomobject]@{
                Site = New-Site $relative $text $call.Index
                Source = $source
                Explicit = $call.Groups[1].Value
                ClassIndex = $classIndex
                Outside = $open.Count -eq 0 -or $kinds[$open[$open.Count - 1]] -eq $scopeNamespace
                Owner = $owner
            })
        } elseif ($definitionAt -ne $none -and $definitionAt -lt $tokenAt) {
            $match = $definitions[$definitionIndex]
            $definitionIndex++
            if ($openNonNamespace -eq 0 -and $startsDefinition.IsMatch($code, $match.Index + $match.Length - 1)) {
                $owner = $match.Groups[1].Value
            }
        } elseif ($tokenAt -ne $none) {
            $tokenIndex++
            $character = $code[$tokenAt]
            if ($character -eq ';') {
                $segmentStart = $tokenAt + 1
            } elseif ($character -eq '}') {
                if ($open.Count -gt 0) {
                    $closed = $open[$open.Count - 1]
                    $open.RemoveAt($open.Count - 1)
                    $ends[$closed] = $tokenAt
                    if ($kinds[$closed] -ne $scopeNamespace) { $openNonNamespace-- }
                }
                $segmentStart = $tokenAt + 1
            } else {
                $header = $code.Substring($segmentStart, $tokenAt - $segmentStart)
                $kind = $scopeOther
                $name = ''
                $match = $namespaceHeader.Match($header)
                if ($match.Success) {
                    $kind = $scopeNamespace
                    $name = $match.Groups[1].Value
                } elseif ($externHeader.IsMatch($header)) {
                    $kind = $scopeNamespace
                } elseif (-not $enumHeader.IsMatch($header)) {
                    $match = $classHeader.Match($header)
                    if ($match.Success) {
                        $kind = $scopeClass
                        $name = $match.Groups[1].Value
                    }
                }
                $qualified = ''
                if ($kind -eq $scopeClass) {
                    $path = [System.Collections.Generic.List[string]]::new()
                    foreach ($index in $open) {
                        if ($kinds[$index] -ne $scopeOther -and $names[$index]) { $path.Add($names[$index]) }
                    }
                    $path.Add($name)
                    $qualified = $path -join '::'
                }
                $open.Add($kinds.Count)
                $kinds.Add($kind)
                $starts.Add($tokenAt)
                $ends.Add(-1)
                $names.Add($name)
                $qualifiedNames.Add($qualified)
                if ($kind -ne $scopeNamespace) { $openNonNamespace++ }
                $segmentStart = $tokenAt + 1
            }
        } else {
            break
        }
    }

    # A class's tr() context: the Q_DECLARE_TR_FUNCTIONS argument, else its
    # qualified moc name when it has Q_OBJECT/Q_GADGET, else none.
    $trContexts = [string[]]::new($kinds.Count)
    for ($index = 0; $index -lt $kinds.Count; $index++) {
        $trContexts[$index] = ''
        if ($kinds[$index] -ne $scopeClass -or $ends[$index] -lt 0) { continue }
        $declared = $declaredTr.Match($code, $starts[$index])
        $meta = $metaObject.Match($code, $starts[$index])
        if ($declared.Success -and $declared.Index + $declared.Length -le $ends[$index]) {
            $trContexts[$index] = $declared.Groups[1].Value
        } elseif ($meta.Success -and $meta.Index + $meta.Length -le $ends[$index]) {
            $trContexts[$index] = $qualifiedNames[$index]
        }
        if (-not $trContexts[$index]) { continue }
        $contexts = $null
        if (-not $classContexts.TryGetValue($names[$index], [ref]$contexts)) {
            $contexts = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
            $classContexts[$names[$index]] = $contexts
        }
        [void]$contexts.Add($trContexts[$index])
    }

    foreach ($call in $fileCalls) {
        if ($call.Explicit) {
            $classCalls.Add([pscustomobject]@{ Site = $call.Site; Source = $call.Source; ClassName = $call.Explicit })
        } elseif ($call.ClassIndex -ge 0) {
            if ($trContexts[$call.ClassIndex]) {
                Add-Key $trContexts[$call.ClassIndex] $call.Source $call.Site
            } else {
                Add-ContextProblem $call.Site $call.Source "class $($names[$call.ClassIndex]) has no tr() context"
            }
        } elseif ($call.Outside) {
            Add-ContextProblem $call.Site $call.Source 'tr() outside any function'
        } elseif (-not $call.Owner) {
            Add-ContextProblem $call.Site $call.Source 'tr() outside a member function'
        } else {
            $classCalls.Add([pscustomobject]@{ Site = $call.Site; Source = $call.Source; ClassName = $call.Owner })
        }
    }
}

foreach ($call in $classCalls) {
    $contexts = $null
    if ($classContexts.TryGetValue($call.ClassName, [ref]$contexts)) {
        if ($contexts.Count -eq 1) {
            foreach ($context in $contexts) { Add-Key $context $call.Source $call.Site }
        } else {
            Add-ContextProblem $call.Site $call.Source "class name $($call.ClassName) is ambiguous"
        }
    } elseif ($call.ClassName.StartsWith('Q', [System.StringComparison]::Ordinal)) {
        Add-Key $call.ClassName $call.Source $call.Site
    } else {
        Add-ContextProblem $call.Site $call.Source "class $($call.ClassName) has no tr() context"
    }
}

# --- catalog ---------------------------------------------------------------

$catalog = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
$document = [System.Xml.XmlDocument]::new()
$document.PreserveWhitespace = $true
$document.XmlResolver = $null
$document.Load((Join-Path $repoRoot $catalogRelative))
foreach ($contextNode in $document.DocumentElement.SelectNodes('context')) {
    $nameNode = $contextNode.SelectSingleNode('name')
    $context = if ($null -ne $nameNode) { $nameNode.InnerText } else { '' }
    foreach ($message in $contextNode.SelectNodes('message')) {
        $sourceNode = $message.SelectSingleNode('source')
        if ($null -eq $sourceNode -or -not $sourceNode.InnerText) { continue }
        $translationNode = $message.SelectSingleNode('translation')
        $catalog[$context + $keySeparator + $sourceNode.InnerText] = [pscustomobject]@{
            Translation = if ($null -ne $translationNode) { $translationNode.InnerText } else { '' }
            Unfinished = $null -ne $translationNode -and $translationNode.GetAttribute('type') -ceq 'unfinished'
        }
    }
}

$placeholder = [regex]'%(?:L?[1-9][0-9]*|n)'
function Get-Placeholders([string]$Text) {
    $found = [string[]]@($placeholder.Matches($Text) | ForEach-Object { $_.Value })
    [Array]::Sort($found, [System.StringComparer]::Ordinal)
    return ($found -join ' ')
}

function Get-SortedKeys($Keys, [int]$Count) {
    $sorted = [string[]]::new($Count)
    $Keys.CopyTo($sorted, 0)
    [Array]::Sort($sorted, [System.StringComparer]::Ordinal)
    return , $sorted
}

foreach ($key in (Get-SortedKeys $exactKeys $exactKeys.Count)) {
    if (-not $catalog.ContainsKey($key)) {
        Add-Violation 'missing' (Format-Site $keySites[$key]) "no entry $(Format-Key $key) in $catalogRelative"
    }
}
foreach ($key in (Get-SortedKeys $catalog.Keys $catalog.Count)) {
    $entry = $catalog[$key]
    # With unresolved tr() calls their entries would read as stale too.
    if ($contextProblems -eq 0 -and -not $exactKeys.Contains($key)) {
        Add-Violation 'stale' $catalogRelative "entry $(Format-Key $key) is used by no source"
    }
    if ($entry.Unfinished) {
        Add-Violation 'unfinished' $catalogRelative "entry $(Format-Key $key) is marked unfinished"
    }
    if (-not $entry.Translation.Trim()) {
        Add-Violation 'unfinished' $catalogRelative "entry $(Format-Key $key) has an empty translation"
    }
    $sourcePlaceholders = Get-Placeholders $key.Substring($key.IndexOf($keySeparator) + 1)
    if ((Get-Placeholders $entry.Translation) -cne $sourcePlaceholders) {
        Add-Violation 'placeholder' $catalogRelative "entry $(Format-Key $key) translates with other placeholders than [$sourcePlaceholders]"
    }
}

if ($violations.Count -gt 0) {
    $violations | ForEach-Object { Write-Output $_ }
    Write-Output "`nTranslation check FAILED: $($violations.Count) violation(s)."
    exit 1
}
Write-Output "Translation check passed ($($catalog.Count) entries)."
exit 0
