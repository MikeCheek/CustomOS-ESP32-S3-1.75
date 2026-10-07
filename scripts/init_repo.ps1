# One-time: turn this folder into a git repository and push it to GitHub.
# Run from the sketch folder in PowerShell:  .\scripts\init_repo.ps1 -Remote https://github.com/<you>/AmoledSmartWatchOS.git
param([string]$Remote = "")
$ErrorActionPreference = "Stop"
if (-not (Test-Path .git)) { git init -b main | Out-Null }
git add -A
git status --short | Select-Object -First 40
git commit -m "AmoledSmartWatchOS firmware + companion app"
if ($Remote) {
    git remote remove origin 2>$null
    git remote add origin $Remote
    git push -u origin main
    Write-Host "Pushed. GitHub Actions now builds the firmware and the app on every push."
} else {
    Write-Host "Committed locally. Add a remote with: .\scripts\init_repo.ps1 -Remote <url>"
}
