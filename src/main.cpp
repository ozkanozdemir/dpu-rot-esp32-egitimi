// =====================================================================
// DPÜ Robot ve Otomasyon Topluluğu · ESP32 Eğitimi
// ESP32 + Firebase Firestore sohbet odası
//
// Kart aynı anda:
//   1) Telefonun hotspot'una bağlanıp internete çıkar (Firestore'a ulaşmak için).
//   2) Kendi Wi-Fi ağını yayınlar. Bu ağa bağlananlar http://192.168.4.1 adresinden
//      sohbet sayfasını açar. Sayfa dosyaları web/ klasöründen kartın hafızasına yüklenir.
//   3) Tarayıcılarla WebSocket üzerinden konuşur: yazılan mesajları Firestore'a kaydeder,
//      Firestore'daki yeni mesajları birkaç saniyede bir okuyup sohbetteki herkese iletir.
//
// Tarayıcı <-> kart mesajları (JSON, ws://192.168.4.1/ws):
//   tarayıcı -> kart : {"type":"join","name":"Ali"}  {"type":"chat","text":"Merhaba"}  {"type":"leave"}
//   kart -> tarayıcı : info, status, welcome, history, message, users, system, error
// =====================================================================

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

// HOTSPOT_SSID ve HOTSPOT_PASSWORD (GitHub'a gitmez; örnek: hotspot.example.h)
#include "hotspot.h"

// ============================== Ayarlar ==============================

// Kartın yayınlayacağı Wi-Fi ağı. Sınıftaki kartların ağları karışmasın diye adın sonuna
// kartın kimliğinden 4 karakter eklenir (ör. DPU-ROT-ESP32-A1B2).
const char *AP_NAME_PREFIX = "DPU-ROT-ESP32-";
const char *AP_PASSWORD = "12345678";  // en az 8 karakter

// Firebase projesi ve mesajların tutulduğu Firestore koleksiyonu.
const char *FIREBASE_PROJECT_ID = "dpu-rot-esp32-egitimi";
const char *COLLECTION = "messages";

const unsigned long POLL_MS = 4000;         // Firestore'daki yeni mesajlara kaç ms'de bir bakılsın
const unsigned long WIFI_RETRY_MS = 15000;  // hotspot'a bağlı değilsek yeniden deneme aralığı
const size_t HISTORY_SIZE = 50;             // sayfada gösterilen son mesaj sayısı
const size_t MAX_NAME_CHARS = 20;
const size_t MAX_TEXT_CHARS = 300;

// =====================================================================

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Firestore'a HTTPS bağlantısı. Aynı bağlantı tekrar tekrar kullanılır (her istekte yeniden
// şifreli bağlantı kurmak yavaş olurdu).
NetworkClientSecure tls;
HTTPClient http;
String firestoreUrl;  // https://firestore.googleapis.com/v1/projects/<proje>/databases/(default)/documents

String apName;
bool online = false;  // Firestore'a ulaşılabiliyor mu

// Sohbetteki bir mesaj. time: Firestore'un verdiği zaman (ör. 2026-11-07T10:15:30.123456Z)
struct Message {
  String id;
  String user;
  String text;
  String time;
};
std::deque<Message> history;  // son mesajlar (eskiden yeniye)
String lastTime;              // Firestore'dan okunan en yeni mesajın zamanı

// Bu karta bağlı ve odaya katılmış kullanıcılar: WebSocket bağlantı no -> isim
std::map<uint32_t, String> users;

// WebSocket olayları web sunucusunun kendi görevinde gelir. Hepsi loop() içinde sırayla
// işlensin diye önce bu kuyruğa konur.
struct Event {
  enum Kind { Connect, Join, Chat, Leave } kind;
  uint32_t client;
  String text;
};
std::deque<Event> inbox;
std::mutex inboxLock;

unsigned long lastPoll = 0;
unsigned long lastWifiTry = 0;
unsigned long lastCleanup = 0;
bool wasConnected = false;

// ------------------------------ Yardımcılar ------------------------------

// Seri monitöre printf gibi yazar; hem USB hem COM portuna gider.
void logf(const char *format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  Serial.print(buffer);
  Serial0.print(buffer);
}

String toJson(const JsonDocument &doc) {
  String json;
  serializeJson(doc, json);
  return json;
}

// Türkçe harfler birden fazla bayt tuttuğu için uzunluk harf sayısı olarak ölçülür.
size_t charCount(const String &text) {
  size_t count = 0;
  for (size_t i = 0; i < text.length(); i++) {
    if ((text[i] & 0xC0) != 0x80) {
      count++;
    }
  }
  return count;
}

void writeMessage(JsonObject obj, const Message &message) {
  obj["id"] = message.id;
  obj["user"] = message.user;
  obj["text"] = message.text;
  obj["time"] = message.time;
}

// Odaya katılmış herkese gönderir.
void broadcast(const JsonDocument &doc) {
  String json = toJson(doc);
  for (auto &user : users) {
    ws.text(user.first, json);
  }
}

void sendError(uint32_t client, const char *code, const char *text) {
  JsonDocument doc;
  doc["type"] = "error";
  doc["code"] = code;
  doc["text"] = text;
  ws.text(client, toJson(doc));
}

void broadcastSystem(const String &text) {
  JsonDocument doc;
  doc["type"] = "system";
  doc["text"] = text;
  broadcast(doc);
}

void broadcastUsers() {
  JsonDocument doc;
  doc["type"] = "users";
  JsonArray list = doc["users"].to<JsonArray>();
  for (auto &user : users) {
    list.add(user.second);
  }
  broadcast(doc);
}

// Firestore'a ulaşılıp ulaşılamadığı değişince bütün sayfalara bildirir.
void setOnline(bool value) {
  if (value == online) {
    return;
  }
  online = value;
  logf("Firestore %s\n", online ? "baglandi" : "baglantisi yok");
  JsonDocument doc;
  doc["type"] = "status";
  doc["online"] = online;
  ws.textAll(toJson(doc));
}

bool hasMessage(const String &id) {
  for (const Message &message : history) {
    if (message.id == id) {
      return true;
    }
  }
  return false;
}

// Mesajı geçmişe ekler ve sohbetteki herkese gönderir.
void addMessage(const Message &message) {
  history.push_back(message);
  if (history.size() > HISTORY_SIZE) {
    history.pop_front();
  }
  JsonDocument doc;
  doc["type"] = "message";
  writeMessage(doc["message"].to<JsonObject>(), message);
  broadcast(doc);
}

// ------------------------------ Firestore ------------------------------

// Firestore REST API'ye JSON gönderir (POST). Cevap response'a yazılır; başarılıysa true.
bool firestorePost(const char *action, const String &body, String &response) {
  http.begin(tls, firestoreUrl + action);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  response = code > 0 ? http.getString() : http.errorToString(code);
  http.end();
  if (code != 200) {
    logf("Firestore hatasi (HTTP %d): %s\n", code, response.substring(0, 200).c_str());
  }
  return code == 200;
}

// Firestore belgesini Message'a çevirir.
bool readDocument(JsonObject document, Message &message) {
  String name = document["name"] | "";
  message.id = name.substring(name.lastIndexOf('/') + 1);
  message.user = document["fields"]["user"]["stringValue"] | "";
  message.text = document["fields"]["text"]["stringValue"] | "";
  message.time = document["fields"]["createdAt"]["timestampValue"] | "";
  return message.id.length() > 0 && message.time.length() > 0;
}

// Firestore'dan yeni mesajları okur. İlk seferde son HISTORY_SIZE mesajı, sonra yalnızca
// en son okunan mesajdan (lastTime) daha yeni olanları ister.
void pollFirestore() {
  JsonDocument query;
  JsonObject structured = query["structuredQuery"].to<JsonObject>();
  structured["from"][0]["collectionId"] = COLLECTION;
  JsonObject order = structured["orderBy"][0].to<JsonObject>();
  order["field"]["fieldPath"] = "createdAt";
  structured["limit"] = HISTORY_SIZE;
  bool firstLoad = lastTime.isEmpty();
  if (firstLoad) {
    order["direction"] = "DESCENDING";
  } else {
    order["direction"] = "ASCENDING";
    JsonObject filter = structured["where"]["fieldFilter"].to<JsonObject>();
    filter["field"]["fieldPath"] = "createdAt";
    filter["op"] = "GREATER_THAN";
    filter["value"]["timestampValue"] = lastTime;
  }

  String response;
  bool ok = firestorePost(":runQuery", toJson(query), response);
  setOnline(ok);
  if (!ok) {
    return;
  }

  JsonDocument result;
  if (deserializeJson(result, response)) {
    logf("Firestore cevabi okunamadi\n");
    return;
  }
  std::vector<Message> found;
  for (JsonObject item : result.as<JsonArray>()) {
    Message message;
    if (readDocument(item["document"], message)) {
      found.push_back(message);
    }
  }
  if (firstLoad) {
    std::reverse(found.begin(), found.end());  // eskiden yeniye sırala
  }
  for (const Message &message : found) {
    lastTime = message.time;
    if (!hasMessage(message.id)) {
      addMessage(message);
    }
  }
}

// Firestore'da kullanılacak rastgele belge adı (20 harf/rakam).
String randomId() {
  const char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  String id;
  for (int i = 0; i < 20; i++) {
    id += chars[esp_random() % 62];
  }
  return id;
}

// Mesajı Firestore'a kaydeder. Zamanı (createdAt) Firestore'un kendi saati yazar.
bool saveToFirestore(const String &user, const String &text, Message &saved) {
  String id = randomId();
  JsonDocument body;
  JsonObject write = body["writes"][0].to<JsonObject>();
  write["update"]["name"] = String("projects/") + FIREBASE_PROJECT_ID + "/databases/(default)/documents/" +
                            COLLECTION + "/" + id;
  write["update"]["fields"]["user"]["stringValue"] = user;
  write["update"]["fields"]["text"]["stringValue"] = text;
  JsonObject transform = write["updateTransforms"][0].to<JsonObject>();
  transform["fieldPath"] = "createdAt";
  transform["setToServerValue"] = "REQUEST_TIME";
  write["currentDocument"]["exists"] = false;

  String response;
  bool ok = firestorePost(":commit", toJson(body), response);
  setOnline(ok);
  if (!ok) {
    return false;
  }
  JsonDocument result;
  deserializeJson(result, response);
  saved = {id, user, text, result["writeResults"][0]["transformResults"][0]["timestampValue"] | ""};
  return true;
}

// ------------------------------ Sohbet ------------------------------

void sendInfo(uint32_t client) {
  JsonDocument doc;
  doc["type"] = "info";
  doc["ssid"] = apName;
  doc["password"] = AP_PASSWORD;
  doc["project"] = FIREBASE_PROJECT_ID;
  doc["online"] = online;
  ws.text(client, toJson(doc));
}

void handleJoin(uint32_t client, String name) {
  name.trim();
  if (users.count(client)) {
    return;  // zaten odada
  }
  if (name.isEmpty() || charCount(name) > MAX_NAME_CHARS) {
    sendError(client, "invalid_name", "İsim 1-20 karakter olmalı.");
    return;
  }
  for (auto &user : users) {
    if (user.second.equalsIgnoreCase(name)) {
      sendError(client, "name_taken", "Bu isim şu an sohbette kullanılıyor, başka bir isim seçin.");
      return;
    }
  }

  users[client] = name;
  logf("Katildi: %s\n", name.c_str());

  JsonDocument welcome;
  welcome["type"] = "welcome";
  welcome["name"] = name;
  ws.text(client, toJson(welcome));

  JsonDocument past;
  past["type"] = "history";
  JsonArray messages = past["messages"].to<JsonArray>();
  for (const Message &message : history) {
    writeMessage(messages.add<JsonObject>(), message);
  }
  ws.text(client, toJson(past));

  broadcastSystem(name + " katıldı");
  broadcastUsers();
}

void handleChat(uint32_t client, String text) {
  text.trim();
  if (!users.count(client) || text.isEmpty()) {
    return;
  }
  if (charCount(text) > MAX_TEXT_CHARS) {
    sendError(client, "too_long", "Mesaj en fazla 300 karakter olabilir.");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendError(client, "offline", "Kartın interneti yok, mesaj gönderilemedi.");
    return;
  }
  Message saved;
  if (!saveToFirestore(users[client], text, saved)) {
    sendError(client, "save_failed", "Mesaj Firestore'a kaydedilemedi, tekrar deneyin.");
    return;
  }
  logf("Mesaj kaydedildi: %s: %s\n", saved.user.c_str(), saved.text.c_str());
  addMessage(saved);
}

void handleLeave(uint32_t client) {
  auto user = users.find(client);
  if (user == users.end()) {
    return;
  }
  String name = user->second;
  users.erase(user);
  logf("Ayrildi: %s\n", name.c_str());
  broadcastSystem(name + " ayrıldı");
  broadcastUsers();
}

// Web sunucusunun görevinde çalışır: gelen olayı sadece kuyruğa koyar.
void onWsEvent(AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data,
               size_t len) {
  Event event{Event::Connect, client->id(), ""};
  if (type == WS_EVT_CONNECT) {
    client->keepAlivePeriod(10);
  } else if (type == WS_EVT_DISCONNECT) {
    event.kind = Event::Leave;
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT || len > 2048) {
      return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
      return;
    }
    String kind = doc["type"] | "";
    if (kind == "join") {
      event = {Event::Join, client->id(), doc["name"] | ""};
    } else if (kind == "chat") {
      event = {Event::Chat, client->id(), doc["text"] | ""};
    } else if (kind == "leave") {
      event.kind = Event::Leave;
    } else {
      return;
    }
  } else {
    return;
  }
  std::lock_guard<std::mutex> lock(inboxLock);
  inbox.push_back(event);
}

// Kuyruktaki olayları sırayla işler (loop() içinde).
void processInbox() {
  while (true) {
    Event event;
    {
      std::lock_guard<std::mutex> lock(inboxLock);
      if (inbox.empty()) {
        return;
      }
      event = inbox.front();
      inbox.pop_front();
    }
    switch (event.kind) {
      case Event::Connect: sendInfo(event.client); break;
      case Event::Join: handleJoin(event.client, event.text); break;
      case Event::Chat: handleChat(event.client, event.text); break;
      case Event::Leave: handleLeave(event.client); break;
    }
  }
}

// ------------------------------ Başlangıç ------------------------------

void setup() {
  Serial.begin(115200);
  Serial0.begin(115200);
  delay(1000);
  logf("\nDPU ROT ESP32 Egitimi - Firestore sohbet odasi\n");

  if (!LittleFS.begin()) {
    logf("Web sitesi bulunamadi: once 'pio run -t uploadfs' ile web/ klasorunu yukleyin\n");
  }

  // Kart hem telefona bağlansın (istemci) hem kendi ağını yayınlasın (erişim noktası).
  WiFi.mode(WIFI_AP_STA);
  // Ağ adının sonu: kartın kendi MAC adresinin son iki baytı (her kartta farklı).
  uint64_t mac = ESP.getEfuseMac();
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02X%02X", (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  apName = String(AP_NAME_PREFIX) + suffix;
  WiFi.softAP(apName.c_str(), AP_PASSWORD);
  WiFi.begin(HOTSPOT_SSID, HOTSPOT_PASSWORD);
  lastWifiTry = millis();

  // Firestore'un sertifikası karttaki kök sertifika listesiyle doğrulanır.
  tls.useBuiltinCACertBundle();
  http.setReuse(true);
  firestoreUrl = String("https://firestore.googleapis.com/v1/projects/") + FIREBASE_PROJECT_ID +
                 "/databases/(default)/documents";

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html").setCacheControl("no-cache");
  server.onNotFound([](AsyncWebServerRequest *request) { request->send(404, "text/plain", "Bulunamadi"); });
  server.begin();

  logf("Wi-Fi yayini: %s (sifre: %s)  sayfa: http://%s\n", apName.c_str(), AP_PASSWORD,
       WiFi.softAPIP().toString().c_str());
  logf("'%s' hotspot'una baglaniliyor...\n", HOTSPOT_SSID);
}

void loop() {
  processInbox();

  // Kopmuş WebSocket bağlantılarını temizle.
  if (millis() - lastCleanup >= 1000) {
    lastCleanup = millis();
    ws.cleanupClients();
  }

  // Telefona bağlanma / kopma anlarını yakala; bağlı değilsek arada bir yeniden dene.
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wasConnected) {
    wasConnected = connected;
    logf(connected ? "Hotspot'a baglandi, IP: %s\n" : "Hotspot baglantisi yok%s\n",
         connected ? WiFi.localIP().toString().c_str() : "");
    lastPoll = 0;
  }
  if (!connected) {
    setOnline(false);
    if (millis() - lastWifiTry >= WIFI_RETRY_MS) {
      WiFi.disconnect();
      WiFi.begin(HOTSPOT_SSID, HOTSPOT_PASSWORD);
      lastWifiTry = millis();
    }
    return;
  }

  // Firestore'daki yeni mesajlara bak.
  if (lastPoll == 0 || millis() - lastPoll >= POLL_MS) {
    pollFirestore();
    lastPoll = millis();
  }
}
