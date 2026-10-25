<#
Flags raw `!` negation (if (!x), !flag, && !y, while (!z), ...) in Source/.
CLAUDE.md's "always use == or !=" rule has no clang-tidy check that can
see it (`!x.empty()` is already bool, so there's no implicit conversion
for readability-implicit-bool-conversion to catch) — this heuristic grep
fills that gap. It is not a real parser: it strips comments and string/char
literals on a best-effort basis, so an unusual line (e.g. an escaped quote
clang-tidy would handle fine) can still produce a false positive or miss.
#>

param(
    [string]$Path = "Source"
)

$pattern = '(?<!=)!(?!=)'
$exitCode = 0

$files = Get-ChildItem -Path $Path -Recurse -Include *.cpp,*.h,*.hpp,*.cc,*.cxx

foreach ($file in $files)
{
    $lines = Get-Content -Path $file.FullName
    $inBlockComment = $false

    for ($i = 0; $i -lt $lines.Count; $i++)
    {
        $line = $lines[$i]

        if ($inBlockComment)
        {
            $endIdx = $line.IndexOf('*/')
            if ($endIdx -eq -1)
            {
                continue
            }
            $line = $line.Substring($endIdx + 2)
            $inBlockComment = $false
        }

        $line = $line -replace '"(?:[^"\\]|\\.)*"', '""'
        $line = $line -replace "'(?:[^'\\]|\\.)*'", "''"
        $line = $line -replace '/\*.*?\*/', ''

        $startIdx = $line.IndexOf('/*')
        if ($startIdx -ne -1)
        {
            $line = $line.Substring(0, $startIdx)
            $inBlockComment = $true
        }

        $slashIdx = $line.IndexOf('//')
        if ($slashIdx -ne -1)
        {
            $line = $line.Substring(0, $slashIdx)
        }

        if ($line -match $pattern)
        {
            Write-Host "$($file.FullName):$($i + 1): $($line.Trim())"
            $exitCode = 1
        }
    }
}

exit $exitCode
