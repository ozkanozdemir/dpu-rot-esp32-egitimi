// Bulut Sohbet istemcisi. Kartla WebSocket (ws://<adres>/ws) üzerinden JSON mesajlaşır.
// Mesaj biçimleri için src/main.cpp dosyasının başındaki açıklamaya bakın.

const $ = (id) => document.getElementById(id);

const NAME_KEY = "chat.name";            // localStorage: son kullanılan isim (form doldurmak için)
const SESSION_NAME_KEY = "chat.session"; // sessionStorage: sayfa yenilenince aynı isimle geri dönmek için
const RECONNECT_MS = 2000;

let socket = null;
let myName = null;      // sohbete katılınca dolar
let wantJoin = false;   // kullanıcı sohbette olmak istiyor mu (kopunca yeniden katılmak için)
let reconnectTimer = null;
const seen = new Set(); // ekrandaki mesajların kimlikleri (aynı mesaj iki kez gösterilmesin)

// ---------- Tarayıcı depolaması (gizli sekmede hata verebilir) ----------

function load(storage, key) {
  try { return storage.getItem(key); } catch { return null; }
}

function save(storage, key, value) {
  try {
    if (value === null) storage.removeItem(key);
    else storage.setItem(key, value);
  } catch { /* depolama kapalıysa sessizce geç */ }
}

// ---------- WebSocket bağlantısı ----------

function connect() {
  if (socket && socket.readyState <= WebSocket.OPEN) return;
  clearTimeout(reconnectTimer);
  setBoard(false);

  socket = new WebSocket(`ws://${location.host}/ws`);

  socket.addEventListener("open", () => {
    setBoard(true);
    if (wantJoin) send({ type: "join", name: $("name-input").value.trim() });
  });

  socket.addEventListener("message", (event) => {
    let msg;
    try { msg = JSON.parse(event.data); } catch { return; }
    handleMessage(msg);
  });

  socket.addEventListener("close", () => {
    setBoard(false);
    if (myName) addSystem("Bağlantı koptu, yeniden bağlanılıyor…");
    myName = null;
    reconnectTimer = setTimeout(connect, RECONNECT_MS);
  });
}

function send(obj) {
  if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify(obj));
    return true;
  }
  return false;
}

// ---------- Karttan gelen mesajlar ----------

function handleMessage(msg) {
  switch (msg.type) {
    case "info":
      $("ap-name").textContent = msg.ssid;
      $("ap-password").textContent = msg.password;
      setCloud(msg.online);
      break;

    case "status":
      setCloud(msg.online);
      break;

    case "welcome":
      myName = msg.name;
      save(sessionStorage, SESSION_NAME_KEY, msg.name);
      save(localStorage, NAME_KEY, msg.name);
      showChat();
      break;

    case "history":
      $("messages").replaceChildren();
      seen.clear();
      for (const m of msg.messages) addChat(m);
      scrollToBottom();
      break;

    case "message":
      addChat(msg.message);
      break;

    case "users":
      renderUsers(msg.users);
      break;

    case "system":
      addSystem(msg.text);
      break;

    case "error":
      if (myName) {
        addSystem(msg.text, true);
      } else {
        // Giriş reddedildi (isim alınmış vb.): giriş ekranında göster, otomatik deneme yapma
        wantJoin = false;
        save(sessionStorage, SESSION_NAME_KEY, null);
        showLogin(msg.text);
      }
      break;
  }
}

// ---------- Ekranlar ----------

function showLogin(error) {
  $("chat").hidden = true;
  $("login").hidden = false;
  $("login-error").textContent = error || "";
  $("login-error").hidden = !error;
  $("name-input").focus();
}

function showChat() {
  $("login").hidden = true;
  $("chat").hidden = false;
  $("message-input").focus();
}

// Tarayıcı karta bağlı mı
function setBoard(connected) {
  $("login-dot").classList.toggle("online", connected);
  $("login-status").textContent = connected ? "Karta bağlandı" : "Karta bağlanıyor…";
  $("login-button").disabled = !connected;
  if (!connected) setCloud(false);
}

// Kart Firestore'a ulaşabiliyor mu
function setCloud(online) {
  $("cloud-dot").classList.toggle("online", online);
  $("status-dot").classList.toggle("online", online);
  $("cloud-status").textContent = online ? "Firestore bağlı" : "Firestore bekleniyor…";
  $("header-status").textContent = online ? "ESP32-S3 · Firestore bağlı" : "ESP32-S3 · Firestore bağlantısı yok";
}

// ---------- İsim rozetleri (avatar) ----------

// Her isme sabit bir renk tonu: aynı isim herkeste aynı renkte görünür
function nameHue(name) {
  let hash = 0;
  for (const ch of name) hash = (hash * 31 + ch.codePointAt(0)) >>> 0;
  return hash % 360;
}

function makeAvatar(name) {
  const el = document.createElement("span");
  el.className = "avatar";
  el.textContent = Array.from(name)[0] || "?";
  el.setAttribute("aria-hidden", "true");
  return el;
}

function renderUsers(names) {
  $("user-count").textContent = names.length;
  const items = names
    .slice()
    .sort((a, b) => a.localeCompare(b, "tr"))
    .map((name) => {
      const li = document.createElement("li");
      li.style.setProperty("--hue", nameHue(name));
      if (name === myName) li.classList.add("me");
      const label = document.createElement("span");
      label.className = "name";
      label.textContent = name;
      li.append(makeAvatar(name), label);
      return li;
    });
  $("user-list").replaceChildren(...items);
}

// ---------- Mesaj listesi ----------

// Bugünün mesajında sadece saat, eski mesajlarda gün de görünsün
function formatTime(date) {
  const time = date.toLocaleTimeString("tr-TR", { hour: "2-digit", minute: "2-digit" });
  if (date.toDateString() === new Date().toDateString()) return time;
  return `${date.toLocaleDateString("tr-TR", { day: "numeric", month: "short" })} ${time}`;
}

function updateEmpty() {
  $("empty").hidden = $("messages").children.length > 0;
}

function scrollToBottom() {
  const list = $("messages");
  list.scrollTop = list.scrollHeight;
}

// Mesajı zamanına göre doğru yere koyar. Kullanıcı zaten en alttaysa aşağı kaydırır
// (yukarıda eski mesajları okuyorsa rahatsız etmez).
function insertMessage(li, time) {
  const list = $("messages");
  const atBottom = list.scrollHeight - list.scrollTop - list.clientHeight < 80;
  li.dataset.time = time;
  const later = [...list.children].find((row) => row.dataset.time > time);
  list.insertBefore(li, later || null);
  if (atBottom) scrollToBottom();
  updateEmpty();
}

// Not: Metinler her zaman textContent ile yazılır; böylece birinin mesajına yazdığı HTML/JS çalışmaz.
function addChat(m) {
  if (seen.has(m.id)) return;
  seen.add(m.id);

  const li = document.createElement("li");
  li.className = m.user === myName ? "msg mine" : "msg";
  li.style.setProperty("--hue", nameHue(m.user));

  const bubble = document.createElement("div");
  bubble.className = "bubble";

  const fromEl = document.createElement("span");
  fromEl.className = "from";
  fromEl.textContent = m.user;

  const textEl = document.createElement("span");
  textEl.className = "text";
  textEl.textContent = m.text;

  const timeEl = document.createElement("time");
  timeEl.className = "time";
  timeEl.dateTime = m.time;
  timeEl.textContent = formatTime(new Date(m.time));

  bubble.append(fromEl, textEl, timeEl);
  li.append(makeAvatar(m.user), bubble);
  insertMessage(li, m.time);
}

// Sistem satırları (katıldı/ayrıldı) o anki zamana göre sona eklenir
function addSystem(text, isError = false) {
  const li = document.createElement("li");
  li.className = isError ? "system error" : "system";
  li.textContent = text;
  insertMessage(li, new Date().toISOString());
}

// ---------- Kullanıcı etkileşimleri ----------

$("login-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const name = $("name-input").value.trim();
  if (!name) return;
  wantJoin = true;
  $("login-error").hidden = true;
  if (!send({ type: "join", name })) connect();
});

$("message-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const input = $("message-input");
  const text = input.value.trim();
  if (!text) return;
  if (send({ type: "chat", text })) input.value = "";
});

$("leave-button").addEventListener("click", () => {
  wantJoin = false;
  myName = null;
  save(sessionStorage, SESSION_NAME_KEY, null);
  // Karta çıktığımızı söyle, sonra bağlantıyı kapat; close olayı giriş ekranı için yeniden bağlanır
  send({ type: "leave" });
  if (socket) socket.close();
  showLogin();
});

$("users-toggle").addEventListener("click", () => {
  const open = $("users").classList.toggle("open");
  $("users-toggle").setAttribute("aria-expanded", open);
});

// ---------- Başlangıç ----------

// Sayfanın açıldığı adresi (kartın IP'si) göster; öğrenciler birbirine söyleyebilsin
$("site-address").textContent = location.host;

// Sayfa yenilendiyse kullanıcıyı aynı isimle sohbete geri al
const sessionName = load(sessionStorage, SESSION_NAME_KEY);
wantJoin = Boolean(sessionName);
$("name-input").value = sessionName || load(localStorage, NAME_KEY) || "";
connect();
