# Rinha de Backend 2026 em C — Detecção de Fraude com Busca Vetorial

Este repositório contém a solução desenvolvida em **C** para a **Rinha de Backend 2026 – Fraud Detection**.

## Arquitetura e Stack

A solução é projetada para rodar sob as rígidas restrições de **1.0 CPU** e **350 MB de RAM** do desafio.

- **API:** Desenvolvida em **C** puro, utilizando chamadas de sistema eficientes (concorrência nativa multi-thread com `pthread` e tratamento de sockets de alta performance).
- **Consumo de Memória:** ~115 MB de RAM por réplica, mantendo e indexando em memória todos os **3.000.000 de vetores de 14 dimensões** do dataset de referência.
- **Indexador Espacial:** Algoritmo implementado em memória utilizando **131.072 buckets** discretos indexados por 7 propriedades das transações, oferecendo buscas com latência extremamente baixa.
- **Load Balancer:** Nginx distribuindo o tráfego em round-robin entre duas réplicas da API.

## Como Executar

A solução está pronta para ser executada e testada.

### Pré-requisitos
- Docker e Docker Compose instalados.

### Execução
Para iniciar toda a infraestrutura:
```sh
docker compose up -d
```

A solução responderá na porta **9999**, expondo os endpoints obrigatórios:
- `GET /ready` (Prontidão da API)
- `POST /fraud-score` (Score de fraude)
