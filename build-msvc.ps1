$ErrorActionPreference = "Stop"

if (-not (Test-Path build)) {
    New-Item -ItemType Directory -Path build | Out-Null
}

cl /nologo /TC /O2 /W4 /D_CRT_SECURE_NO_WARNINGS /Fe:build\rinha-api.exe src\main.c ws2_32.lib
