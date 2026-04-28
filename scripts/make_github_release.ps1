# make_github_release.ps1
# Creates a GitHub release for the current extension version and uploads the VSIX.
#
# Prerequisites:
#   - gh CLI installed and authenticated  (winget install GitHub.cli  /  gh auth login)
#   - make_release.ps1 has been run first so Release\ is populated
#   - Git tag matching the version does NOT already exist locally or remotely
#
# Usage:
#   .\scripts\make_github_release.ps1
#   .\scripts\make_github_release.ps1 -Draft          # create as draft first
#   .\scripts\make_github_release.ps1 -Notes "..."    # override release notes

param(
    [switch] $Draft,
    [string] $Notes
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Resolve version from package.json
# ---------------------------------------------------------------------------
$pkgJson = Get-Content "$PSScriptRoot\..\vscode-extension\package.json" -Raw | ConvertFrom-Json
$version = $pkgJson.version
$tag     = "v$version"
$vsix    = "$PSScriptRoot\..\Release\silabs-8051-debug-$version.vsix"

if (-not (Test-Path $vsix)) {
    Write-Error "VSIX not found at '$vsix'. Run make_release.ps1 first."
    exit 1
}

# ---------------------------------------------------------------------------
# Check gh is available
# ---------------------------------------------------------------------------
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    Write-Error "gh CLI not found. Install with: winget install GitHub.cli"
    exit 1
}

# ---------------------------------------------------------------------------
# Determine release notes
# ---------------------------------------------------------------------------
if (-not $Notes) {
    # Pull the top section from CHANGELOG.md (everything between the first and
    # second '## [' heading) as the default release body.
    $changelog = Get-Content "$PSScriptRoot\..\CHANGELOG.md" -Raw
    if ($changelog -match '(?s)(## \[?v?[\d].*?)(\r?\n## \[?v?[\d]|$)') {
        $Notes = $matches[1].Trim()
    } else {
        $Notes = "Release $tag"
    }
}

# ---------------------------------------------------------------------------
# Tag, push, create release
# ---------------------------------------------------------------------------
Write-Host "Version : $version"
Write-Host "Tag     : $tag"
Write-Host "VSIX    : $vsix"
Write-Host ""

# Create and push the tag if it doesn't exist yet
$existingTag = git tag --list $tag
if (-not $existingTag) {
    Write-Host "Creating git tag $tag ..."
    git tag $tag
    Write-Host "Pushing tag ..."
    git push origin $tag
} else {
    Write-Host "Tag $tag already exists — skipping tag creation."
}

# Build the gh command
$ghArgs = @(
    'release', 'create', $tag,
    $vsix,
    '--title', "v$version",
    '--notes', $Notes
)
if ($Draft) { $ghArgs += '--draft' }

Write-Host ""
Write-Host "Creating GitHub release ..."
gh @ghArgs

Write-Host ""
Write-Host "Done. Release $tag published."
Write-Host "Users can install via VSCode: Extensions panel → '...' → 'Install from VSIX...'"
