# One-time: turn this folder into a git repository and push it to GitHub.
# Run from the sketch folder in PowerShell:
#   .\scripts\init_repo.ps1 -Remote https://github.com/<you>/AmoledSmartWatchOS.git
# Safe to run again (skips what's already done).
param([string]$Remote = "")

# Note: no $ErrorActionPreference = "Stop" - in Windows PowerShell 5.1 that turns
# any text git writes to stderr (even harmless notes) into a fatal error.
function Git-Ok { param([string[]]$GitArgs) & git @GitArgs; return ($LASTEXITCODE -eq 0) }

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Host "git is not installed: https://git-scm.com/download/win" -ForegroundColor Red; exit 1
}
if (-not (git config user.name)) {
    Write-Host "Set your git identity first:" -ForegroundColor Yellow
    Write-Host '  git config --global user.name "Your Name"'
    Write-Host '  git config --global user.email "you@example.com"'
    exit 1
}

if (-not (Test-Path .git)) { git init -b main | Out-Null; Write-Host "Repository created." }

git add -A
$pending = git status --porcelain
if ($pending) {
    if (Git-Ok @("commit", "-q", "-m", "AmoledSmartWatchOS firmware + companion app")) { Write-Host "Committed." }
    else { Write-Host "Commit failed (see above)." -ForegroundColor Red; exit 1 }
} else {
    Write-Host "Nothing new to commit."
}

if ($Remote) {
    $remotes = @(git remote)
    if ($remotes -contains "origin") { git remote set-url origin $Remote }
    else { git remote add origin $Remote }
    if (Git-Ok @("push", "-u", "origin", "main")) {
        Write-Host "Pushed. GitHub Actions now builds the firmware and the app on every push." -ForegroundColor Green
    } else {
        Write-Host "Push failed: check the URL, that the GitHub repo exists (empty, no README) and that you're signed in." -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host "Committed locally. To push: .\scripts\init_repo.ps1 -Remote <url>"
}
