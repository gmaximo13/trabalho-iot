// --- BIBLIOTECAS ---
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// --- CREDENCIAIS ---
#define WIFI_SSID      "tng"
#define WIFI_PASSWORD  "12345678"
#define FIREBASE_HOST  "trabalho-final-iot-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH  "Q4RQoqHIylQEY7ckVWtDQJuUdaI6kq56bds1TdNK"

// --- AJUSTES DO SISTEMA ---
#define TOTAL_VAGAS           3
#define DISTANCIA_OCUPADA_CM  6
#define INTERVALO_LEITURA_MS  2000

// --- CORREÇÃO DE FUSO HORÁRIO ---
const long gmtOffset_sec      = 0;
const int  daylightOffset_sec = 0;

// --- ESTRUTURA DE DADOS PARA CADA VAGA ---
struct Vaga {
  const char* nome;
  uint8_t trig, echo, ledVerde, ledVermelho;
  String  statusAtual, statusAnterior;
  unsigned long tsEntrada;
};

// --- MAPEAMENTO DE PINOS ---
Vaga vagas[TOTAL_VAGAS] = {
  { "vaga1", 12, 13, 27, 16, "livre", "livre", 0 },
  { "vaga2", 14, 32, 17, 33, "livre", "livre", 0 },
  { "vaga3", 23, 21, 15, 22, "livre", "livre", 0 }
};

// --- OBJETOS GLOBAIS ---
FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;
FirebaseData   stream;

WiFiUDP   ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", gmtOffset_sec, 60000);

unsigned long tAnt = 0;
String parkingStatus = "aberto";

// --- DECLARAÇÃO DE FUNÇÕES ---
void streamCallback(FirebaseStream data);
void streamTimeoutCallback(bool timeout);
void fecharEstacionamento();
void abrirEstacionamento();
void gerenciarVagas();
long medirDist(uint8_t t, uint8_t e);
void atualizarLeds(int i);
void handleEntrada(int idx);
void handleSaida(int idx);

// --- FUNÇÃO DE SETUP ---
void setup() {
  Serial.begin(115200);
  Serial.println("\n[SETUP] Iniciando Estacionamento Inteligente (Controle Remoto)...");

  for (int i = 0; i < TOTAL_VAGAS; i++) {
    pinMode(vagas[i].trig, OUTPUT);
    pinMode(vagas[i].echo, INPUT);
    pinMode(vagas[i].ledVerde, OUTPUT);
    pinMode(vagas[i].ledVermelho, OUTPUT);
  }

  abrirEstacionamento();

  Serial.println("\n[WiFi] Conectando...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    tentativas++;
    if (tentativas > 20) {
      Serial.println("\n[WiFi] FALHA AO CONECTAR.");
      Serial.println("Verifique as credenciais, o sinal do WiFi e se a rede é 2.4GHz.");
      Serial.print("Status do WiFi: ");
      Serial.println(WiFi.status());
      while(true) { delay(1000); }
    }
  }

  Serial.println("\n[WiFi] Conectado com sucesso!");
  Serial.print("[WiFi] Endereço IP: ");
  Serial.println(WiFi.localIP());

  Serial.println("[Firebase] Configurando...");
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Serial.println("[Firebase] Configurado.");
 
  if (!Firebase.RTDB.beginStream(&stream, "/estacionamento/config")) {
    Serial.println("[STREAM] Erro ao iniciar stream: " + stream.errorReason());
  }
  Firebase.RTDB.setStreamCallback(&stream, streamCallback, streamTimeoutCallback);

  Serial.println("[NTP] Sincronizando relógio...");
  timeClient.begin();
  while (!timeClient.update()) {
    timeClient.forceUpdate();
    delay(500);
  }
  Serial.println("[NTP] Relógio sincronizado.");
  Serial.println("\n[SETUP] Configuração concluída. Sistema em operação.");
}

// --- FUNÇÃO DE LOOP ---
void loop() {
  if (parkingStatus == "aberto") {
    if (millis() - tAnt >= INTERVALO_LEITURA_MS) {
      tAnt = millis();
      timeClient.update();
      gerenciarVagas();
    }
  }
}

// --- FUNÇÕES DE CONTROLE ---
void fecharEstacionamento() {
  Serial.println("[CONTROLE] Fechando o estacionamento! Todos os LEDs vermelhos ligados.");
  for (int i = 0; i < TOTAL_VAGAS; i++) {
    digitalWrite(vagas[i].ledVerde, LOW);
    digitalWrite(vagas[i].ledVermelho, HIGH);
  }
}

void abrirEstacionamento() {
  Serial.println("[CONTROLE] Abrindo o estacionamento! Retomando operação normal.");
  gerenciarVagas();
}

// --- CALLBACKS DO STREAM ---
void streamCallback(FirebaseStream data) {
  Serial.printf("[STREAM] Evento recebido: %s, %s, %s\n", data.streamPath().c_str(), data.dataPath().c_str(), data.eventType().c_str());
 
  if (data.dataTypeEnum() == fb_esp_rtdb_data_type_json) {
    FirebaseJson *json = data.to<FirebaseJson *>();
    FirebaseJsonData jsonData;
    if (json->get(jsonData, "status")) {
      if (jsonData.type == "string") {
        String newStatus = jsonData.stringValue;
        if (newStatus != parkingStatus) {
          parkingStatus = newStatus;
          if (parkingStatus == "fechado") {
            fecharEstacionamento();
          } else {
            abrirEstacionamento();
          }
        }
      }
    }
  } else if (data.dataTypeEnum() == fb_esp_rtdb_data_type_string) {
      String newStatus = data.stringData();
      if (newStatus != parkingStatus) {
          parkingStatus = newStatus;
          if (parkingStatus == "fechado") {
            fecharEstacionamento();
          } else {
            abrirEstacionamento();
          }
      }
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    Serial.println("[STREAM] Timeout, a conexão pode ter sido perdida. A biblioteca tentará reconectar.");
  }
}

// --- FUNÇÕES PRINCIPAIS ---
void gerenciarVagas() {
  if (parkingStatus != "aberto") return;

  Serial.println("--------------------");
  for (int i = 0; i < TOTAL_VAGAS; i++) {
    long d = medirDist(vagas[i].trig, vagas[i].echo);
    Serial.printf("Vaga %d: Distância = %ld cm\n", (i+1), d);
   
    if (d > 0 && d < DISTANCIA_OCUPADA_CM) {
      vagas[i].statusAtual = "ocupada";
    } else if (d >= DISTANCIA_OCUPADA_CM) {
      vagas[i].statusAtual = "livre";
    } else {
      vagas[i].statusAtual = vagas[i].statusAnterior;
      Serial.printf("Vaga %d: Leitura inválida (-1), mantendo status anterior: %s\n", (i+1), vagas[i].statusAnterior.c_str());
    }

    if (vagas[i].statusAtual != vagas[i].statusAnterior) {
      Serial.printf("Vaga %d: Mudança de status -> %s\n", (i+1), vagas[i].statusAtual.c_str());
      if (vagas[i].statusAtual == "ocupada") {
        handleEntrada(i);
      } else {
        handleSaida(i);
      }
      vagas[i].statusAnterior = vagas[i].statusAtual;
    }
    atualizarLeds(i);
  }
}

void handleEntrada(int idx) {
  unsigned long ts = timeClient.getEpochTime();
  vagas[idx].tsEntrada = ts;

  String path = "/estacionamento/vagas/" + String(vagas[idx].nome);
  FirebaseJson j;
  j.set("status", "ocupada");
  j.set("entrada", String(ts));
  j.set("ultimaAtualizacao/.sv", "timestamp");

  if (!Firebase.RTDB.setJSON(&fbdo, path.c_str(), &j)) {
    Serial.println("[Firebase] ERRO ao enviar entrada: " + fbdo.errorReason());
  }
}

void handleSaida(int idx) {
  unsigned long tsOut = timeClient.getEpochTime();
 
  String path = "/estacionamento/vagas/" + String(vagas[idx].nome);
  FirebaseJson j;
  j.set("status", "livre");
  j.set("ultimaAtualizacao/.sv", "timestamp");
 
  Firebase.RTDB.updateNode(&fbdo, path.c_str(), &j);
  Firebase.RTDB.deleteNode(&fbdo, (path + "/entrada").c_str());

  if (vagas[idx].tsEntrada > 0) {
    unsigned long dur = tsOut - vagas[idx].tsEntrada;
    String hist = "/estacionamento/historico/" + String(vagas[idx].nome);
    FirebaseJson h;
    h.set("entrada", String(vagas[idx].tsEntrada));
    h.set("saida",   String(tsOut));
    h.set("duracaoSeg", String(dur));
    Firebase.RTDB.pushJSON(&fbdo, hist.c_str(), &h);
  }
  vagas[idx].tsEntrada = 0;
}

void atualizarLeds(int i) {
  if (parkingStatus != "aberto") return;
  bool oc = (vagas[i].statusAtual == "ocupada");
  digitalWrite(vagas[i].ledVerde,    oc ? LOW  : HIGH);
  digitalWrite(vagas[i].ledVermelho, oc ? HIGH : LOW);
}

long medirDist(uint8_t t, uint8_t e) {
  digitalWrite(t, LOW);
  delayMicroseconds(2);
  digitalWrite(t, HIGH);
  delayMicroseconds(10);
  digitalWrite(t, LOW);
 
  long dur = pulseIn(e, HIGH, 25000);
 
  return dur > 0 ? dur * 0.0343 / 2 : -1;
}