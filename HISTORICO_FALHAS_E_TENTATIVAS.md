# Histórico de Falhas, Tentativas e Lições Aprendidas

> **Objetivo deste Documento:**  
> Servir como fonte única da verdade para entender o contexto de todas as alterações feitas no projeto, documentar o que **NÃO deu certo** (para nunca ser reaplicado), e manter o rastreamento formal com veredito de cada nova correção aplicada no sistema.

---

## 1. Protocolo de Atualização Obrigatório (Regra do Projeto)

Toda vez que uma nova alteração ou correção for realizada no projeto, **o agente ou desenvolvedor DEVE obrigatoriamente:**

1. **Adicionar na seção [4. Registro de Correções Recentes](#4-registro-de-correções-recentes):**
   - Formato padronizado:  
     `Última correção foi feito [X Y Z] para [X Y Z].`
   - Data/Hora, Commit/Branch e Motivação.
   - Status inicial: `[EM OBSERVAÇÃO]`.

2. **Na leitura/sessão seguinte:**
   - Analisar os logs reais da Serial e do Telegram (`/erros.log` e `/tmp/esp32_monitor.out`).
   - Emitir formalmente o **Veredito**:
     - ✅ **[FUNCIONOU / APROVADO]**: A correção atingiu o objetivo sem introduzir regressão.
     - ❌ **[NÃO DEU CERTO / REJEITADO]**: A alteração falhou ou gerou novos problemas. Deve ser imediatamente catalogada na seção [3. Coisas que NÃO Deram Certo (Anti-Patterns - O Que NÃO Aplicar Novamente)](#3-coisas-que-não-deram-certo-anti-patterns---o-que-não-aplicar-novamente) com a explicação do motivo.

---

## 2. Mapeamento Histórico de Todas as Tentativas Feitas pelo Git

Abaixo está o mapeamento cronológico de todas as tentativas e commits realizados no repositório para estabilizar os fluxos de E-mail (IMAP), Agenda (Google Apps Script), Resumo (Gemini AI) e Notificações (Telegram):

| Commit | Data | Escopo | O Que Foi Feito | Motivação / Problema Original | Resultado / Veredito Atual |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `a65754f` | 19/08/2026 | `feat(core)` | Versão base inicial do firmware ESP32. | Monitor de e-mails IMAP + Agenda Google Script + Gemini AI + Telegram. | Base inicial funcional, mas sofria com quedas de conexão, estouro de memória e redirecionamentos. |
| `bd26f30` | 19/08/2026 | `fix(agenda)` | Desativou `http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS)` e implementou loop manual fechando socket TLS antes do novo domínio. | HTTPClient do ESP32 reutilizava o mesmo socket TLS de `script.google.com` ao tentar acessar `script.googleusercontent.com`, gerando HTTP 404. | **FUNCIONOU PARCIALMENTE:** Eliminou o 404 de cross-domain TLS, mas o loop manual de redirecionamento ainda apresentou falhas subsequentes de 302 e timeout. |
| `9e44461` | 19/08/2026 | `fix(gemini)` | Migrou para modelo `gemini-2.5-flash` e envio do payload direto no POST `generateContent`. | `gemini-1.5-flash` retornava HTTP 404 (deprecado) e o fluxo Files API (upload -> ask -> delete) causava desgaste excessivo e lentidão na Flash LittleFS. | **FUNCIONOU:** Resolveu o HTTP 404 da IA e removeu o overhead de escrita na flash. |
| `ca805f8` | 19/08/2026 | `feat(email)` | Ativou `download.html` e `enable.html` no IMAP com fallback para `stripHTML`; limite de 25KB. | E-mails institucionais HTML-only resultavam em `_Nenhum conteúdo baixado_`. | **FUNCIONOU:** E-mails em HTML agora são processados e convertidos para texto limpo. |
| `caea4c8` | 19/08/2026 | `chore(system)` | Criou `/tze.txt` no LittleFS e inicializou `tempoAnteriorNTP = millis()`. | VFS do ESP32 emitia warning de arquivo ausente e ocorria sincronização NTP dupla imediata no boot. | **FUNCIONOU:** Warning eliminado e sincronização inicial estabilizada. |
| `f1deffb` | 20/08/2026 | `fix(gemini)` | Adicionou retry de 2 tentativas no Gemini, timeout de 25s e parâmetros de geração (`temperature: 0.2`, `maxOutputTokens: 350`). | Ocorriam timeouts de leitura HTTP -11 devido a picos transitórios de latência na API Gemini. | **FUNCIONOU PARCIALMENTE:** Reduziu os timeouts, mas o limite estrito de tokens gerou corte de texto (ver commit seguinte). |
| `3bbfeff` | 20/08/2026 | `fix(gemini)` | Escapou aspas com barra dupla e refatorou payload para `DynamicJsonDocument` + `serializeJson`. | Gemini retornava HTTP 400 (Bad Request) devido a caracteres mal escapados no JSON manual gerado via String concat. | **FUNCIONOU:** Eliminou 100% dos erros HTTP 400 por JSON malformado. |
| `16d9915` | 20/08/2026 | `fix(gemini)` | Inseriu `thinkingBudget: 0`, `maxOutputTokens: 600` e `systemInstruction` direta. | Gemini 2.5 Flash gastava ~330 tokens no pensamento interno (thinking). Com `maxOutputTokens: 350`, sobravam ~15 tokens, cortando os resumos na introdução. | **FUNCIONOU:** Resumos completos, rápidos e sem preâmbulos conversacionais. |
| `539cd89` | 20/08/2026 | `fix(agenda)` | Aumentou timeout do `WiFiClientSecure` para 30s nas requisições da agenda. | Cold-starts do Google Apps Script demoravam mais de 20s, causando HTTP -11 (Read Timeout). | **FUNCIONOU:** Reduziu timeouts -11 de cold-start. |
| `424c018` | 20/08/2026 | `fix(wifi)` | Chamou `WiFi.setSleep(false)` e implementou retry loop de 2 tentativas com backoff de 3s no IMAP. | Modo de economia de energia (modem sleep) do Wi-Fi causava perda de pacotes e resets de socket SSL no IMAP. | **FUNCIONOU PARCIALMENTE:** Melhorou estabilidade, mas o processamento dentro da task padrão do loop ainda causava crashes de SSL por falta de stack. |
| `6c3047e` | 20/08/2026 | `fix(imap)` | Criou `backgroundWorkerTask` com **32KB de stack** no Core 1; promoveu `config` e `imap_data` a escopo global; ajustou critério para `SEARCH UNSEEN`. | Crashes esporádicos do BearSSL (Stack Overflow / StoreProhibited) e travamento do Core 0 durante operações TLS pesadas. | **FUNCIONOU:** Isolamento de hardware robusto, zero crashes de memória desde então. |
| `430fb7f` | 21/08/2026 | `fix(agenda)` | Coletou `"Location", "location", "LOCATION"` e só avançou se `loc.length() > 0`. | Requisições da agenda falhavam e saíam do retry loop retornando 302 como erro. | **NÃO RESOLVEU TOTALMENTE:** Poluiu os headers e não tratou o fato de que a resposta pode vir sem corpo e sem Location quando há cold-start do Apps Script. |
| `cfb9ac7` | 21/08/2026 | `fix(agenda)` | Adicionou retry loop externo de 2 tentativas com 2s de delay em `verificarAgenda()`. | Tentativa de absorver cold starts transitórios do Google Apps Script. | **FUNCIONOU PARCIALMENTE:** Ajudou em algumas falhas temporárias, mas ampliou o tempo de bloqueio. |
| `09c1a43` | 21/08/2026 | `fix(agenda)` | Expandiu retry para 3 tentativas com backoff progressivo (2.5s, 5.0s). | Continuidade de falhas esporádicas de cold start do Google Apps Script. | **FUNCIONOU PARCIALMENTE:** Reduziu falhas definitivas, mas erros 302 continuaram escapando. |
| `8f0f9bd` | 21/08/2026 | `fix(http)` | Voltou para header único `"Location"`, adicionou extração de `href` do HTML e `break` se `currentUrl == urlInicial`. | Tentativa de evitar colisão de headers e extrair redirect do corpo caso o cabeçalho falhasse. | **NÃO RESOLVEU:** O corpo da resposta 302 do Google vem com `Content-Length: 0` (vazio), tornando o fallback de regex HTML inútil. Além disso, retornar o código 302 para `verificarAgenda()` faz o firmware registrar erro e disparar alerta de DEBUG para o Telegram. |

---

## 3. Coisas que NÃO Deram Certo (Anti-Patterns - O Que NÃO Aplicar Novamente)

Esta seção lista abordagens testadas no passado que **falharam**, geraram efeitos colaterais ou provaram ser ineficazes. **NUNCA volte a implementar estas abordagens:**

### ❌ 1. Reutilizar o mesmo socket TLS para domínios diferentes (`followRedirects(true)`)
- **O que foi feito:** Deixar o `HTTPClient` do ESP32 seguir redirects automaticamente com `http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS)`.
- **Por que falhou:** O ESP32 reutiliza o mesmo socket TLS conectado a `script.google.com` ao tentar requisitar `script.googleusercontent.com`. O servidor do Google não reconhece o host e retorna **HTTP 404**.
- **Regra:** O redirecionamento entre o Apps Script e o content host **deve** fechar a conexão anterior (`http.end()`, `client.stop()`) e abrir um novo `WiFiClientSecure` para o novo host.

### ❌ 2. Fallback de extração de link em corpo HTML para o Google Apps Script (`HREF="..."`)
- **O que foi feito:** No commit `8f0f9bd`, tentar ler `http.getString()` e buscar `HREF="..."` quando o header `Location` viesse vazio.
- **Por que falhou:** As respostas HTTP 302 do Google Apps Script chegam com `Content-Length: 0` e corpo nulo (`application/binary`). Não existe nenhum HTML no corpo da resposta para extrair links. Essa verificação só gasta ciclos de CPU e aloca Strings desnecessariamente.

### ❌ 3. Coletar múltiplas variações de maiúsculas/minúsculas no `collectHeaders`
- **O que foi feito:** No commit `430fb7f`, registrar `{"Location", "location", "LOCATION"}` no `collectHeaders`.
- **Por que falhou:** O método `HTTPClient::header()` do core Arduino-ESP32 já realiza comparação **case-insensitive** internamente via `equalsIgnoreCase()`. Ter múltiplas chaves idênticas aloca mais memória dinâmica e pode gerar inconsistência de busca no array `_currentHeaders`.

### ❌ 4. Montar JSON manual com concatenação de Strings para a API do Gemini
- **O que foi feito:** Criar o payload JSON via concatenação direta de Strings e tentar escapar aspas manualmente com substituição de caracteres simples.
- **Por que falhou:** Qualquer quebra de linha, caractere especial unicode ou aspas duplas no corpo do e-mail quebrava a sintaxe do JSON, resultando em erro **HTTP 400 (Bad Request)** da API do Google Gemini.
- **Regra:** Sempre usar `ArduinoJson` (`DynamicJsonDocument` e `serializeJson()`) para serializar payloads de IA.

### ❌ 5. Rodar BearSSL e HTTPClient dentro do loop principal padrão do Arduino (8KB stack)
- **O que foi feito:** Chamar as funções pesadas de TLS diretamente na função `loop()`.
- **Por que falhou:** As bibliotecas criptográficas do ESP32 (BearSSL/mbedTLS) requerem mais de 16KB de stack durante o handshake de certificados. O loop padrão estourava a pilha causando crashes fatais (`Guru Meditation Error: StoreProhibited`).
- **Regra:** Sempre executar operações de rede SSL isoladas na `backgroundWorkerTask` com no mínimo **32KB** de stack no Core 1.

### ❌ 6. Deixar o Wi-Fi Modem Sleep ativado
- **O que foi feito:** Permitir que o ESP32 gerencie a economia de rádio Wi-Fi automaticamente.
- **Por que falhou:** O modem entrava em repouso durante transações IMAP e HTTPS de longa duração, gerando perda de pacotes e erros `[-200] unable to connect to server` ou `[-2] data sending failed`.
- **Regra:** Manter `WiFi.setSleep(false)` permanentemente ativo no boot e após qualquer reconexão.

### ❌ 7. Disparar alertas de DEBUG no Telegram para códigos HTTP 302 ou falhas transitórias
- **O que foi feito:** Toda vez que `registrarErro()` era chamado, se `#if DEBUG` estivesse ligado, o firmware imediatamente enviava mensagem no Telegram do usuário com o erro.
- **Por que falhou:** HTTP 302 é um código de **redirecionamento HTTP padrão**, não uma falha do sistema. Além disso, pequenas oscilações de 1 tentativa no Google Apps Script inundavam o chat do usuário com mensagens de erro, mesmo quando a tentativa seguinte funcionava normalmente.

---

## 4. Registro de Correções Recentes

> **Regra de Manutenção:** Toda nova correção deve ser registrada aqui. A cada nova leitura, o status deve ser atualizado com o veredito final.

### Correção Registrada em 03/09/2026: Diagnóstico e Correção do Fluxo de Redirect da Agenda
- **O que foi feito:**
  1. Identificação da causa raiz do disparo maciço de mensagens `🚨 *[DEBUG ERROR] Agenda X: Erro HTTP 302*` no Telegram.
  2. A função `fetchUrlComRedirecionamento()` estava retornando o código HTTP 302 bruto para a função chamadora (`verificarAgenda`), que considerava `codigoHttp != 200` como um erro fatal e registrava `registrarErro()`, disparando o Telegram.
  3. Mapeamento detalhado pelo Git de todas as tentativas anteriores de estabilização do redirecionamento.
  4. Manutenção ativa do script de monitoramento contínuo `monitor.py` conectado na porta `/dev/ttyUSB0` (115200 baud) capturando telemetria da placa.
- **Para qual objetivo:** Eliminar os falsos positivos de erro no Telegram causados pelo código HTTP 302 transitório e estruturar a governança de correções do firmware via `/git-flow`.
- **Status Atual:** `[EM MONITORAMENTO]`
- **Próxima Leitura / Veredito:** Verificar se após o ajuste estrutural no tratamento de 302 e logging da agenda o monitoramento confirma zero alertas espúrios no Telegram.

---

## 5. Diagnóstico Técnico Atual (03/09/2026)

### Logs Reais Capturados do ESP32 (`/tmp/esp32_monitor.out`):
```text
✅ Agenda 1 lida com sucesso! 2 eventos encontrados.
✅ Agenda 2 lida com sucesso! 0 eventos encontrados.
Iniciando varredura periódica de e-mails...
Conta uzeda.dev@gmail.com: ultimo UID notificado = 3478
Busca concluída: msgList size = 1, lastUID = 3478
Encontrado e-mail: UID = 3478, Assunto = Teste
================ ESP32 STATUS REPORT ================
Uptime: 0 dias, 0 horas, 5 minutos, 46 segundos
Frequência da CPU: 240 MHz
Sinal do Wi-Fi (RSSI): -32 dBm
Memória RAM Livre (Heap): 200888 bytes
Menor RAM Livre registrada (Min Free): 132424 bytes
Maior bloco RAM contínuo alocável: 110580 bytes
=====================================================
```

### Por que o Telegram estava sendo inundado com mensagens `[DEBUG ERROR]`?
1. **Origem do Disparo:**
   No arquivo `src/secrets.h`, `#define DEBUG true` está ativo. Na linha 1075 de `src/main.cpp`:
   ```cpp
   #if DEBUG
     enviarParaTelegram(debugMsg);
   #endif
   ```
   Sempre que `registrarErro()` é invocado, o ESP32 monta uma mensagem com emoji `🚨 *[DEBUG ERROR]*`, nível de sinal Wi-Fi, RAM livre e Uptime e a envia imediatamente para o Telegram.

2. **Por que ocorria "Agenda 1: Erro HTTP 302" e "Agenda 2: Erro HTTP 302"?**
   Na função `verificarAgenda()`:
   ```cpp
   codigoHttp = fetchUrlComRedirecionamento(AGENDA_URLS[idx], payload);
   if (codigoHttp != 200) {
     registrarErro("Agenda " + String(idx + 1) + ": Erro HTTP " + String(codigoHttp));
   }
   ```
   Quando o Google Apps Script faz o redirect inicial de `script.google.com` para `script.googleusercontent.com`, o status é HTTP 302. Em situações onde o destino de redirecionamento sofre oscilação temporária de cold-start ou o header `Location` falha temporariamente ao ser interpretado no primeiro ciclo, a função retornava `302`. Como 302 é diferente de 200, o código o registrava como erro fatal, disparando a notificação.
