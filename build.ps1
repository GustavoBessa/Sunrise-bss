<#
.SYNOPSIS
    Builds Sunrise (steam_api64.dll) and optionally publishes it into a game install.

.DESCRIPTION
    Wraps the same MSBuild invocation the CI uses. The project pins a PlatformToolset and a
    Windows SDK that only ship with the newest Visual Studio, so this script reads both values
    out of the vcxproj and substitutes the closest installed alternative when they are absent.
    Nothing is overridden when the pinned versions are present, which keeps a correctly
    provisioned machine byte-for-byte identical to the CI build.

.PARAMETER Configuration
    Release (default) or Debug. Release is what CI ships.

.PARAMETER GamePath
    Directory holding destiny2.exe. Falls back to the SUNRISE_GAME_PATH environment variable.
    Required by -Deploy, -ClearCache and -TailLog.

.PARAMETER Deploy
    Copies the built DLL and PDB over steam_api64.dll in GamePath. The outgoing DLL is backed
    up to steam_api64.dll.bak the first time only, so the original is never lost to a rebuild.

.PARAMETER ClearCache
    Deletes GamePath\Sunrise\cache. Rarely needed: the cache format is version-stamped and a
    mismatched file is refused on read. Use it for a corrupt cache, not for a version change.

.PARAMETER TailLog
    Follows GamePath\Sunrise\logs\sunrise.log after the build. Warns when the file sink is off.

.PARAMETER Clean
    Removes the build directory before compiling.

.EXAMPLE
    .\build.ps1
    Release build only.

.EXAMPLE
    .\build.ps1 -Deploy -TailLog -GamePath 'D:\Destiny 2'
    Build, publish into the install, then follow the log.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [string]$GamePath,

    [switch]$Deploy,
    [switch]$ClearCache,
    [switch]$TailLog,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$RepoRoot = $PSScriptRoot
$SolutionPath = Join-Path $RepoRoot 'Sunrise.sln'
$ProjectPath = Join-Path $RepoRoot 'Sunrise\Sunrise.vcxproj'
$BuildDir = Join-Path $RepoRoot "build\x64\$Configuration"
$GameExecutableName = 'destiny2.exe'
# The game delay-imports this name; the mod ships under it rather than its own.
$ArtifactName = 'steam_api64.dll'
$SymbolName = 'steam_api64.pdb'

function Write-Step([string]$Message) {
    Write-Host ''
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Write-Note([string]$Message) {
    Write-Host "    $Message" -ForegroundColor DarkGray
}

function Write-Warn([string]$Message) {
    Write-Host "    ! $Message" -ForegroundColor Yellow
}

function Stop-WithError([string]$Message) {
    Write-Host ''
    Write-Host "ERRO: $Message" -ForegroundColor Red
    exit 1
}

# --- Toolchain discovery -------------------------------------------------------------------

function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        Stop-WithError 'vswhere.exe nao encontrado. Instale o Visual Studio Build Tools com o workload "Desktop development with C++".'
    }
    # -prerelease so a VS 2026 preview install is accepted; the CI image runs one.
    $path = & $vswhere -latest -prerelease `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath -format value
    if ([string]::IsNullOrWhiteSpace($path)) {
        Stop-WithError 'Nenhuma instalacao do Visual Studio com as ferramentas C++ (VC.Tools.x86.x64) foi encontrada.'
    }
    return ($path | Select-Object -First 1)
}

function Find-MSBuild([string]$VsInstall) {
    $candidate = Join-Path $VsInstall 'MSBuild\Current\Bin\amd64\MSBuild.exe'
    if (Test-Path $candidate) { return $candidate }
    $candidate = Join-Path $VsInstall 'MSBuild\Current\Bin\MSBuild.exe'
    if (Test-Path $candidate) { return $candidate }
    $found = Get-ChildItem (Join-Path $VsInstall 'MSBuild') -Recurse -Filter 'MSBuild.exe' `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $found) {
        Stop-WithError "MSBuild.exe nao encontrado em $VsInstall."
    }
    return $found.FullName
}

function Get-InstalledToolsets([string]$VsInstall) {
    # The toolset props directories are the authority on what MSBuild can actually target.
    # The trailing \* matters: a wildcard anywhere in the path makes Get-ChildItem treat it as a
    # pattern and return the matched directory itself rather than its children.
    $glob = Join-Path $VsInstall 'MSBuild\Microsoft\VC\v*\Platforms\x64\PlatformToolsets\*'
    $dirs = Get-ChildItem $glob -Directory -ErrorAction SilentlyContinue
    if ($null -eq $dirs) { return @() }
    return @($dirs | Where-Object { $_.Name -match '^v\d+$' } |
        Select-Object -ExpandProperty Name -Unique |
        Sort-Object { [int]$_.Substring(1) } -Descending)
}

function Get-InstalledSdks {
    $roots = @(
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots',
        'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots'
    )
    $kitsRoot = $null
    foreach ($key in $roots) {
        $value = (Get-ItemProperty $key -Name KitsRoot10 -ErrorAction SilentlyContinue).KitsRoot10
        if (-not [string]::IsNullOrWhiteSpace($value)) { $kitsRoot = $value; break }
    }
    if ($null -eq $kitsRoot) { return @() }
    $includeDir = Join-Path $kitsRoot 'Include'
    $dirs = Get-ChildItem $includeDir -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'um\windows.h') }
    if ($null -eq $dirs) { return @() }
    return @($dirs | Select-Object -ExpandProperty Name | Sort-Object { [version]$_ } -Descending)
}

function Get-ProjectRequirement([string]$Path, [string]$Element) {
    $text = Get-Content $Path -Raw
    $match = [regex]::Match($text, "<$Element>([^<]+)</$Element>")
    if (-not $match.Success) { return $null }
    return $match.Groups[1].Value.Trim()
}

# --- Project file sanity ------------------------------------------------------------------

function Test-ProjectFileSync {
    # The vcxproj enumerates every translation unit by hand while CMake globs, so a file added
    # to disk builds locally under CMake yet breaks the MSBuild and CI paths until it is listed.
    $listed = ([regex]::Matches((Get-Content $ProjectPath -Raw), '<ClCompile Include=')).Count
    $onDisk = @(Get-ChildItem (Join-Path $RepoRoot 'Sunrise\src') -Recurse -Filter '*.cpp').Count +
              @(Get-ChildItem (Join-Path $RepoRoot 'Sunrise\vendor') -Recurse -Filter '*.cpp').Count
    if ($listed -eq $onDisk) {
        Write-Note "vcxproj em sincronia ($listed arquivos .cpp)."
        return
    }
    Write-Warn "vcxproj lista $listed arquivos .cpp, mas ha $onDisk no disco."
    Write-Warn 'Arquivos novos precisam de uma entrada <ClCompile Include="..." /> no Sunrise.vcxproj.'
}

# --- Build --------------------------------------------------------------------------------

Write-Step 'Localizando toolchain'
$vsInstall = Find-VisualStudio
$msbuild = Find-MSBuild $vsInstall
Write-Note "Visual Studio: $vsInstall"
Write-Note "MSBuild:       $msbuild"

$requiredToolset = Get-ProjectRequirement $ProjectPath 'PlatformToolset'
$requiredSdk = Get-ProjectRequirement $ProjectPath 'WindowsTargetPlatformVersion'
$availableToolsets = Get-InstalledToolsets $vsInstall
$availableSdks = Get-InstalledSdks

if ($availableToolsets.Count -eq 0) {
    Stop-WithError 'Nenhum PlatformToolset encontrado na instalacao do Visual Studio.'
}
if ($availableSdks.Count -eq 0) {
    Stop-WithError 'Nenhum Windows SDK encontrado. Instale o componente Windows 11 SDK.'
}

$msbuildArgs = @(
    $SolutionPath
    '/m'
    "/p:Configuration=$Configuration"
    '/p:Platform=x64'
    '/verbosity:minimal'
    '/nologo'
)

if ($availableToolsets -contains $requiredToolset) {
    Write-Note "Toolset:       $requiredToolset (o exigido pelo projeto)"
} else {
    $substitute = $availableToolsets[0]
    Write-Warn "Toolset $requiredToolset ausente; usando $substitute."
    Write-Warn 'O projeto compila com /W4 e warnings-as-errors, entao um toolset mais antigo pode falhar.'
    $msbuildArgs += "/p:PlatformToolset=$substitute"
}

if ($availableSdks -contains $requiredSdk) {
    Write-Note "SDK:           $requiredSdk (o exigido pelo projeto)"
} else {
    $substitute = $availableSdks[0]
    Write-Warn "Windows SDK $requiredSdk ausente; usando $substitute."
    $msbuildArgs += "/p:WindowsTargetPlatformVersion=$substitute"
}

Write-Step 'Verificando o vcxproj'
Test-ProjectFileSync

if ($Clean) {
    Write-Step 'Limpando build'
    $target = Join-Path $RepoRoot 'build'
    if (Test-Path $target) {
        Remove-Item $target -Recurse -Force
        Write-Note "Removido: $target"
    } else {
        Write-Note 'Nada para remover.'
    }
}

Write-Step "Compilando ($Configuration|x64)"
$started = Get-Date
& $msbuild @msbuildArgs
if ($LASTEXITCODE -ne 0) {
    Stop-WithError "MSBuild terminou com codigo $LASTEXITCODE."
}

$artifactPath = Join-Path $BuildDir $ArtifactName
if (-not (Test-Path $artifactPath)) {
    Stop-WithError "MSBuild reportou sucesso, mas $artifactPath nao existe."
}
$artifact = Get-Item $artifactPath
$elapsed = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
Write-Host ''
Write-Host "    OK  $($artifact.FullName)" -ForegroundColor Green
Write-Note ('versao {0} | {1:N0} KiB | {2}s' -f `
        $artifact.VersionInfo.FileVersion, ($artifact.Length / 1KB), $elapsed)

# --- Target install -----------------------------------------------------------------------

if (-not ($Deploy -or $ClearCache -or $TailLog)) {
    Write-Host ''
    Write-Note 'Use -Deploy para publicar na pasta do jogo, ou -TailLog para acompanhar o log.'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($GamePath)) { $GamePath = $env:SUNRISE_GAME_PATH }
if ([string]::IsNullOrWhiteSpace($GamePath)) {
    Stop-WithError 'Informe -GamePath, ou defina a variavel de ambiente SUNRISE_GAME_PATH.'
}
if (-not (Test-Path (Join-Path $GamePath $GameExecutableName))) {
    Stop-WithError "$GameExecutableName nao encontrado em $GamePath."
}

# Everything the runtime writes lives in one directory beside the DLL.
$RuntimeDir = Join-Path $GamePath 'Sunrise'
$LogPath = Join-Path $RuntimeDir 'logs\sunrise.log'
$SettingsPath = Join-Path $RuntimeDir 'settings.json'

if ($Deploy) {
    Write-Step "Publicando em $GamePath"
    if (Get-Process -Name 'destiny2' -ErrorAction SilentlyContinue) {
        Stop-WithError 'destiny2.exe esta em execucao e mantem a DLL travada. Feche o jogo primeiro.'
    }

    $target = Join-Path $GamePath $ArtifactName
    $backup = "$target.bak"
    if ((Test-Path $target) -and -not (Test-Path $backup)) {
        $outgoing = Get-Item $target
        $label = $outgoing.VersionInfo.FileDescription
        if ([string]::IsNullOrWhiteSpace($label)) {
            $label = 'sem descricao (provavelmente a DLL original da Steam)'
        }
        Copy-Item $target $backup
        Write-Note "Backup unico criado: $ArtifactName.bak  [$label]"
    } elseif (Test-Path $backup) {
        Write-Note "Backup ja existe; preservado: $ArtifactName.bak"
    }

    Copy-Item $artifactPath $target -Force
    Write-Note "Copiado: $ArtifactName"
    $symbolPath = Join-Path $BuildDir $SymbolName
    if (Test-Path $symbolPath) {
        Copy-Item $symbolPath (Join-Path $GamePath $SymbolName) -Force
        Write-Note "Copiado: $SymbolName"
    }
}

if ($ClearCache) {
    Write-Step 'Limpando o cache de conteudo'
    $target = Join-Path $RuntimeDir 'cache'
    if (Test-Path $target) {
        Remove-Item $target -Recurse -Force
        Write-Note "Removido: $target (sera reconstruido no proximo boot)"
    } else {
        Write-Note 'Nenhum cache presente.'
    }
}

if ($TailLog) {
    Write-Step 'Acompanhando o log'
    if (Test-Path $SettingsPath) {
        $settings = Get-Content $SettingsPath -Raw
        if ($settings -match '"file_sink"\s*:\s*false') {
            Write-Warn 'file_sink esta false no settings.json; nada sera escrito em disco.'
            Write-Warn "Edite $SettingsPath e coloque os niveis em debug."
        }
    } else {
        Write-Note 'settings.json ainda nao existe; ele e criado no primeiro boot.'
    }
    Write-Note "Aguardando $LogPath  (Ctrl+C para sair)"
    while (-not (Test-Path $LogPath)) { Start-Sleep -Milliseconds 500 }
    Get-Content $LogPath -Wait -Tail 40
}
