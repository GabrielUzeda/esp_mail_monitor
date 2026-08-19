/*
  Monitor de e-mail (IMAP) -> filtro blocklist -> notificação no Telegram
  Otimizado para ESP32 - Biblioteca ESP-Mail-Client (Mobizt) v3.x / v4.x

  Bibliotecas necessárias (Gerenciador de Bibliotecas da Arduino IDE):
   - ESP Mail Client (por Mobizt)
   - HTTPClient (já embutida no core do ESP32)
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESP_Mail_Client.h>
#include "secrets.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>

// Protótipos de funções de log
void registrarErro(String motivo);
void imprimirErros();

// ===================== Wi-Fi =====================
const char* WIFI_SSID     = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;

// ===================== LED Status =====================
const int LED_PIN = 2; // LED azul embutido na placa NodeMCU (GPIO 2)

// ===================== Telegram =====================
const char* TELEGRAM_TOKEN   = SECRET_TELEGRAM_TOKEN;
const char* TELEGRAM_CHAT_ID = SECRET_TELEGRAM_CHAT_ID;

// ===================== Contas de e-mail =====================
struct EmailAccount {
  const char* email;
  const char* appPassword;
  int lastUID;
};

EmailAccount contas[] = {
  {"uzeda.dev@gmail.com", "hhwcqwemenvwldxq", 0}
};

#define IMAP_HOST "imap.gmail.com"
#define IMAP_PORT 993

// Blocklist em formato de array de ponteiros (fica armazenado diretamente na flash)
const char* blocklist[] = {"LinkedIn"};
const int QUANTIDADE_PALAVRAS = sizeof(blocklist) / sizeof(blocklist[0]);

// ===================== Controle de tempo =====================
unsigned long tempoAnterior = 0;
const unsigned long intervaloChecagem = 300000; // 5 minutos = 300000ms
const unsigned long intervaloRetryWifi = 5000;  // 5 segundos = 5000ms
unsigned long tempoAnteriorAgenda = 0;
const unsigned long intervaloChecagemAgenda = 120000; // 2 minutos = 120000ms

IMAPSession imap;
Preferences preferences;

// Ponteiro para a conta sendo processada (acessado dentro do callback)
EmailAccount* contaAtual = nullptr;

// Lista de UIDs encontradas na busca para buscar o corpo depois
std::vector<int> uidsParaBuscar;

// ===================== JSON Helper =====================
// Escapa os caracteres críticos para o Payload ser aceito no formato JSON sem erros
String escaparJson(const String &input) {
  String saida;
  saida.reserve(input.length() + 16); 
  for (size_t i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    switch (c) {
      case '"':  saida += "\""; break;
      case '\\': saida += "\\\\"; break;
      case '\n': saida += "\\n";  break;
      case '\r': break; 
      default:   saida += c;
    }
  }
  return saida;
}

// ===================== Gemini API =====================
const char* GEMINI_API_KEY = SECRET_GEMINI_API_KEY;

String askGemini(String textoEmail, bool foiTruncado = false) {
  if (textoEmail.length() == 0) return "_E-mail sem conteúdo legível._";

  // Limita o tamanho do texto enviado ao Gemini para economizar RAM e payload
  if (textoEmail.length() > 6000) {
    textoEmail = textoEmail.substring(0, 6000) + F("\n[...texto truncado]");
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20000);
  HTTPClient http;
  String url = String("https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=") + GEMINI_API_KEY;
  if (!http.begin(client, url)) {
    Serial.println(F("Falha ao iniciar HTTPClient para Gemini"));
    registrarErro("Falha ao conectar no Gemini HTTPS");
    return "_Erro ao conectar ao Gemini_";
  }
  http.setTimeout(20000); // 20s de timeout seguro contra o Watchdog (WDT)

  http.addHeader("Content-Type", "application/json");

  String prompt;
  if (foiTruncado) {
    prompt = "O e-mail a seguir é longo e foi truncado por limitações de memória do microcontrolador. Resuma as informações principais presentes neste trecho inicial de forma breve e clara em português (destacando datas, prazos ou solicitações, se houver):\n\nConteúdo do e-mail:\n" + textoEmail;
  } else {
    prompt = "Resuma o conteúdo deste e-mail de forma breve e objetiva, extraindo apenas as informações mais importantes (como datas, horários, convocações ou ações necessárias). Responda em português de forma clara.\n\nConteúdo do e-mail:\n" + textoEmail;
  }

  String payload;
  payload.reserve(prompt.length() + 128);
  payload += "{\"contents\":[{\"parts\":[{\"text\":\"";
  payload += escaparJson(prompt);
  payload += "\"}]}]}";

  int code = http.POST(payload);
  String result = http.getString();
  http.end();
  client.stop();

  if (code == 200) {
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, result);
    if (!err && doc["candidates"][0]["content"]["parts"][0]["text"]) {
      return doc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
    }
  }
  Serial.printf("Erro Analise Gemini (%d): %s\n", code, result.c_str());
  registrarErro("Gemini API HTTP " + String(code));
  return "_Erro ao resumir o email com IA._";
}

// ===================== Telegram: Envio via HTTPS (POST + JSON) =====================
void enviarParaTelegram(String texto) {
  // O Telegram recusa textos acima de 4096 caracteres
  if (texto.length() > 4000) {
    texto = texto.substring(0, 4000) + F("\n[mensagem truncada para segurança]");
  }

  WiFiClientSecure cliente;
  cliente.setInsecure(); // Ignora a verificação da cadeia do certificado do Telegram (evita bloqueios futuros)

  HTTPClient http;
  String url = String("https://api.telegram.org/bot") + TELEGRAM_TOKEN + "/sendMessage";

  if (!http.begin(cliente, url)) {
    Serial.println(F("Falha ao iniciar requisição para o Telegram"));
    registrarErro("Falha ao iniciar requisição para o Telegram");
    digitalWrite(LED_PIN, HIGH); // Acende o LED de erro
    return;
  }
  http.setTimeout(10000); // 10s de timeout


  http.addHeader("Content-Type", "application/json");

  String payload;
  payload.reserve(texto.length() + 64);
  payload += "{\"chat_id\":\"";
  payload += TELEGRAM_CHAT_ID;
  payload += "\",\"text\":\"";
  payload += escaparJson(texto);
  payload += "\"}";

  int codigo = http.POST(payload);
  if (codigo <= 0) {
    Serial.printf("Erro ao enviar para o Telegram: %s\n", http.errorToString(codigo).c_str());
    registrarErro(String("Erro ao enviar para o Telegram: ") + http.errorToString(codigo));
    digitalWrite(LED_PIN, HIGH); // Acende o LED de erro
  } else if (codigo != 200) {
    Serial.printf("Telegram respondeu com código HTTP %d\n", codigo);
    registrarErro(String("Telegram respondeu com HTTP ") + String(codigo));
    digitalWrite(LED_PIN, HIGH); // Acende o LED de erro
  }
  http.end();
}


// ===================== Filtro Blocklist =====================
bool contemPalavraBloqueada(String assunto, String corpo) {
  assunto.toLowerCase();
  corpo.toLowerCase();

  for (int i = 0; i < QUANTIDADE_PALAVRAS; i++) {
    String termo = String(blocklist[i]);
    termo.toLowerCase();
    if (assunto.indexOf(termo) != -1 || corpo.indexOf(termo) != -1) {
      Serial.printf("Mensagem bloqueada pelo termo: %s\n", blocklist[i]);
      return true;
    }
  }
  return false;
}


// Lê o conteúdo de um arquivo do LittleFS e retorna como String
String lerLittleFS(const char* caminho) {
  if (!caminho || strlen(caminho) == 0) return "";
  // O path começa com '/' quando é um arquivo no LittleFS
  if (caminho[0] != '/') return String(caminho); // Não é um path, é o próprio conteúdo
  File f = LittleFS.open(caminho, "r");
  if (!f) {
    Serial.printf("LittleFS: falha ao abrir '%s'\n", caminho);
    return "";
  }
  String conteudo = f.readString();
  f.close();
  return conteudo;
}

// Strip avançado de tags HTML e CSS
String stripHTML(String html) {
  // 1. Remove blocos inteiros indesejados (CSS, JS, Header)
  String lowerHtml = html;
  lowerHtml.toLowerCase();
  
  const char* tagsRemover[][2] = {
    {"<style", "</style>"},
    {"<script", "</script>"},
    {"<head", "</head>"}
  };

  for (int i = 0; i < 3; i++) {
    int inicio = 0;
    while ((inicio = lowerHtml.indexOf(tagsRemover[i][0], inicio)) != -1) {
      int fim = lowerHtml.indexOf(tagsRemover[i][1], inicio);
      if (fim != -1) {
        int len = (fim + strlen(tagsRemover[i][1])) - inicio;
        html.remove(inicio, len);
        lowerHtml.remove(inicio, len);
      } else {
        html.remove(inicio);
        lowerHtml.remove(inicio);
        break;
      }
    }
  }

  // 1.5. Converte links <a href="...">Texto</a> para Markdown [Texto](URL)
  int p = 0;
  while ((p = lowerHtml.indexOf("<a ", p)) != -1) {
    int closeTag = html.indexOf(">", p);
    if (closeTag == -1) break;
    
    int href = lowerHtml.indexOf("href=\"", p);
    String url = "";
    if (href != -1 && href < closeTag) {
      int endHref = html.indexOf("\"", href + 6);
      if (endHref != -1) url = html.substring(href + 6, endHref);
    } else {
      href = lowerHtml.indexOf("href='", p);
      if (href != -1 && href < closeTag) {
        int endHref = html.indexOf("'", href + 6);
        if (endHref != -1) url = html.substring(href + 6, endHref);
      }
    }
    
    int endA = lowerHtml.indexOf("</a>", closeTag);
    if (endA != -1) {
      String linkText = html.substring(closeTag + 1, endA);
      // Limpa tags filhas (como <img>)
      String cleanText = "";
      bool inside = false;
      for (unsigned int k = 0; k < linkText.length(); k++) {
        char c = linkText.charAt(k);
        if (c == '<') inside = true;
        else if (c == '>') inside = false;
        else if (!inside) cleanText += c;
      }
      cleanText.trim();
      cleanText.replace("[", ""); cleanText.replace("]", "");
      if (cleanText.length() == 0) cleanText = "Link";
      
      if (url.length() > 0 && url.startsWith("http")) {
        String markdown = "[" + cleanText + "](" + url + ")";
        html = html.substring(0, p) + markdown + html.substring(endA + 4);
        lowerHtml = lowerHtml.substring(0, p) + markdown + lowerHtml.substring(endA + 4);
        p += markdown.length();
      } else {
        html = html.substring(0, p) + cleanText + html.substring(endA + 4);
        lowerHtml = lowerHtml.substring(0, p) + cleanText + lowerHtml.substring(endA + 4);
        p += cleanText.length();
      }
    } else {
      p = closeTag + 1;
    }
  }

  // 2. Substitui quebras de linha HTML por reais para formatação
  html.replace("<br>", "\n");
  html.replace("<br/>", "\n");
  html.replace("<br />", "\n");
  html.replace("</p>", "\n\n");
  html.replace("</div>", "\n");

  // 3. Remove as tags restantes
  String saida;
  saida.reserve(html.length());
  bool inTag = false;
  for (unsigned int i = 0; i < html.length(); i++) {
    char c = html.charAt(i);
    if (c == '<') { inTag = true; }
    else if (c == '>') { inTag = false; }
    else if (!inTag) { saida += c; }
  }
  
  // 4. Limpa excesso de espaços
  String finalString = "";
  finalString.reserve(saida.length());
  bool lastSpace = false;
  for (unsigned int i = 0; i < saida.length(); i++) {
    char c = saida.charAt(i);
    if (c == '\r') continue; // Ignora carriage return
    
    if (c == ' ' || c == '\t') {
      if (!lastSpace) finalString += ' ';
      lastSpace = true;
    } else {
      finalString += c;
      lastSpace = (c == '\n'); // Se for nova linha, absorve os espaços seguintes
    }
  }

  // Reduz múltiplas quebras de linha para no máximo duas
  while (finalString.indexOf("\n\n\n") != -1) {
    finalString.replace("\n\n\n", "\n\n");
  }

  // Escapa caracteres que podem quebrar o Markdown do Telegram
  finalString.replace("*", "");
  finalString.replace("_", " ");
  finalString.replace("`", "'");

  // Decodifica as entidades HTML comuns que sobraram no texto
  finalString.replace("&lt;", "<");
  finalString.replace("&gt;", ">");
  finalString.replace("&amp;", "&");
  finalString.replace("&quot;", "\"");
  finalString.replace("&nbsp;", " ");
  finalString.replace("&#39;", "'");

  finalString.trim();
  return finalString;
}

// ===================== Callback do IMAP =====================
// Coleta e agrupa todos os e-mails processados em UMA única requisição para economizar banda
void imapCallback(IMAP_Status status) {
  Serial.println(status.info());
  if (!status.success()) {
    return;
  }

  // Se a operação foi finalizada em modo de cabeçalho (apenas busca)
  if (imap.headerOnly()) {
    uidsParaBuscar.clear();
    IMAP_MSG_List msgList = imap.data();
    Serial.printf("Busca concluída: msgList size = %d, lastUID = %d\n", (int)msgList.msgItems.size(), contaAtual->lastUID);
    for (size_t i = 0; i < msgList.msgItems.size(); i++) {
      IMAP_MSG_Item msg = msgList.msgItems[i];
      Serial.printf("Encontrado e-mail: UID = %d, Assunto = %s\n", msg.UID, msg.subject ? msg.subject : "NULL");
      if (msg.UID > contaAtual->lastUID) {
        uidsParaBuscar.push_back(msg.UID);
        Serial.printf("Adicionado UID %d para busca de corpo\n", msg.UID);
      }
    }
    imap.empty(); // Libera RAM
    return;
  }

  // Se a operação foi finalizada em modo de leitura de corpo (Fetch)
  IMAP_MSG_List msgList = imap.data();
  int maxUID = contaAtual->lastUID;

  for (size_t i = 0; i < msgList.msgItems.size(); i++) {
    IMAP_MSG_Item msg = msgList.msgItems[i];
    Serial.printf("Processando e-mail baixado: UID = %d, Assunto = %s\n", msg.UID, msg.subject ? msg.subject : "NULL");

    if (msg.UID <= contaAtual->lastUID) {
      continue;
    }

    String assunto = (msg.subject && strlen(msg.subject) > 0) ? msg.subject : "(Sem Assunto)";
    String remetente = (msg.from && strlen(msg.from) > 0) ? msg.from : "(Remetente Desconhecido)";

    // Extrai o corpo do e-mail: prioriza texto plano; se não houver (comum em comunicados/newsletters), usa HTML
    String corpoBruto = "";
    if (msg.text.content && strlen(msg.text.content) > 0) {
      corpoBruto = msg.text.content;
    } else if (msg.html.content && strlen(msg.html.content) > 0) {
      corpoBruto = msg.html.content;
    }

    bool emailTruncado = (corpoBruto.length() >= 24000);

    String corpoLimpo = stripHTML(corpoBruto);
    if (corpoLimpo.length() > 6000) {
      emailTruncado = true;
    }

    if (!contemPalavraBloqueada(assunto, corpoLimpo)) {
      String resumoGemini = "_Nenhum conteúdo baixado_";

      if (corpoLimpo.length() > 0) {
        Serial.println("Solicitando resumo ao Gemini...");
        resumoGemini = askGemini(corpoLimpo, emailTruncado);
      }

      String mensagem = "📧 *Novo e-mail (Resumido por IA)!*\n";
      mensagem += "Conta: " + String(contaAtual->email) + "\n";
      mensagem += "De: " + remetente + "\n";
      mensagem += "Assunto: " + assunto + "\n\n";
      mensagem += "🤖 *Resumo Gemini:*\n" + resumoGemini;

      if (emailTruncado) {
        mensagem += "\n\n⚠️ _*Aviso:* O e-mail excedeu os limites de memória do ESP32 e foi truncado. Recomenda-se abrir o link abaixo para visualizar a mensagem completa na íntegra._";
      }

      // Adiciona link direto para abrir no Gmail
      if (msg.ID && strlen(msg.ID) > 0) {
        String msgId = msg.ID;
        msgId.replace("<", "");
        msgId.replace(">", "");
        msgId.trim();
        mensagem += "\n\n🔗 *Abrir no Gmail:*\nhttps://mail.google.com/mail/u/0/#search/rfc822msgid:" + msgId;
      }

      enviarParaTelegram(mensagem);
      delay(1000); // Evita bater no rate limit do Telegram em rajadas de e-mails
    }

    // Atualiza o maior UID encontrado
    if (msg.UID > maxUID) {
      maxUID = msg.UID;
    }
  }

  // Se o maior UID encontrado for maior que o salvo anteriormente, atualiza
  if (maxUID > contaAtual->lastUID) {
    contaAtual->lastUID = maxUID;

    // Salva o novo lastUID na memória flash (NVS)
    int idx = -1;
    for (size_t i = 0; i < (sizeof(contas) / sizeof(contas[0])); i++) {
      if (&contas[i] == contaAtual) {
        idx = i;
        break;
      }
    }
    if (idx != -1) {
      String prefKey = "uid_" + String(idx);
      preferences.putInt(prefKey.c_str(), maxUID);
      Serial.printf("Salvo novo UID (%d) para conta %s\n", maxUID, contaAtual->email);
    }
  }

  // Libera buffer interno do IMAP para preservar a RAM do ESP32
  imap.empty();
}


// ===================== Fluxo Principal do IMAP =====================
bool verificarContaEmail(EmailAccount &conta) {
  contaAtual = &conta;
  bool sucesso = true;

  // Garante fechamento prévio de qualquer sessão pendente e liberação de RAM
  imap.closeSession();
  imap.empty();

  Session_Config config;
  config.server.host_name = IMAP_HOST;
  config.server.port = IMAP_PORT;
  config.login.email = conta.email;
  config.login.password = conta.appPassword;

  // Limpa lista de UIDs para buscar
  uidsParaBuscar.clear();

  IMAP_Data imap_data;

  // FASE 1: BUSCA (Apenas busca as UIDs não lidas)
  imap_data.search.criteria = F("UNSEEN");
  imap_data.limit.search = 5;
  imap_data.limit.msg_size = 512;
  imap_data.fetch.uid.clear();

  // Desativa downloads e buffers de corpo para busca rápida
  imap_data.storage.type = esp_mail_file_storage_type_none;
  imap_data.download.header     = true;
  imap_data.download.text       = false;
  imap_data.download.html       = false;
  imap_data.download.attachment = false;
  imap_data.download.inlineImg  = false;
  imap_data.enable.text = false;
  imap_data.enable.html = false;
  imap_data.enable.recent_sort = true;

  if (!imap.connect(&config, &imap_data)) {
    Serial.printf("Falha de conexão IMAP (Busca): %s (código %d, %s)\n",
                  conta.email, imap.errorCode(), imap.errorReason().c_str());
    String motivo = "IMAP Conexão (" + String(conta.email) + "): [" + String(imap.errorCode()) + "] " + imap.errorReason();
    registrarErro(motivo);
    imap.closeSession();
    imap.empty();
    return false;
  }

  // Define de onde puxar os e-mails e usa "true" para readOnlyMode (impede marcação de lido)
  if (!imap.selectFolder(F("INBOX"), true)) {
    Serial.printf("Falha ao selecionar Caixa de Entrada para corpos: %s\n", conta.email);
    String motivo = "IMAP SelectFolder (" + String(conta.email) + "): [" + String(imap.errorCode()) + "] " + imap.errorReason();
    registrarErro(motivo);
    sucesso = false;
    goto cleanup;
  }

  // Dispara a busca (imapCallback preenche uidsParaBuscar)
  // O segundo parâmetro = false mantém a sessão aberta
  MailClient.readMail(&imap, false); 

  if (imap.errorCode() > 0) {
    Serial.printf("Erro na busca IMAP UNSEEN: %s\n", imap.errorReason().c_str());
    String motivo = "IMAP Search UNSEEN (" + String(conta.email) + "): [" + String(imap.errorCode()) + "] " + imap.errorReason();
    registrarErro(motivo);
    sucesso = false;
    goto cleanup;
  }

  // FASE 2: FETCH (Baixa o corpo diretamente para a RAM para cada UID encontrada)
  if (!uidsParaBuscar.empty()) {
    Serial.printf("Iniciando busca de corpos para %d UIDs...\n", (int)uidsParaBuscar.size());
    std::vector<int> localUids = uidsParaBuscar;
    for (int uid : localUids) {
      Serial.printf("Baixando corpo para UID: %d\n", uid);
      
      // Limpa critério de busca e define UID específica para o Fetch
      imap_data.search.criteria.clear();
      imap_data.fetch.uid = String(uid);

      // Recebe na RAM de forma limitada e super segura (Max 50KB)
      imap_data.storage.type = esp_mail_file_storage_type_none;
      imap_data.fetch.set_seen      = false; // Garante que não marcará como lido no fetch
      imap_data.download.header     = true;
      imap_data.download.text       = true;  // Baixa texto plano
      imap_data.download.html       = true;  // Baixa HTML caso não haja texto plano
      imap_data.download.attachment = false; // Ignora anexos grandes
      imap_data.download.inlineImg  = false;

      // Habilita recebimento do corpo na RAM
      imap_data.enable.text         = true;
      imap_data.enable.html         = true;
      imap_data.limit.msg_size      = 25000; // Limite conservador de 25KB para garantir folga total na RAM!

      // Executa o Fetch (callback processará o corpo na RAM)
      MailClient.readMail(&imap, false);
      
      if (imap.errorCode() > 0) {
        Serial.println("Erro durante o download do corpo do e-mail!");
        String motivo = "IMAP Fetch Corpo UID " + String(uid) + " (" + String(conta.email) + "): [" + String(imap.errorCode()) + "] " + imap.errorReason();
        registrarErro(motivo);
        sucesso = false;
        goto cleanup;
      }
    }
  }

cleanup:
  // Derruba a sessão ativamente e limpa buffers para poupar a Heap Memory
  imap.closeSession();
  imap.empty();

  return sucesso;
}


void imprimirStatusUso() {
  unsigned long ms = millis();
  unsigned long secs = ms / 1000;
  unsigned long mins = secs / 60;
  unsigned long hours = mins / 60;
  unsigned long days = hours / 24;
  secs = secs % 60;
  mins = mins % 60;
  hours = hours % 60;

  Serial.println(F("\n================ ESP32 STATUS REPORT ================"));
  Serial.printf("Uptime: %d dias, %d horas, %d minutos, %d segundos\n", (int)days, (int)hours, (int)mins, (int)secs);
  Serial.printf("Frequência da CPU: %u MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("Sinal do Wi-Fi (RSSI): %d dBm\n", WiFi.RSSI());
  Serial.printf("Memória RAM Livre (Heap): %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Menor RAM Livre registrada (Min Free): %u bytes\n", ESP.getMinFreeHeap());
  Serial.printf("Maior bloco RAM contínuo alocável: %u bytes\n", ESP.getMaxAllocHeap());
  Serial.println(F("=====================================================\n"));
}
// ===================== Agenda (Google Calendar Bridge) =====================
const char* AGENDA_URLS[] = {
  "https://script.google.com/macros/s/AKfycbzR-5Yr3pbeJVjGjmRxr7KTSMnLkNYoTxP-vcEoAgI7DNjmenB9_AiFdYyLWTHgRsia/exec",
  "https://script.google.com/macros/s/AKfycbyslbFyCw84PUgO04_S6Aqa1xVfnvGUolBodNQ0TWBfUNuylJ8kWXB7ZeaIQxjVgcrl/exec"
};
const int QTD_AGENDAS = sizeof(AGENDA_URLS) / sizeof(AGENDA_URLS[0]);

struct AgendaEvento {
  time_t start;
  String title;
  int contaIdx;
};

bool compararEventos(const AgendaEvento &a, const AgendaEvento &b) {
  return a.start < b.start;
}

const int HISTORICO_SIZE = 15; // Histórico de compromissos notificados
struct Notificado {
  time_t startTime = 0;
  String title = "";
  bool notificado1h = false;
  bool notificado30m = false;
};

Notificado historicoNotificacoes[HISTORICO_SIZE];
int indexHistorico = 0;

Notificado* obterHistorico(time_t startTime, String title) {
  for (int i = 0; i < HISTORICO_SIZE; i++) {
    if (historicoNotificacoes[i].startTime == startTime && historicoNotificacoes[i].title == title) {
      return &historicoNotificacoes[i];
    }
  }
  return nullptr;
}

Notificado* adicionarHistorico(time_t startTime, String title) {
  Notificado* n = &historicoNotificacoes[indexHistorico];
  n->startTime = startTime;
  n->title = title;
  n->notificado1h = false;
  n->notificado30m = false;
  indexHistorico = (indexHistorico + 1) % HISTORICO_SIZE;
  return n;
}

int fetchUrlComRedirecionamento(String urlInicial, String &outPayload) {
  outPayload = "";
  String currentUrl = urlInicial;
  int maxRedirects = 5;
  int finalStatus = -1;

  while (maxRedirects > 0 && currentUrl.length() > 0) {
    maxRedirects--;
    int statusAttempt = -1;

    for (int retry = 1; retry <= 3; retry++) {
      WiFiClientSecure client;
      client.setInsecure();
      client.setTimeout(15000);

      HTTPClient http;
      // IMPORTANTE: Desativa o redirecionamento automático interno do HTTPClient.
      // O HTTPClient do ESP32 reutiliza o mesmo socket TLS para domínios diferentes,
      // causando erro 404 ao pular de script.google.com para script.googleusercontent.com.
      http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

      if (http.begin(client, currentUrl)) {
        http.setTimeout(25000);
        http.addHeader("User-Agent", "ESP32-MailMonitor/1.0");
        const char * headerKeys[] = {"Location", "location"};
        http.collectHeaders(headerKeys, 2);

        statusAttempt = http.GET();
        if (statusAttempt > 0) {
          if (statusAttempt == 200) {
            outPayload = http.getString();
            http.end();
            client.stop();
            return 200;
          } else if (statusAttempt == 301 || statusAttempt == 302 || statusAttempt == 303 || statusAttempt == 307 || statusAttempt == 308) {
            String loc = http.header("Location");
            if (loc.length() == 0) loc = http.header("location");
            if (loc.length() == 0) loc = http.getLocation();

            if (loc.length() > 0) {
              if (!loc.startsWith("http://") && !loc.startsWith("https://")) {
                if (loc.startsWith("/")) {
                  if (loc.startsWith("/macros/echo")) {
                    loc = "https://script.googleusercontent.com" + loc;
                  } else {
                    loc = "https://script.google.com" + loc;
                  }
                } else {
                  loc = "https://script.google.com/" + loc;
                }
              }
              currentUrl = loc;
            }
          }
          http.end();
          client.stop();

          // Se for redirect válido (30x), avança para o próximo passo imediatamente
          if (statusAttempt == 301 || statusAttempt == 302 || statusAttempt == 303 || statusAttempt == 307 || statusAttempt == 308) {
            break;
          }
        } else {
          http.end();
          client.stop();
        }
      } else {
        client.stop();
      }

      if (retry < 3) delay(1500 * retry);
    }

    finalStatus = statusAttempt;
    if (finalStatus != 301 && finalStatus != 302 && finalStatus != 303 && finalStatus != 307 && finalStatus != 308) {
      break;
    }
    delay(500);
  }

  return finalStatus;
}

void verificarAgenda() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  bool algumErro = false;

  for (int idx = 0; idx < QTD_AGENDAS; idx++) {
    if (idx > 0) {
      delay(2000);
    }

    String payload;
    int codigoHttp = fetchUrlComRedirecionamento(AGENDA_URLS[idx], payload);

    if (codigoHttp != 200) {
      Serial.printf("Erro na requisição da Agenda %d: %d\n", idx + 1, codigoHttp);
      registrarErro("Agenda " + String(idx + 1) + ": Erro HTTP " + String(codigoHttp));
      algumErro = true;
      continue;
    }

    size_t jsonCap = max((size_t)4096, (size_t)(payload.length() * 2 + 512));
    if (jsonCap > 16384) jsonCap = 16384;
    DynamicJsonDocument doc(jsonCap);
    DeserializationError erro = deserializeJson(doc, payload);
    if (erro) {
      Serial.printf("Falha no parse do JSON da Agenda %d: %s\n", idx + 1, erro.c_str());
      registrarErro("Agenda " + String(idx + 1) + ": Falha JSON: " + String(erro.c_str()));
      algumErro = true;
      continue;
    }

    JsonArray array = doc["events"].as<JsonArray>();
    Serial.printf("✅ Agenda %d lida com sucesso! %d eventos encontrados.\n", idx + 1, array.size());
    time_t nowTime = time(nullptr);

    for (JsonVariant v : array) {
      JsonObject event = v.as<JsonObject>();
      time_t eventStart = event["start"];
      String title = event["title"].as<String>();
      String desc = event.containsKey("desc") ? event["desc"].as<String>() : "";
      String loc = event.containsKey("loc") ? event["loc"].as<String>() : "";

      double diff = difftime(eventStart, nowTime);

      Notificado* hist = obterHistorico(eventStart, title);
      if (!hist) {
        hist = adicionarHistorico(eventStart, title);
      }

      // Notificação de 1 hora (entre 58 e 62 minutos)
      if (diff >= 3480 && diff <= 3720 && !hist->notificado1h) {
        hist->notificado1h = true;
        String txt = "⏰ *Lembrete de Compromisso (Conta " + String(idx + 1) + ")*!\n";
        txt += "O evento \"" + title + "\" começará em *1 hora*.\n";
        if (loc.length() > 0) txt += "📍 Local: " + loc + "\n";
        if (desc.length() > 0) {
          desc.replace("*", ""); desc.replace("_", ""); desc.replace("`", ""); desc.replace("[", ""); desc.replace("]", "");
          txt += "📝 Detalhes:\n" + desc + "\n";
        }
        enviarParaTelegram(txt);
        Serial.printf("Notificação de 1h enviada para: %s (Conta %d)\n", title.c_str(), idx + 1);
      }

      // Notificação de 30 minutos (entre 28 e 32 minutos)
      if (diff >= 1680 && diff <= 1920 && !hist->notificado30m) {
        hist->notificado30m = true;
        String txt = "⏰ *Lembrete de Compromisso (Conta " + String(idx + 1) + ")*!\n";
        txt += "O evento \"" + title + "\" começará em *30 minutos*.\n";
        if (loc.length() > 0) txt += "📍 Local: " + loc + "\n";
        if (desc.length() > 0) {
          desc.replace("*", ""); desc.replace("_", ""); desc.replace("`", ""); desc.replace("[", ""); desc.replace("]", "");
          txt += "📝 Detalhes:\n" + desc + "\n";
        }
        enviarParaTelegram(txt);
        Serial.printf("Notificação de 30m enviada para: %s (Conta %d)\n", title.c_str(), idx + 1);
      }
    }
  }

  if (algumErro) {
    digitalWrite(LED_PIN, HIGH);
  }
}

void enviarResumoDiario() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  std::vector<AgendaEvento> todosEventos;
  std::vector<String> todasTarefas;
  bool algumErro = false;

  for (int idx = 0; idx < QTD_AGENDAS; idx++) {
    if (idx > 0) {
      delay(2000);
    }

    String payload;
    int codigoHttp = fetchUrlComRedirecionamento(AGENDA_URLS[idx], payload);

    if (codigoHttp != 200) {
      Serial.printf("Erro na requisição do Resumo Diário %d: %d\n", idx + 1, codigoHttp);
      registrarErro("Resumo Agenda " + String(idx + 1) + ": Erro HTTP " + String(codigoHttp));
      algumErro = true;
      continue;
    }


    size_t jsonCap = max((size_t)4096, (size_t)(payload.length() * 2 + 512));
    if (jsonCap > 16384) jsonCap = 16384;
    DynamicJsonDocument doc(jsonCap);
    DeserializationError erro = deserializeJson(doc, payload);
    if (erro) {
      Serial.printf("Falha no parse do JSON do Resumo %d: %s\n", idx + 1, erro.c_str());
      registrarErro("Resumo Agenda " + String(idx + 1) + ": Falha JSON: " + String(erro.c_str()));
      algumErro = true;
      continue;
    }

    // 1. Coleta os eventos
    JsonArray events = doc["events"].as<JsonArray>();
    Serial.printf("✅ Resumo da Agenda %d: %d eventos.\n", idx + 1, events.size());
    for (JsonVariant v : events) {
      JsonObject event = v.as<JsonObject>();
      time_t eventStart = event["start"];
      String title = event["title"].as<String>();
      todosEventos.push_back({eventStart, title, idx + 1});
    }

    // 2. Coleta as tarefas
    if (doc.containsKey("tasks") && doc["tasks"].is<JsonArray>()) {
      JsonArray tasks = doc["tasks"].as<JsonArray>();
      Serial.printf("✅ Resumo das Tarefas %d: %d tarefas.\n", idx + 1, tasks.size());
      for (JsonVariant v : tasks) {
        String taskTitle = "";
        if (v.is<JsonObject>()) {
          JsonObject obj = v.as<JsonObject>();
          if (obj.containsKey("title")) taskTitle = obj["title"].as<String>();
          else if (obj.containsKey("name")) taskTitle = obj["name"].as<String>();
          else if (obj.containsKey("summary")) taskTitle = obj["summary"].as<String>();
          else if (obj.containsKey("text")) taskTitle = obj["text"].as<String>();
        } else {
          taskTitle = v.as<String>();
        }

        taskTitle.trim();
        if (taskTitle.length() > 0) {
          todasTarefas.push_back(taskTitle + " (Conta " + String(idx + 1) + ")");
        }
      }
    } else {
      Serial.printf("ℹ️ Resumo das Tarefas %d: nenhuma lista de tarefas encontrada no JSON.\n", idx + 1);
    }

  }

  // Ordena todos os eventos por horário
  std::sort(todosEventos.begin(), todosEventos.end(), compararEventos);

  String msg = "🌅 *BOM DIA! RESUMO DO DIA* 🌅\n\n";

  // 1. Processa Agenda (compromissos)
  msg += "📅 *Compromissos para hoje:*\n";
  if (todosEventos.empty()) {
    msg += "_Nenhum compromisso agendado._\n";
  } else {
    for (const auto &ev : todosEventos) {
      time_t eventStart = ev.start;
      struct tm *timeinfo = localtime(&eventStart);
      char horaBuf[6];
      strftime(horaBuf, sizeof(horaBuf), "%H:%M", timeinfo);

      msg += "- *" + String(horaBuf) + "* - " + ev.title + " (Conta " + String(ev.contaIdx) + ")\n";
    }
  }

  msg += "\n";

  // 2. Processa Tarefas (Google Tasks)
  msg += "📝 *Tarefas para hoje:*\n";
  if (todasTarefas.empty()) {
    msg += "_Nenhuma tarefa pendente._\n";
  } else {
    for (const auto &taskTitle : todasTarefas) {
      msg += "- [ ] " + taskTitle + "\n";
    }
  }

  enviarParaTelegram(msg);
  Serial.println(F("Resumo diário enviado com sucesso!"));

  if (algumErro) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    // Não apaga o LED aqui — deixa a lógica central de e-mail decidir
  }
}


void verificarTodasContas() {
  bool algumErro = false;
  for (size_t i = 0; i < (sizeof(contas) / sizeof(contas[0])); i++) {
    String prefKey = "uid_" + String(i);
    contas[i].lastUID = preferences.getInt(prefKey.c_str(), 0);
    Serial.printf("Conta %s: ultimo UID notificado = %d\n", contas[i].email, contas[i].lastUID);
    if (!verificarContaEmail(contas[i])) {
      algumErro = true;
    }
  }

  if (algumErro) {
    digitalWrite(LED_PIN, HIGH); // Acende o LED de erro se alguma conta falhou
  } else {
    digitalWrite(LED_PIN, LOW);  // Apaga o LED se tudo ocorreu com sucesso
  }
  
  imprimirStatusUso();
}


// ===================== Setup e Loop =====================

// ===================== LOG DE ERROS =====================
void registrarErro(String motivo) {
  char buf[64];
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  if (timeinfo.tm_year > 100) { // garante que o NTP já sincronizou
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  } else {
    sprintf(buf, "%lu ms", millis());
  }

  // Rotação segura do arquivo de erros no LittleFS (evita desgaste e estouro da Flash)
  if (LittleFS.exists("/erros.log")) {
    File checkSize = LittleFS.open("/erros.log", "r");
    if (checkSize) {
      size_t fSize = checkSize.size();
      checkSize.close();
      if (fSize > 32768) { // Acima de 32KB, renova o log
        LittleFS.remove("/erros.log");
      }
    }
  }

  File logFile = LittleFS.open("/erros.log", "a");
  if (logFile) {
    logFile.printf("[%s] %s\n", buf, motivo.c_str());
    logFile.close();
  }

#if DEBUG
  static bool enviandoErroTelegram = false;
  if (!enviandoErroTelegram) {
    enviandoErroTelegram = true;
    String debugMsg = "🚨 *[DEBUG ERROR]*\n";
    debugMsg += "⏱️ *Horário:* " + String(buf) + "\n";
    debugMsg += "📍 *Detalhe:* " + motivo + "\n";
    if (WiFi.status() == WL_CONNECTED) {
      debugMsg += "📶 *Wi-Fi:* " + String(WiFi.RSSI()) + " dBm (" + WiFi.localIP().toString() + ")\n";
    } else {
      debugMsg += "📶 *Wi-Fi:* Desconectado\n";
    }
    debugMsg += "🧠 *RAM Livre:* " + String(ESP.getFreeHeap()) + " bytes (Mín: " + String(ESP.getMinFreeHeap()) + ")\n";
    debugMsg += "⏱️ *Uptime:* " + String(millis() / 1000) + "s";
    enviarParaTelegram(debugMsg);
    enviandoErroTelegram = false;
  }
#endif
}



void imprimirErros() {
  if (LittleFS.exists("/erros.log")) {
    Serial.println("\n=== LOG DE ERROS ANTERIORES ===");
    File logFile = LittleFS.open("/erros.log", "r");
    while(logFile.available()){
      Serial.write(logFile.read());
    }
    logFile.close();
    Serial.println("===============================\n");
    // Opcional: apagar após ler para não crescer infinitamente
    // LittleFS.remove("/erros.log"); 
  } else {
    Serial.println("\nNenhum erro registrado no log.");
  }
}

void setup() {
  Serial.begin(115200);

  // Configura o pino do LED de status
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); // Começa desligado

  // Inicializa o LittleFS e formata caso esteja corrompido/desformatado
  if (!LittleFS.begin(true)) {
    Serial.println(F("Erro ao iniciar/formatar LittleFS"));
  } else {
    if (!LittleFS.exists("/tze.txt")) {
      File tze = LittleFS.open("/tze.txt", "w");
      if (tze) {
        tze.print("<-03>3");
        tze.close();
      }
    }
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando ao Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(F("\nWi-Fi conectado com sucesso!"));

  // Configura a hora via NTP usando a API nativa do ESP32 (Fuso -3 para Horário de Brasília)
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Sincronizando horário via NTP...");
  time_t now = time(nullptr);
  while (now < 24 * 3600) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println(F("\nHorário sincronizado com sucesso!"));

  // Inicializa o Preferences
  preferences.begin("mail_monitor", false);

  // Atribui a função que a biblioteca executará sempre que encontrar novos e-mails
  imap.callback(imapCallback);

  // Execução inicial ao ligar
  verificarTodasContas();
  delay(2000); // Pausa de 2s para liberar socket SSL do IMAP antes de iniciar a Agenda
  verificarAgenda();
  tempoAnterior = millis();
  tempoAnteriorAgenda = millis();
  imprimirErros();
}

void loop() {
  static bool wifiWasConnected = true;

  // Lógica auto-recuperável: Reconecta se a internet cair em algum momento do dia
  if (WiFi.status() != WL_CONNECTED) {
    registrarErro("Acende o LED de erro por falha de Wi-Fi");
    digitalWrite(LED_PIN, HIGH); // Acende o LED de erro por falha de Wi-Fi
    wifiWasConnected = false;
    Serial.println(F("Wi-Fi desconectado, tentando reconectar..."));
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    delay(intervaloRetryWifi);
    return;
  }

  if (!wifiWasConnected) {
    wifiWasConnected = true;
    digitalWrite(LED_PIN, LOW); // Apaga o LED pois o Wi-Fi reconectou
    Serial.println(F("Wi-Fi reconectado com sucesso!"));
  }

  unsigned long tempoAtual = millis();
  
  if (tempoAtual - tempoAnterior >= intervaloChecagem) {
    tempoAnterior = tempoAtual;
    Serial.println(F("Iniciando varredura periódica de e-mails..."));
    verificarTodasContas();
    delay(2000); // Pausa de 2s para liberar socket SSL do IMAP
  }

  if (tempoAtual - tempoAnteriorAgenda >= intervaloChecagemAgenda) {
    tempoAnteriorAgenda = tempoAtual;
    Serial.println(F("Iniciando varredura periódica da agenda..."));
    verificarAgenda();
  }

  // Ressincronização diária do NTP para prevenir desvios (drift) no relógio
  static unsigned long tempoAnteriorNTP = millis();
  if (tempoAtual - tempoAnteriorNTP >= 86400000) {
    tempoAnteriorNTP = tempoAtual;
    configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println(F("Ressincronizando horário NTP..."));
  }

  // Lógica do Resumo Diário às 06:30 da manhã (GMT-3)
  static int ultimoDiaResumo = -1;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    if (timeinfo.tm_hour == 6 && timeinfo.tm_min == 30 && timeinfo.tm_mday != ultimoDiaResumo) {
      ultimoDiaResumo = timeinfo.tm_mday;
      Serial.println(F("Disparando resumo diário (06:30)..."));
      enviarResumoDiario();
    }
  }
}

