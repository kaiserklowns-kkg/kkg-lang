# Puts `klangc` and `web` on your PATH, for good.
#
#     powershell -ExecutionPolicy Bypass -File tools\install.ps1
#
# `$env:Path += ...` only lasts until the window closes, which is not an install.
# This writes the user's persistent PATH, so a terminal opened tomorrow still has
# the commands. It touches nothing outside your own user environment, adds the
# directory only once however many times it is run, and prints how to undo it.

$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$current = [Environment]::GetEnvironmentVariable('Path', 'User')

if ($null -eq $current) { $current = '' }

$already = $current -split ';' | Where-Object { $_ -eq $tools }
if ($already) {
    Write-Host "Already installed: $tools is on your PATH."
} else {
    $updated = if ($current.TrimEnd(';') -eq '') { $tools } else { $current.TrimEnd(';') + ';' + $tools }
    [Environment]::SetEnvironmentVariable('Path', $updated, 'User')
    Write-Host "Added to your PATH: $tools"
}

# The change reaches new processes, not this one, so make the current session
# work too rather than leaving you to reopen a terminal.
if (($env:Path -split ';') -notcontains $tools) {
    $env:Path = $env:Path.TrimEnd(';') + ';' + $tools
}

Write-Host ''
Write-Host 'You can now run, from anywhere inside the repository:'
Write-Host ''
Write-Host '    web run                 build a page and serve it'
Write-Host '    web build               build it and stop'
Write-Host '    klangc new myapp        a project that already runs'
Write-Host '    klangc run file.kkg     compile and execute a native program'
Write-Host '    klangc build file.kkg   compile to an executable'
Write-Host ''
Write-Host 'Both need Docker running; they use it to supply the C compiler and'
Write-Host 'Emscripten. Nothing else has to be installed.'
Write-Host ''
Write-Host "To undo: remove $tools from Path in the environment variables dialog,"
Write-Host 'or run  [Environment]::SetEnvironmentVariable(''Path'', (([Environment]::GetEnvironmentVariable(''Path'',''User'') -split '';'' | Where-Object { $_ -ne ''' + $tools + ''' }) -join '';''), ''User'')'
