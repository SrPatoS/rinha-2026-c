param(
    [string]$OfficialRepo = "D:\job-repos\rinha-official",
    [string]$Image = "srvini/rinha-2026-c:latest",
    [string]$ResultsDir = "bench-results"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$resultsPath = Join-Path $root $ResultsDir
New-Item -ItemType Directory -Force $resultsPath | Out-Null

$matrix = @(
    @{ Name = "balanced-3000"; CandidateLimit = "3000"; ApiCpus = "0.40"; NginxCpus = "0.20"; Workers = "192" },
    @{ Name = "api-heavy-3000"; CandidateLimit = "3000"; ApiCpus = "0.45"; NginxCpus = "0.10"; Workers = "192" },
    @{ Name = "api-max-3000"; CandidateLimit = "3000"; ApiCpus = "0.475"; NginxCpus = "0.05"; Workers = "192" },
    @{ Name = "balanced-1500"; CandidateLimit = "1500"; ApiCpus = "0.40"; NginxCpus = "0.20"; Workers = "192" },
    @{ Name = "api-heavy-1500"; CandidateLimit = "1500"; ApiCpus = "0.45"; NginxCpus = "0.10"; Workers = "192" },
    @{ Name = "balanced-5000"; CandidateLimit = "5000"; ApiCpus = "0.40"; NginxCpus = "0.20"; Workers = "192" },
    @{ Name = "balanced-10000"; CandidateLimit = "10000"; ApiCpus = "0.40"; NginxCpus = "0.20"; Workers = "192" }
)

docker build -t $Image $root

$summary = @()
foreach ($case in $matrix) {
    Write-Host "==> $($case.Name)"
    $env:CANDIDATE_LIMIT = $case.CandidateLimit
    $env:API_CPUS = $case.ApiCpus
    $env:NGINX_CPUS = $case.NginxCpus
    $env:WORKERS = $case.Workers

    docker compose -f (Join-Path $root "docker-compose.yml") --compatibility up -d --force-recreate | Out-Host
    Start-Sleep -Seconds 4

    docker run --rm --network host -e K6_NO_USAGE_REPORT=true -v "${OfficialRepo}\test:/test" -w /test grafana/k6 run --quiet /test/test.js | Out-Host

    $source = Join-Path $OfficialRepo "test\test\results.json"
    $target = Join-Path $resultsPath "$($case.Name).json"
    Copy-Item $source $target -Force

    $json = Get-Content $source -Raw | ConvertFrom-Json
    $summary += [pscustomobject]@{
        name = $case.Name
        candidates = $case.CandidateLimit
        api_cpus = $case.ApiCpus
        nginx_cpus = $case.NginxCpus
        p99 = $json.p99
        final_score = $json.scoring.final_score
        weighted_errors = $json.scoring.weighted_errors_E
        http_errors = $json.scoring.breakdown.http_errors
    }
}

$summary | Sort-Object final_score -Descending | Format-Table -AutoSize
$summary | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $resultsPath "summary.json")
