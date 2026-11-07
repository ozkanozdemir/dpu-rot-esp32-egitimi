# ESP32 Bulut Sohbet

**DPÜ Robot ve Otomasyon Topluluğu · ESP32 Eğitimi**

ESP32-S3 kartının hem kendi Wi-Fi ağını yayınladığı hem de telefonun internetine bağlandığı bir sohbet odası.
Karta bağlanan herkes tarayıcıdan bir isim seçip sohbete katılır; mesajlar **Google Firebase Firestore**'a kaydedilir
ve oradan okunur.

```
 Telefonunuz  ──Wi-Fi / WebSocket──▶  ESP32-S3  ──Hotspot / HTTPS──▶  Firestore
 (tarayıcı)                          (web sunucusu)                   (bulut veritabanı)
```

## Neler var?

- Kart kendi Wi-Fi ağını yayınlar: `DPU-ROT-ESP32-XXXX` (şifre `12345678`). Sınıftaki kartlar karışmasın diye
  sondaki 4 karakter her kartta farklıdır.
- Bu ağa bağlanıp tarayıcıda **http://192.168.4.1** açılınca karşılama sayfası gelir; isim girilince sohbete geçilir.
- Sohbette o karta bağlı kişiler, Firestore'daki son 50 mesaj ve yeni mesajlar görünür; yazılan mesaj Firestore'a kaydedilir.
- Kart aynı anda telefonun hotspot'una bağlanıp Firestore'a HTTPS ile ulaşır; yeni mesajlara 4 saniyede bir bakar.
- Gerekli kütüphaneler `lib/` klasöründe; derlemek için internetten kütüphane indirilmez.

## Klasörler

| Yol | İçerik |
|---|---|
| `src/main.cpp` | Kartın bütün kodu. Ayarlar dosyanın başında. |
| `src/hotspot.example.h` | Telefon hotspot bilgileri için örnek (gerçeği `hotspot.h`, GitHub'a gitmez). |
| `web/` | Web sitesi (HTML, CSS, JS, QR). Kartın hafızasına (LittleFS) ayrıca yüklenir. |
| `lib/` | ESPAsyncWebServer, AsyncTCP, ArduinoJson kütüphaneleri. |
| `firestore.rules` | Firestore güvenlik kuralları: herkes okuyabilir ve mesaj yazabilir, kimse silemez. |
| `platformio.ini` | ESP32-S3-DevKitC-1 N16R8 kart ayarları. |

## Kurulum

1. [PlatformIO](https://platformio.org/)'yu kurun (VS Code eklentisi ya da `pip install platformio`).
2. `src/hotspot.example.h` dosyasını `src/hotspot.h` adıyla kopyalayıp telefonunuzun hotspot adını ve şifresini yazın.
   ESP32 yalnızca 2.4 GHz ağlara bağlanır; iPhone'da **Kişisel Erişim Noktası → Uyumluluğu Artır** açık olmalı.
3. Kendi Firebase projenizi kullanacaksanız `src/main.cpp`'deki `FIREBASE_PROJECT_ID` değerini değiştirin,
   Firestore'u açın ve kuralları yükleyin:
   ```bash
   firebase deploy --only firestore:rules --project <proje-id>
   ```
4. Kartı USB ile bağlayıp web sitesini ve kodu yükleyin:
   ```bash
   pio run -t uploadfs
   pio run -t upload
   pio device monitor
   ```
5. Seri monitörde yazan Wi-Fi ağına bağlanın ve tarayıcıda **http://192.168.4.1** adresini açın.

## Tarayıcı ile kart arasındaki mesajlar

WebSocket adresi: `ws://192.168.4.1/ws`, mesajlar JSON.

| Yön | Mesaj |
|---|---|
| tarayıcı → kart | `{"type":"join","name":"Ali"}`, `{"type":"chat","text":"Merhaba"}`, `{"type":"leave"}` |
| kart → tarayıcı | `info`, `status`, `welcome`, `history`, `message`, `users`, `system`, `error` |

Firestore'da her mesaj `messages` koleksiyonunda bir belgedir: `user`, `text` ve Firestore'un kendi saatiyle
yazılan `createdAt`.

## Kütüphaneler

`lib/` klasöründeki kütüphaneler kendi lisanslarıyla dağıtılır:
[ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) ve
[AsyncTCP](https://github.com/ESP32Async/AsyncTCP) (LGPL-3.0),
[ArduinoJson](https://github.com/bblanchon/ArduinoJson) (MIT).
