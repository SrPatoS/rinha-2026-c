# Rinha de Backend 2026 em C

Esqueleto inicial para a Rinha de Backend 2026.

## Rodando localmente

```sh
make run
```

Em outro terminal:

```sh
curl -i http://localhost:9999/ready
```

## WSL/Ubuntu

Se o Ubuntu do WSL ainda nao tiver compilador:

```sh
sudo apt update
sudo apt install -y build-essential
```

Depois, dentro do diretorio do projeto:

```sh
make
./build/convert-references resources/example-references.json resources/example-references.bin
PORT=9999 REFERENCES_PATH=resources/example-references.json ./build/rinha-api
```

Para usar o formato binario rapido:

```sh
PORT=9999 REFERENCES_PATH=resources/example-references.bin ./build/rinha-api
```

## Compilando no Windows com Visual Studio

Abra um "Developer PowerShell for VS" e rode:

```powershell
.\build-msvc.ps1
$env:PORT = "9999"
$env:REFERENCES_PATH = "resources\example-references.json"
.\build\rinha-api.exe
```

Esse build e apenas para desenvolvimento local. O alvo da submissao continua sendo Linux/amd64 via Docker.

## Docker

Para a submissao oficial, publique a imagem declarada no `docker-compose.yml` e mantenha a branch `submission` apenas com os artefatos necessarios para executar o teste.

Durante o desenvolvimento local, voce pode trocar a imagem por `build: .` no compose.

## Proximos passos

- Trocar `resources/example-references.json` pelo dataset oficial pre-processado.
- Criar conversor de `references.json.gz` para formato binario compacto.
- Substituir a busca brute force por indice aproximado ou buckets com refinamento exato.
