param(
  [string]$BaselineImage = "srvini/rinha-2026-c:edge-id",
  [string]$CandidateImage,
  [int]$Rounds = 4,
  [int]$DiscardFirst = 1,
  [int]$VUs = 100,
  [string]$Duration = "20s",
  [int]$PayloadCount = 128,
  [string]$OutDir = "bench-results/ab"
)

$ErrorActionPreference = "Stop"

if (-not $CandidateImage) {
  throw "Informe -CandidateImage, ex: ./tools/bench-ab.ps1 -CandidateImage srvini/rinha-2026-c:edge-debian"
}

$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$outPath = Join-Path $repo $OutDir
New-Item -ItemType Directory -Force -Path $outPath | Out-Null

function Stop-BenchContainer {
  cmd /c "docker rm -f rinha-bench-lb 2>NUL 1>NUL"
}

function Get-KnownIds {
  param([int]$Count)
  $known = Join-Path $repo "src/known_ids.inc"
  $ids = Select-String -Path $known -Pattern '\{(\d+)u,\s*\d+\}' |
    Select-Object -First $Count |
    ForEach-Object { [uint32]$_.Matches[0].Groups[1].Value }
  if ($ids.Count -eq 0) {
    throw "Nao consegui extrair ids de src/known_ids.inc"
  }
  return @($ids)
}

function New-K6Script {
  param([uint32[]]$Ids, [int]$VUs, [string]$Duration)

  $idsJson = "[" + (($Ids | ForEach-Object { $_.ToString() }) -join ",") + "]"
  $script = @"
import http from 'k6/http';
import { check } from 'k6';

export const options = {
  vus: $VUs,
  duration: '$Duration',
  summaryTrendStats: ['avg', 'min', 'med', 'max', 'p(90)', 'p(95)', 'p(99)'],
  thresholds: {
    http_req_failed: ['rate==0'],
  },
};

const ids = $idsJson;
const headers = { 'Content-Type': 'application/json' };

export default function () {
  const id = ids[(__ITER + (__VU * 17)) % ids.length];
  const body = JSON.stringify({
    id: 'tx-' + id,
    transaction: {
      amount: 384.88,
      installments: 3,
      requested_at: '2026-03-11T20:23:35Z',
    },
    customer: {
      avg_amount: 769.76,
      tx_count_24h: 3,
      known_merchants: ['MERC-009', 'MERC-001'],
    },
    merchant: {
      id: 'MERC-001',
      mcc: '5912',
      avg_amount: 298.95,
    },
    terminal: {
      is_online: false,
      card_present: true,
      km_from_home: 13.7,
    },
    last_transaction: {
      timestamp: '2026-03-11T14:58:35Z',
      km_from_current: 18.8,
    },
  });
  const res = http.post('http://host.docker.internal:9999/fraud-score', body, { headers });
  check(res, { 'status 200': (r) => r.status === 200 });
}
"@

  $path = Join-Path $outPath "bench-k6.js"
  Set-Content -Path $path -Value $script -Encoding ASCII
  return $path
}

function Invoke-Round {
  param(
    [string]$Name,
    [string]$Image,
    [int]$Round,
    [string]$ScriptPath
  )

  Stop-BenchContainer
  docker run -d --name rinha-bench-lb -p 9999:9999 -e PORT=9999 $Image /app/rinha-lb | Out-Null
  Start-Sleep -Seconds 2

  $summary = Join-Path $outPath "$Name-round-$Round.json"
  docker run --rm `
    -e K6_SUMMARY_EXPORT="/out/summary.json" `
    -v "${outPath}:/out" `
    -v "${ScriptPath}:/scripts/bench-k6.js:ro" `
    grafana/k6:latest run /scripts/bench-k6.js | Out-Host

  $exported = Join-Path $outPath "summary.json"
  if (Test-Path $exported) {
    Move-Item -Force $exported $summary
  } else {
    throw "k6 nao gerou summary.json para $Name rodada $Round"
  }

  Stop-BenchContainer
  return $summary
}

function Read-Metrics {
  param([string]$Path)
  $json = Get-Content $Path -Raw | ConvertFrom-Json
  $lat = $json.metrics.http_req_duration
  $failed = $json.metrics.http_req_failed
  [pscustomobject]@{
    file = Split-Path $Path -Leaf
    avg = [double]$lat.avg
    p90 = [double]$lat.'p(90)'
    p95 = [double]$lat.'p(95)'
    p99 = [double]$lat.'p(99)'
    max = [double]$lat.max
    failRate = [double]$failed.value
  }
}

function Summarize {
  param([string]$TestName, [object[]]$Rows)
  $usable = @($Rows | Select-Object -Skip $DiscardFirst)
  if ($usable.Count -eq 0) { $usable = @($Rows) }
  [pscustomobject]@{
    name = $TestName
    roundsUsed = $usable.Count
    avgMs = ($usable | Measure-Object avg -Average).Average
    p95Ms = ($usable | Measure-Object p95 -Average).Average
    p99Ms = ($usable | Measure-Object p99 -Average).Average
    maxMs = ($usable | Measure-Object max -Average).Average
    failRate = ($usable | Measure-Object failRate -Average).Average
  }
}

$ids = Get-KnownIds -Count $PayloadCount
$scriptPath = New-K6Script -Ids $ids -VUs $VUs -Duration $Duration
$pairs = @(
  @{ name = "baseline"; image = $BaselineImage },
  @{ name = "candidate"; image = $CandidateImage }
)

$allRows = @{}
foreach ($pair in $pairs) {
  $rows = @()
  for ($i = 1; $i -le $Rounds; $i++) {
    Write-Host ("`n== {0} round {1}/{2}: {3} ==" -f $pair.name, $i, $Rounds, $pair.image)
    $summary = Invoke-Round -Name $pair.name -Image $pair.image -Round $i -ScriptPath $scriptPath
    $row = Read-Metrics -Path $summary
    $rows += $row
    $row | Format-Table | Out-Host
  }
  $allRows[$pair.name] = $rows
}

$summaryRows = @(
  Summarize -TestName "baseline" -Rows $allRows["baseline"],
  Summarize -TestName "candidate" -Rows $allRows["candidate"]
)

$summaryFile = Join-Path $outPath ("summary-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".json")
$summaryRows | ConvertTo-Json -Depth 5 | Set-Content -Path $summaryFile -Encoding ASCII

Write-Host "`n== A/B summary, first $DiscardFirst round(s) discarded =="
$summaryRows | Format-Table -AutoSize
Write-Host "`nSaved: $summaryFile"

$base = $summaryRows[0]
$cand = $summaryRows[1]
$delta = (($cand.p99Ms - $base.p99Ms) / $base.p99Ms) * 100.0
Write-Host ("`np99 delta candidate vs baseline: {0:N2}%" -f $delta)

if ($cand.failRate -eq 0 -and $cand.p99Ms -lt ($base.p99Ms * 0.95) -and $cand.p95Ms -le $base.p95Ms) {
  Write-Host "RESULT: candidate passou no filtro local."
  exit 0
}

Write-Host "RESULT: candidate NAO passou no filtro local."
exit 2
