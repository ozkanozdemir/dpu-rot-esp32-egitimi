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

// Arduino'nun temel fonksiyonlarını (Serial, delay, millis, String vb.) projeye ekler.
#include <Arduino.h>
// JSON metni oluşturmak ve okumak için ArduinoJson kütüphanesini ekler (lib/ArduinoJson).
#include <ArduinoJson.h>
// Web sunucusu ve WebSocket için ESPAsyncWebServer kütüphanesini ekler (lib/ESPAsyncWebServer).
#include <ESPAsyncWebServer.h>
// İnternetteki sunuculara HTTP/HTTPS isteği (GET/POST) göndermeyi sağlar; Firestore için kullanılır.
#include <HTTPClient.h>
// Kartın flash hafızasındaki dosya sistemi; web sitesi dosyaları buradan okunur.
#include <LittleFS.h>
// Şifreli (TLS/HTTPS) bağlantı kurar ve sunucunun sertifikasını doğrular.
#include <NetworkClientSecure.h>
// Kartın Wi-Fi'sini yönetir: hem telefona bağlanma hem kendi ağını yayınlama.
#include <WiFi.h>

// std::reverse gibi hazır algoritmalar için (listeyi ters çevirmek).
#include <algorithm>
// İki ucundan ekleme/çıkarma yapılabilen liste (mesaj geçmişi ve olay kuyruğu için).
#include <deque>
// Anahtar -> değer eşleşmesi tutan tablo (bağlantı numarası -> kullanıcı adı).
#include <map>
// Kilit (mutex): iki görev aynı veriye aynı anda dokunmasın diye.
#include <mutex>
// Boyutu değişebilen dizi (Firestore'dan gelen mesajları geçici olarak tutmak için).
#include <vector>

// Telefonun hotspot adını (HOTSPOT_SSID) ve şifresini (HOTSPOT_PASSWORD) içeren dosyayı ekler.
// Bu dosya GitHub'a gönderilmez; nasıl doldurulacağı hotspot.example.h dosyasında yazar.
#include "hotspot.h"

// ============================== Ayarlar ==============================

// Kartın yayınlayacağı Wi-Fi ağının adının başı. Sınıftaki kartların ağları karışmasın diye
// adın sonuna kartın kimliğinden 4 karakter eklenir (ör. DPU-ROT-ESP32-A1B2).
const char *AP_NAME_PREFIX = "DPU-ROT-ESP32-";
// Kartın yayınladığı Wi-Fi ağının şifresi; en az 8 karakter olmak zorundadır.
const char *AP_PASSWORD = "12345678";

// Mesajların kaydedildiği Firebase projesinin kimliği (Firebase konsolundaki "Project ID").
const char *FIREBASE_PROJECT_ID = "dpu-rot-esp32-egitimi";
// Mesajların tutulduğu Firestore koleksiyonunun adı (bir klasör gibi düşünülebilir).
const char *COLLECTION = "messages";

// Firestore'daki yeni mesajlara kaç milisaniyede bir bakılacağı (4000 ms = 4 saniye).
const unsigned long POLL_MS = 4000;
// Telefonun hotspot'una bağlı değilsek kaç milisaniyede bir yeniden deneneceği (15 saniye).
const unsigned long WIFI_RETRY_MS = 15000;
// Sayfada gösterilen ve kartın hafızasında tutulan son mesaj sayısı.
const size_t HISTORY_SIZE = 50;
// Bir kullanıcı adının en fazla kaç harf olabileceği.
const size_t MAX_NAME_CHARS = 20;
// Bir mesajın en fazla kaç harf olabileceği.
const size_t MAX_TEXT_CHARS = 300;

// =====================================================================

// 80 numaralı portta (tarayıcıların varsayılan HTTP portu) çalışan web sunucusunu oluşturur.
AsyncWebServer server(80);
// "/ws" adresinde çalışan WebSocket'i oluşturur; tarayıcılar canlı mesajlaşmak için buraya bağlanır.
AsyncWebSocket ws("/ws");

// Firestore'a şifreli (HTTPS) bağlanmak için kullanılan bağlantı nesnesi.
// Aynı bağlantı tekrar tekrar kullanılır; her istekte yeniden şifreli bağlantı kurmak yavaş olurdu.
NetworkClientSecure tls;
// Firestore'a HTTP istekleri (POST) gönderen nesne; yukarıdaki şifreli bağlantıyı kullanır.
HTTPClient http;
// Firestore REST API'nin ana adresi; setup() içinde proje kimliğiyle doldurulur.
// Örnek: https://firestore.googleapis.com/v1/projects/<proje>/databases/(default)/documents
String firestoreUrl;

// Kartın yayınladığı Wi-Fi ağının tam adı (ör. DPU-ROT-ESP32-40D4); setup() içinde oluşturulur.
String apName;
// Firestore'a şu an ulaşılabiliyor mu? Sayfadaki "Firestore bağlı" göstergesi bunu kullanır.
bool online = false;

// Sohbetteki bir mesajın bilgilerini bir arada tutan yapı.
struct Message {
  // Firestore'daki belge adı; her mesajın benzersiz kimliği.
  String id;
  // Mesajı yazan kullanıcının adı.
  String user;
  // Mesajın metni.
  String text;
  // Firestore'un verdiği kayıt zamanı (ör. 2026-11-07T10:15:30.123456Z).
  String time;
};
// Son mesajların listesi; eskiden yeniye sıralı tutulur.
std::deque<Message> history;
// Firestore'dan okunan en yeni mesajın zamanı; bir sonraki sorguda "bundan yenileri" demek için.
String lastTime;

// Bu karta bağlı ve sohbete katılmış kullanıcılar: WebSocket bağlantı numarası -> kullanıcı adı.
std::map<uint32_t, String> users;

// WebSocket'ten gelen bir olayı (bağlanma, katılma, mesaj, ayrılma) tutan yapı.
// Bu olaylar web sunucusunun kendi görevinde gelir; hepsi loop() içinde sırayla işlensin diye
// önce bir kuyruğa konur.
struct Event {
  // Olayın türü: bağlandı, katılmak istiyor, mesaj yazdı, ayrıldı.
  enum Kind { Connect, Join, Chat, Leave } kind;
  // Olayı gönderen tarayıcının WebSocket bağlantı numarası.
  uint32_t client;
  // Olayla gelen metin: katılırken isim, mesaj yazarken mesajın kendisi.
  String text;
};
// Henüz işlenmemiş olayların bekletildiği kuyruk.
std::deque<Event> inbox;
// Kuyruğa aynı anda iki görevin dokunmasını engelleyen kilit.
std::mutex inboxLock;

// Firestore'a en son ne zaman bakıldığı (millis() cinsinden; 0 = hemen bak).
unsigned long lastPoll = 0;
// Telefonun hotspot'una en son ne zaman bağlanmaya çalışıldığı.
unsigned long lastWifiTry = 0;
// Kopmuş WebSocket bağlantılarının en son ne zaman temizlendiği.
unsigned long lastCleanup = 0;
// Bir önceki turda hotspot'a bağlı mıydık? (bağlanma / kopma anını yakalamak için)
bool wasConnected = false;

// ------------------------------ Yardımcılar ------------------------------

// Seri monitöre printf gibi biçimli yazı yazar; yazı hem USB hem COM portuna gider.
void logf(const char *format, ...) {
  // Yazının hazırlanacağı 256 karakterlik geçici alan.
  char buffer[256];
  // Fonksiyona verilen değişken sayıdaki değerlere ulaşmak için liste.
  va_list args;
  // Listeyi format parametresinden sonraki değerlerle başlatır.
  va_start(args, format);
  // Biçim metnindeki %s, %d gibi yerleri değerlerle doldurup buffer'a yazar.
  vsnprintf(buffer, sizeof(buffer), format, args);
  // Değer listesiyle işi bitirir.
  va_end(args);
  // Hazırlanan yazıyı USB portundaki seri monitöre gönderir.
  Serial.print(buffer);
  // Aynı yazıyı COM portundaki seri monitöre gönderir.
  Serial0.print(buffer);
}

// JSON belgesini tek satırlık metne çevirip geri verir.
String toJson(const JsonDocument &doc) {
  // Sonucun yazılacağı boş metin.
  String json;
  // JSON belgesini metne çevirip json değişkenine yazar.
  serializeJson(doc, json);
  // Hazırlanan metni geri verir.
  return json;
}

// Metindeki harf sayısını verir. Türkçe harfler (ç, ş, ğ...) birden fazla bayt tuttuğu için
// bayt sayısı yerine harf sayısı sayılır.
size_t charCount(const String &text) {
  // Sayılan harf sayısı.
  size_t count = 0;
  // Metnin her baytına tek tek bakar.
  for (size_t i = 0; i < text.length(); i++) {
    // Bayt bir harfin devamı değil de yeni bir harfin başlangıcıysa (UTF-8 kuralı)...
    if ((text[i] & 0xC0) != 0x80) {
      // ...harf sayısını bir artırır.
      count++;
    }
  }
  // Toplam harf sayısını geri verir.
  return count;
}

// Bir mesajın bilgilerini, sayfaya gönderilecek JSON nesnesine yazar.
void writeMessage(JsonObject obj, const Message &message) {
  // Mesajın benzersiz kimliği (sayfa aynı mesajı iki kez göstermesin diye).
  obj["id"] = message.id;
  // Mesajı yazan kişinin adı.
  obj["user"] = message.user;
  // Mesajın metni.
  obj["text"] = message.text;
  // Mesajın kaydedildiği zaman.
  obj["time"] = message.time;
}

// Bir JSON mesajını sohbete katılmış herkese gönderir.
void broadcast(const JsonDocument &doc) {
  // JSON'u bir kez metne çevirir (her kullanıcı için yeniden çevirmemek için).
  String json = toJson(doc);
  // Sohbetteki her kullanıcı için...
  for (auto &user : users) {
    // ...metni o kullanıcının WebSocket bağlantısına gönderir.
    ws.text(user.first, json);
  }
}

// Tek bir tarayıcıya hata mesajı gönderir (ör. "Bu isim kullanılıyor").
void sendError(uint32_t client, const char *code, const char *text) {
  // Gönderilecek JSON belgesi.
  JsonDocument doc;
  // Mesajın türü: hata.
  doc["type"] = "error";
  // Hatanın kısa kodu (programın anlaması için).
  doc["code"] = code;
  // Kullanıcıya gösterilecek açıklama.
  doc["text"] = text;
  // Hatayı sadece o tarayıcıya gönderir.
  ws.text(client, toJson(doc));
}

// Sohbetteki herkese "Ali katıldı" gibi bir sistem satırı gönderir.
void broadcastSystem(const String &text) {
  // Gönderilecek JSON belgesi.
  JsonDocument doc;
  // Mesajın türü: sistem bildirimi.
  doc["type"] = "system";
  // Gösterilecek yazı.
  doc["text"] = text;
  // Herkese gönderir.
  broadcast(doc);
}

// Çevrimiçi kullanıcı listesini sohbetteki herkese gönderir.
void broadcastUsers() {
  // Gönderilecek JSON belgesi.
  JsonDocument doc;
  // Mesajın türü: kullanıcı listesi.
  doc["type"] = "users";
  // İsimlerin ekleneceği boş dizi.
  JsonArray list = doc["users"].to<JsonArray>();
  // Sohbetteki her kullanıcı için...
  for (auto &user : users) {
    // ...adını diziye ekler.
    list.add(user.second);
  }
  // Listeyi herkese gönderir.
  broadcast(doc);
}

// Firestore'a ulaşılabilirlik durumunu günceller; durum değiştiyse açık olan bütün sayfalara bildirir.
void setOnline(bool value) {
  // Durum değişmediyse...
  if (value == online) {
    // ...hiçbir şey yapmadan çıkar.
    return;
  }
  // Yeni durumu kaydeder.
  online = value;
  // Seri monitöre durumu yazar.
  logf("Firestore %s\n", online ? "baglandi" : "baglantisi yok");
  // Sayfalara gönderilecek JSON belgesi.
  JsonDocument doc;
  // Mesajın türü: bağlantı durumu.
  doc["type"] = "status";
  // Firestore'a ulaşılabiliyor mu (true/false).
  doc["online"] = online;
  // Sohbete katılmamış olanlar dahil, karta bağlı bütün tarayıcılara gönderir.
  ws.textAll(toJson(doc));
}

// Verilen kimlikteki mesaj geçmişte zaten var mı? (aynı mesaj iki kez eklenmesin diye)
bool hasMessage(const String &id) {
  // Geçmişteki her mesaja bakar.
  for (const Message &message : history) {
    // Kimlik eşleşirse...
    if (message.id == id) {
      // ...mesaj zaten var demektir.
      return true;
    }
  }
  // Hiçbiri eşleşmediyse mesaj yeni demektir.
  return false;
}

// Mesajı geçmişe ekler ve sohbetteki herkese gönderir.
void addMessage(const Message &message) {
  // Mesajı geçmişin sonuna (en yeni olarak) ekler.
  history.push_back(message);
  // Geçmişteki mesaj sayısı sınırı aştıysa...
  if (history.size() > HISTORY_SIZE) {
    // ...en eski mesajı siler.
    history.pop_front();
  }
  // Gönderilecek JSON belgesi.
  JsonDocument doc;
  // Mesajın türü: yeni sohbet mesajı.
  doc["type"] = "message";
  // Mesajın bilgilerini "message" alanına yazar.
  writeMessage(doc["message"].to<JsonObject>(), message);
  // Herkese gönderir.
  broadcast(doc);
}

// ------------------------------ Firestore ------------------------------

// Firestore REST API'ye JSON gönderir (POST). Sunucunun cevabı response'a yazılır.
// İstek başarılıysa (HTTP 200) true döner.
bool firestorePost(const char *action, const String &body, String &response) {
  // İsteği hazırlar: şifreli bağlantı + tam adres (ör. .../documents:runQuery).
  http.begin(tls, firestoreUrl + action);
  // Sunucuya gönderilen verinin JSON olduğunu bildirir.
  http.addHeader("Content-Type", "application/json");
  // İsteği gönderir ve sunucunun cevap kodunu alır (200 = başarılı).
  int code = http.POST(body);
  // Sunucuya ulaşıldıysa cevabın metnini, ulaşılamadıysa hatanın açıklamasını alır.
  response = code > 0 ? http.getString() : http.errorToString(code);
  // İsteği bitirir (bağlantı bir sonraki istek için açık kalır).
  http.end();
  // İstek başarısız olduysa...
  if (code != 200) {
    // ...hatayı seri monitöre yazar (cevabın sadece ilk 200 harfini).
    logf("Firestore hatasi (HTTP %d): %s\n", code, response.substring(0, 200).c_str());
  }
  // Başarılı mı değil mi, onu geri verir.
  return code == 200;
}

// Firestore'dan gelen bir belgeyi (JSON) Message yapısına çevirir. Gerekli bilgiler varsa true döner.
bool readDocument(JsonObject document, Message &message) {
  // Belgenin tam adını alır (ör. projects/.../documents/messages/Ab12Cd34...).
  String name = document["name"] | "";
  // Tam adın son "/" işaretinden sonraki kısmı mesajın kimliğidir.
  message.id = name.substring(name.lastIndexOf('/') + 1);
  // "user" alanındaki metni (kullanıcı adı) alır; yoksa boş bırakır.
  message.user = document["fields"]["user"]["stringValue"] | "";
  // "text" alanındaki metni (mesaj) alır; yoksa boş bırakır.
  message.text = document["fields"]["text"]["stringValue"] | "";
  // "createdAt" alanındaki zamanı alır; yoksa boş bırakır.
  message.time = document["fields"]["createdAt"]["timestampValue"] | "";
  // Kimlik ve zaman doluysa belge geçerlidir.
  return message.id.length() > 0 && message.time.length() > 0;
}

// Firestore'dan yeni mesajları okur. İlk seferde son HISTORY_SIZE mesajı, sonra yalnızca
// en son okunan mesajdan (lastTime) daha yeni olanları ister.
void pollFirestore() {
  // Firestore'a gönderilecek sorgu (JSON).
  JsonDocument query;
  // Sorgunun ana kısmı: "structuredQuery".
  JsonObject structured = query["structuredQuery"].to<JsonObject>();
  // Hangi koleksiyondan okunacağı: messages.
  structured["from"][0]["collectionId"] = COLLECTION;
  // Sıralama ayarının yazılacağı nesne.
  JsonObject order = structured["orderBy"][0].to<JsonObject>();
  // Mesajlar kayıt zamanına (createdAt) göre sıralanacak.
  order["field"]["fieldPath"] = "createdAt";
  // En fazla HISTORY_SIZE (50) mesaj istenecek.
  structured["limit"] = HISTORY_SIZE;
  // Daha önce hiç mesaj okunmadıysa bu ilk yüklemedir.
  bool firstLoad = lastTime.isEmpty();
  // İlk yüklemedeysek...
  if (firstLoad) {
    // ...en yeniden eskiye sıralayıp son 50 mesajı al.
    order["direction"] = "DESCENDING";
  // İlk yükleme değilse...
  } else {
    // ...eskiden yeniye sırala.
    order["direction"] = "ASCENDING";
    // Filtre ayarının yazılacağı nesne.
    JsonObject filter = structured["where"]["fieldFilter"].to<JsonObject>();
    // Filtre kayıt zamanına (createdAt) uygulanacak.
    filter["field"]["fieldPath"] = "createdAt";
    // Koşul: "büyüktür", yani daha yeni.
    filter["op"] = "GREATER_THAN";
    // Karşılaştırılacak zaman: en son okuduğumuz mesajın zamanı.
    filter["value"]["timestampValue"] = lastTime;
  }

  // Sunucunun cevabının yazılacağı metin.
  String response;
  // Sorguyu Firestore'un ":runQuery" adresine gönderir.
  bool ok = firestorePost(":runQuery", toJson(query), response);
  // Sorgunun başarısına göre "Firestore bağlı" durumunu günceller.
  setOnline(ok);
  // Sorgu başarısız olduysa...
  if (!ok) {
    // ...bu turu bitirir; 4 saniye sonra yeniden denenir.
    return;
  }

  // Cevabın açılacağı JSON belgesi.
  JsonDocument result;
  // Cevap metnini JSON'a çevirir; çevrilemezse...
  if (deserializeJson(result, response)) {
    // ...hatayı seri monitöre yazar...
    logf("Firestore cevabi okunamadi\n");
    // ...ve bu turu bitirir.
    return;
  }
  // Cevaptan çıkan mesajların geçici listesi.
  std::vector<Message> found;
  // Cevaptaki her sonuç için...
  for (JsonObject item : result.as<JsonArray>()) {
    // Sonucun çevrileceği boş mesaj.
    Message message;
    // Sonuçta geçerli bir belge varsa...
    if (readDocument(item["document"], message)) {
      // ...mesajı listeye ekler.
      found.push_back(message);
    }
  }
  // İlk yüklemede mesajlar yeniden eskiye geldiği için...
  if (firstLoad) {
    // ...listeyi ters çevirip eskiden yeniye sıralar.
    std::reverse(found.begin(), found.end());
  }
  // Listedeki her mesaj için (eskiden yeniye)...
  for (const Message &message : found) {
    // ...en son görülen zamanı günceller (bir sonraki sorgu bundan yenilerini ister).
    lastTime = message.time;
    // Mesaj daha önce eklenmediyse (ör. bu kartın kendi yazdığı mesaj değilse)...
    if (!hasMessage(message.id)) {
      // ...geçmişe ekler ve herkese gönderir.
      addMessage(message);
    }
  }
}

// Firestore'da yeni mesaja verilecek rastgele belge adını (20 harf/rakam) üretir.
String randomId() {
  // Kullanılabilecek karakterler: büyük harfler, küçük harfler ve rakamlar (toplam 62).
  const char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  // Üretilen kimlik.
  String id;
  // 20 kez...
  for (int i = 0; i < 20; i++) {
    // ...kartın donanım rastgele sayı üretecinden bir karakter seçip ekler.
    id += chars[esp_random() % 62];
  }
  // Hazırlanan kimliği geri verir.
  return id;
}

// Mesajı Firestore'a kaydeder. Kayıt zamanını (createdAt) kart değil, Firestore'un kendi saati yazar;
// böylece farklı kartların saatleri yanlış olsa bile mesajlar doğru sıralanır.
// Başarılıysa kaydedilen mesaj "saved" içine yazılır ve true döner.
bool saveToFirestore(const String &user, const String &text, Message &saved) {
  // Yeni mesajın kimliğini üretir.
  String id = randomId();
  // Firestore'a gönderilecek istek (JSON).
  JsonDocument body;
  // Yapılacak yazma işleminin bilgilerinin konacağı nesne ("writes" dizisinin ilk elemanı).
  JsonObject write = body["writes"][0].to<JsonObject>();
  // Yazılacak belgenin tam adı: projects/<proje>/databases/(default)/documents/messages/<kimlik>.
  write["update"]["name"] = String("projects/") + FIREBASE_PROJECT_ID + "/databases/(default)/documents/" +
                            COLLECTION + "/" + id;
  // Belgenin "user" alanı: mesajı yazanın adı.
  write["update"]["fields"]["user"]["stringValue"] = user;
  // Belgenin "text" alanı: mesajın metni.
  write["update"]["fields"]["text"]["stringValue"] = text;
  // Firestore'un sunucu tarafında dolduracağı alanın ayarı.
  JsonObject transform = write["updateTransforms"][0].to<JsonObject>();
  // Doldurulacak alan: createdAt.
  transform["fieldPath"] = "createdAt";
  // Değeri: isteğin sunucuya ulaştığı an (Firestore'un saati).
  transform["setToServerValue"] = "REQUEST_TIME";
  // Koşul: bu kimlikte bir belge daha önce yoksa yaz (var olan mesajın üzerine yazma).
  write["currentDocument"]["exists"] = false;

  // Sunucunun cevabının yazılacağı metin.
  String response;
  // İsteği Firestore'un ":commit" adresine gönderir.
  bool ok = firestorePost(":commit", toJson(body), response);
  // İsteğin başarısına göre "Firestore bağlı" durumunu günceller.
  setOnline(ok);
  // Kayıt başarısız olduysa...
  if (!ok) {
    // ...false döner.
    return false;
  }
  // Cevabın açılacağı JSON belgesi.
  JsonDocument result;
  // Cevap metnini JSON'a çevirir.
  deserializeJson(result, response);
  // Kaydedilen mesajı doldurur; zamanı Firestore'un cevabındaki createdAt değeridir.
  saved = {id, user, text, result["writeResults"][0]["transformResults"][0]["timestampValue"] | ""};
  // Kaydın başarılı olduğunu bildirir.
  return true;
}

// ------------------------------ Sohbet ------------------------------

// Yeni bağlanan tarayıcıya kartın bilgilerini (ağ adı, şifre, proje, Firestore durumu) gönderir.
// Karşılama sayfasındaki adımlar bu bilgilerle doldurulur.
void sendInfo(uint32_t client) {
  // Gönderilecek JSON belgesi.
  JsonDocument doc;
  // Mesajın türü: bilgi.
  doc["type"] = "info";
  // Kartın yayınladığı Wi-Fi ağının adı.
  doc["ssid"] = apName;
  // Kartın yayınladığı Wi-Fi ağının şifresi.
  doc["password"] = AP_PASSWORD;
  // Firebase projesinin kimliği.
  doc["project"] = FIREBASE_PROJECT_ID;
  // Firestore'a şu an ulaşılabiliyor mu.
  doc["online"] = online;
  // Bilgiyi sadece o tarayıcıya gönderir.
  ws.text(client, toJson(doc));
}

// Bir tarayıcı "join" (katıl) mesajı gönderince çalışır: ismi kontrol eder, kullanıcıyı sohbete alır.
void handleJoin(uint32_t client, String name) {
  // İsmin başındaki ve sonundaki boşlukları siler.
  name.trim();
  // Bu bağlantı zaten sohbetteyse...
  if (users.count(client)) {
    // ...tekrar katılmasına gerek yok, çıkar.
    return;
  }
  // İsim boşsa ya da 20 harften uzunsa...
  if (name.isEmpty() || charCount(name) > MAX_NAME_CHARS) {
    // ...tarayıcıya hata gönderir...
    sendError(client, "invalid_name", "İsim 1-20 karakter olmalı.");
    // ...ve çıkar.
    return;
  }
  // Sohbetteki her kullanıcıya bakar...
  for (auto &user : users) {
    // ...aynı isim (büyük/küçük harf farkı gözetmeden) kullanılıyorsa...
    if (user.second.equalsIgnoreCase(name)) {
      // ...tarayıcıya hata gönderir...
      sendError(client, "name_taken", "Bu isim şu an sohbette kullanılıyor, başka bir isim seçin.");
      // ...ve çıkar.
      return;
    }
  }

  // Kullanıcıyı sohbettekiler listesine ekler.
  users[client] = name;
  // Seri monitöre yazar.
  logf("Katildi: %s\n", name.c_str());

  // "Hoş geldin" mesajı için JSON belgesi.
  JsonDocument welcome;
  // Mesajın türü: hoş geldin (sayfa bunu alınca sohbet ekranına geçer).
  welcome["type"] = "welcome";
  // Kabul edilen isim.
  welcome["name"] = name;
  // Hoş geldin mesajını sadece bu tarayıcıya gönderir.
  ws.text(client, toJson(welcome));

  // Geçmiş mesajlar için JSON belgesi.
  JsonDocument past;
  // Mesajın türü: geçmiş.
  past["type"] = "history";
  // Mesajların ekleneceği boş dizi.
  JsonArray messages = past["messages"].to<JsonArray>();
  // Geçmişteki her mesaj için...
  for (const Message &message : history) {
    // ...diziye yeni bir eleman ekleyip mesajın bilgilerini yazar.
    writeMessage(messages.add<JsonObject>(), message);
  }
  // Geçmişi sadece bu tarayıcıya gönderir.
  ws.text(client, toJson(past));

  // Herkese "<isim> katıldı" yazar.
  broadcastSystem(name + " katıldı");
  // Herkese güncel kullanıcı listesini gönderir.
  broadcastUsers();
}

// Bir tarayıcı "chat" (mesaj) gönderince çalışır: mesajı kontrol eder, Firestore'a kaydeder, herkese iletir.
void handleChat(uint32_t client, String text) {
  // Mesajın başındaki ve sonundaki boşlukları siler.
  text.trim();
  // Gönderen sohbete katılmamışsa ya da mesaj boşsa...
  if (!users.count(client) || text.isEmpty()) {
    // ...hiçbir şey yapmadan çıkar.
    return;
  }
  // Mesaj 300 harften uzunsa...
  if (charCount(text) > MAX_TEXT_CHARS) {
    // ...tarayıcıya hata gönderir...
    sendError(client, "too_long", "Mesaj en fazla 300 karakter olabilir.");
    // ...ve çıkar.
    return;
  }
  // Kart telefonun hotspot'una bağlı değilse (internet yoksa)...
  if (WiFi.status() != WL_CONNECTED) {
    // ...tarayıcıya hata gönderir...
    sendError(client, "offline", "Kartın interneti yok, mesaj gönderilemedi.");
    // ...ve çıkar.
    return;
  }
  // Kaydedilen mesajın yazılacağı yapı.
  Message saved;
  // Mesajı Firestore'a kaydetmeyi dener; başaramazsa...
  if (!saveToFirestore(users[client], text, saved)) {
    // ...tarayıcıya hata gönderir...
    sendError(client, "save_failed", "Mesaj Firestore'a kaydedilemedi, tekrar deneyin.");
    // ...ve çıkar.
    return;
  }
  // Seri monitöre kaydedilen mesajı yazar.
  logf("Mesaj kaydedildi: %s: %s\n", saved.user.c_str(), saved.text.c_str());
  // Mesajı geçmişe ekler ve herkese gönderir (Firestore'dan tekrar okunmasını beklemeden).
  addMessage(saved);
}

// Bir tarayıcı çıkış yapınca ya da bağlantısı kopunca çalışır: kullanıcıyı sohbetten çıkarır.
void handleLeave(uint32_t client) {
  // Bu bağlantının sohbettekiler listesindeki kaydını arar.
  auto user = users.find(client);
  // Kayıt yoksa (sohbete hiç katılmamışsa)...
  if (user == users.end()) {
    // ...yapılacak bir şey yok, çıkar.
    return;
  }
  // Kullanıcının adını saklar (kaydı silmeden önce).
  String name = user->second;
  // Kullanıcıyı listeden siler.
  users.erase(user);
  // Seri monitöre yazar.
  logf("Ayrildi: %s\n", name.c_str());
  // Herkese "<isim> ayrıldı" yazar.
  broadcastSystem(name + " ayrıldı");
  // Herkese güncel kullanıcı listesini gönderir.
  broadcastUsers();
}

// WebSocket'te bir şey olunca (bağlanma, kopma, mesaj gelmesi) kütüphane bu fonksiyonu çağırır.
// Web sunucusunun kendi görevinde çalışır; bu yüzden işi burada yapmaz, olayı sadece kuyruğa koyar.
void onWsEvent(AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data,
               size_t len) {
  // Kuyruğa konacak olay; varsayılan olarak "bağlandı" türünde ve bu tarayıcıya ait.
  Event event{Event::Connect, client->id(), ""};
  // Yeni bir tarayıcı bağlandıysa...
  if (type == WS_EVT_CONNECT) {
    // ...10 saniyede bir "orada mısın?" sinyali gönderilsin; kapanan sekmeler böylece anlaşılır.
    client->keepAlivePeriod(10);
  // Bir tarayıcının bağlantısı koptuysa...
  } else if (type == WS_EVT_DISCONNECT) {
    // ...olay "ayrıldı" olur.
    event.kind = Event::Leave;
  // Tarayıcıdan veri geldiyse...
  } else if (type == WS_EVT_DATA) {
    // ...gelen parçanın bilgilerini alır.
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    // Mesaj tek parça değilse, metin değilse ya da 2048 bayttan büyükse...
    if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT || len > 2048) {
      // ...yok sayar.
      return;
    }
    // Gelen JSON'un açılacağı belge.
    JsonDocument doc;
    // Gelen metni JSON'a çevirir; çevrilemezse...
    if (deserializeJson(doc, data, len)) {
      // ...yok sayar.
      return;
    }
    // Mesajın türünü okur ("join", "chat" ya da "leave").
    String kind = doc["type"] | "";
    // Katılma isteğiyse...
    if (kind == "join") {
      // ...olay "katıl" olur, metni de istenen isimdir.
      event = {Event::Join, client->id(), doc["name"] | ""};
    // Sohbet mesajıysa...
    } else if (kind == "chat") {
      // ...olay "mesaj" olur, metni de mesajın kendisidir.
      event = {Event::Chat, client->id(), doc["text"] | ""};
    // Çıkış isteğiyse...
    } else if (kind == "leave") {
      // ...olay "ayrıldı" olur.
      event.kind = Event::Leave;
    // Bilinmeyen bir türse...
    } else {
      // ...yok sayar.
      return;
    }
  // Başka bir olay türüyse (ör. ping)...
  } else {
    // ...yok sayar.
    return;
  }
  // Kuyruğu kilitler (loop() aynı anda kuyruktan okumasın diye); kilit fonksiyon bitince açılır.
  std::lock_guard<std::mutex> lock(inboxLock);
  // Olayı kuyruğun sonuna ekler.
  inbox.push_back(event);
}

// Kuyrukta bekleyen olayları sırayla işler; loop() içinden çağrılır.
void processInbox() {
  // Kuyruk boşalana kadar döner.
  while (true) {
    // Kuyruktan alınacak olay.
    Event event;
    // Bu süslü parantez bloğu bitince kilit kendiliğinden açılır.
    {
      // Kuyruğu kilitler (web sunucusu aynı anda kuyruğa yazmasın diye).
      std::lock_guard<std::mutex> lock(inboxLock);
      // Kuyruk boşsa...
      if (inbox.empty()) {
        // ...işlenecek olay kalmamıştır, çıkar.
        return;
      }
      // Kuyruğun başındaki (en eski) olayı alır.
      event = inbox.front();
      // Alınan olayı kuyruktan siler.
      inbox.pop_front();
    }
    // Olayın türüne göre ilgili fonksiyonu çağırır.
    switch (event.kind) {
      // Bağlandı: tarayıcıya kartın bilgilerini gönder.
      case Event::Connect: sendInfo(event.client); break;
      // Katılmak istiyor: ismi kontrol edip sohbete al.
      case Event::Join: handleJoin(event.client, event.text); break;
      // Mesaj yazdı: Firestore'a kaydedip herkese ilet.
      case Event::Chat: handleChat(event.client, event.text); break;
      // Ayrıldı: sohbetten çıkar.
      case Event::Leave: handleLeave(event.client); break;
    }
  }
}

// ------------------------------ Başlangıç ------------------------------

// Kart açıldığında (ya da resetlendiğinde) bir kez çalışır.
void setup() {
  // USB portundaki seri monitörü 115200 hızında başlatır.
  Serial.begin(115200);
  // COM portundaki seri monitörü 115200 hızında başlatır.
  Serial0.begin(115200);
  // Seri monitörün bağlanmasına 1 saniye zaman tanır; ilk yazılar kaybolmasın.
  delay(1000);
  // Programın başladığını seri monitöre yazar.
  logf("\nDPU ROT ESP32 Egitimi - Firestore sohbet odasi\n");

  // Kartın hafızasındaki dosya sistemini açar; açamazsa...
  if (!LittleFS.begin()) {
    // ...web sitesi dosyalarının yüklenmesi gerektiğini yazar.
    logf("Web sitesi bulunamadi: once 'pio run -t uploadfs' ile web/ klasorunu yukleyin\n");
  }

  // Kart hem telefona bağlansın (istemci) hem kendi ağını yayınlasın (erişim noktası).
  WiFi.mode(WIFI_AP_STA);
  // Kartın donanımına kazınmış benzersiz MAC adresini okur.
  uint64_t mac = ESP.getEfuseMac();
  // Ağ adının sonuna eklenecek 4 karakter (+ metin sonu işareti için 1 karakter).
  char suffix[5];
  // MAC adresinin son iki baytını 4 karakterlik onaltılık sayıya çevirir (ör. "40D4").
  snprintf(suffix, sizeof(suffix), "%02X%02X", (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  // Ağın tam adını oluşturur (ör. DPU-ROT-ESP32-40D4).
  apName = String(AP_NAME_PREFIX) + suffix;
  // Kartın kendi Wi-Fi ağını bu ad ve şifreyle yayınlamaya başlar (adresi 192.168.4.1 olur).
  WiFi.softAP(apName.c_str(), AP_PASSWORD);
  // Telefonun hotspot'una bağlanmaya başlar (internete çıkmak için).
  WiFi.begin(HOTSPOT_SSID, HOTSPOT_PASSWORD);
  // Bağlanma denemesinin zamanını kaydeder.
  lastWifiTry = millis();

  // Firestore'un sertifikası, kartta hazır bulunan güvenilir kök sertifika listesiyle doğrulanacak.
  tls.useBuiltinCACertBundle();
  // HTTP bağlantısı her istekten sonra kapatılmasın, bir sonraki istekte tekrar kullanılsın.
  http.setReuse(true);
  // Firestore REST API'nin ana adresini proje kimliğiyle oluşturur.
  firestoreUrl = String("https://firestore.googleapis.com/v1/projects/") + FIREBASE_PROJECT_ID +
                 "/databases/(default)/documents";

  // WebSocket'te bir olay olunca onWsEvent fonksiyonu çağrılsın.
  ws.onEvent(onWsEvent);
  // WebSocket'i web sunucusuna ekler (ws://192.168.4.1/ws).
  server.addHandler(&ws);
  // Web sitesi dosyalarını kartın hafızasından sunar; adres "/" ise index.html açılır.
  // "no-cache": tarayıcı her seferinde güncel dosyayı istesin.
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html").setCacheControl("no-cache");
  // Olmayan bir adres istenirse "Bulunamadi" (404) cevabı gönderir.
  server.onNotFound([](AsyncWebServerRequest *request) { request->send(404, "text/plain", "Bulunamadi"); });
  // Web sunucusunu başlatır.
  server.begin();

  // Kartın ağ adını, şifresini ve sayfa adresini seri monitöre yazar.
  logf("Wi-Fi yayini: %s (sifre: %s)  sayfa: http://%s\n", apName.c_str(), AP_PASSWORD,
       WiFi.softAPIP().toString().c_str());
  // Hangi hotspot'a bağlanılmaya çalışıldığını seri monitöre yazar.
  logf("'%s' hotspot'una baglaniliyor...\n", HOTSPOT_SSID);
}

// setup() bittikten sonra sürekli, tekrar tekrar çalışır.
void loop() {
  // WebSocket'ten gelip kuyrukta bekleyen olayları işler.
  processInbox();

  // Son temizlikten bu yana 1 saniye geçtiyse...
  if (millis() - lastCleanup >= 1000) {
    // ...temizlik zamanını günceller...
    lastCleanup = millis();
    // ...ve kopmuş WebSocket bağlantılarını hafızadan temizler.
    ws.cleanupClients();
  }

  // Kart şu an telefonun hotspot'una bağlı mı?
  bool connected = WiFi.status() == WL_CONNECTED;
  // Bağlantı durumu bir önceki turdan farklıysa (yeni bağlandı ya da koptu)...
  if (connected != wasConnected) {
    // ...yeni durumu kaydeder...
    wasConnected = connected;
    // ...bağlandıysa IP adresini, koptuysa durumu seri monitöre yazar...
    logf(connected ? "Hotspot'a baglandi, IP: %s\n" : "Hotspot baglantisi yok%s\n",
         connected ? WiFi.localIP().toString().c_str() : "");
    // ...ve Firestore'a hemen bakılsın diye bekleme süresini sıfırlar.
    lastPoll = 0;
  }
  // Hotspot'a bağlı değilsek...
  if (!connected) {
    // ...Firestore'a da ulaşılamaz.
    setOnline(false);
    // Son denemeden bu yana 15 saniye geçtiyse...
    if (millis() - lastWifiTry >= WIFI_RETRY_MS) {
      // ...yarım kalan bağlantı denemesini durdurur...
      WiFi.disconnect();
      // ...hotspot'a yeniden bağlanmayı dener...
      WiFi.begin(HOTSPOT_SSID, HOTSPOT_PASSWORD);
      // ...ve deneme zamanını kaydeder.
      lastWifiTry = millis();
    }
    // İnternet olmadığı için bu turun geri kalanını (Firestore'u) atlar.
    return;
  }

  // Hiç bakılmadıysa ya da son bakıştan bu yana 4 saniye geçtiyse...
  if (lastPoll == 0 || millis() - lastPoll >= POLL_MS) {
    // ...Firestore'daki yeni mesajları okur...
    pollFirestore();
    // ...ve bakış zamanını kaydeder.
    lastPoll = millis();
  }
}
