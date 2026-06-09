/*.
 * ============================================================
 *  JAMSEHAT v4.1 — HR/SpO2 + OLED SH1106 1.3"
 *  ESP8266 NodeMCU + MAX30105
 *
 *  PERUBAHAN v4.1:
 *  - Jam lebih kecil (font fub20 → fub17 atau 6x13)
 *  - STATE_RESULT lebih terpusat, nilai besar di tengah
 *  - Sensor lebih sensitif: threshold IR 30000, amplitude 0x3F
 *    agar tetap akurat meski ada celah casing ~1mm
 *  - Hotspot SELALU ON di STATE_CLOCK, SCAN, RESULT
 *    Hanya mati saat STATE_SLEEP
 *  - Halaman web menyimpan history 10 hasil terakhir
 *
 *  ALUR:
 *  Boot → NTP (WiFi STA) → WiFi OFF
 *       → SoftAP "JamSehat" ON (dan tetap ON terus)
 *       → STATE_CLOCK (jam, tengah)
 *       → [tombol] → STATE_SCAN (30 detik)
 *       → STATE_RESULT (30 detik, HR & SpO2 besar di tengah)
 *       → STATE_CLOCK
 *       → [idle 1 menit] → STATE_SLEEP (OLED + AP mati)
 *       → [tombol] → AP ON → STATE_CLOCK
 *
 *  ANDROID: connect WiFi "JamSehat" pw:12345678
 *           buka browser → http://192.168.4.1
 *           update realtime tiap 3 detik + history 10 scan terakhir
 *
 *  SH1106 SAFE ZONE:
 *   y=0..15  → ZONA KUNING, jangan taruh teks
 *   y=16..63 → aman, semua teks mulai x≥3
 * ============================================================
 */

#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <MAX30105.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <heartRate.h>
#include <spo2_algorithm.h>
#include <time.h>

// ─── PIN ───────────────────────────────────────────
#define SDA_PIN 4
#define SCL_PIN 5
#define BUTTON_PIN 14

// ─── OLED ──────────────────────────────────────────
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0);

// ─── WIFI NTP ──────────────────────────────────────
const char *STA_SSID = "NAMA_WIFI_ANDA"; // ← ganti
const char *STA_PASS = "PASSWORD_WIFI";  // ← ganti
const long GMT_OFFSET = 7 * 3600;        // WIB
#define NTP_TIMEOUT 30000UL

// ─── SOFT AP ───────────────────────────────────────
const char *AP_SSID = "JamSehat";
const char *AP_PASS = "12345678";
bool apActive = false;

// ─── WEB SERVER ────────────────────────────────────
ESP8266WebServer server(80);

// ─── SENSOR ────────────────────────────────────────
MAX30105 sensor;
bool sensorActive = false;

// ─── HISTORY HASIL SCAN ────────────────────────────
#define HIST_MAX 10
struct ScanResult {
  int hr;
  int spo2;
  char timeStr[9]; // "HH:MM:SS"
};
ScanResult scanHistory[HIST_MAX];
int historyCount = 0;
int historyHead = 0; // indeks ringbuffer

// ─── STATE ─────────────────────────────────────────
enum AppState { STATE_CLOCK, STATE_SCAN, STATE_RESULT, STATE_SLEEP };
AppState state = STATE_CLOCK;
unsigned long stateMs = 0;
unsigned long lastBtnMs = 0;

#define SCAN_MS 30000UL
#define RESULT_MS 30000UL
#define SLEEP_MS 60000UL

// ─── BUTTON ────────────────────────────────────────
#define DEBOUNCE_MS 200
unsigned long lastDebMs = 0;
bool lastRaw = HIGH;

// ─── SENSOR BUFFER ─────────────────────────────────
#define BUF_SIZE 100
uint32_t irBuf[BUF_SIZE];
uint32_t redBuf[BUF_SIZE];
int32_t bufIdx = 0;

// ─── MEDIAN ────────────────────────────────────────
#define MED_SIZE 5
int hrHist[MED_SIZE];
int o2Hist[MED_SIZE];
int histIdx = 0;
bool histFull = false;

int medianVal(int *a, int n) {
  int tmp[n];
  memcpy(tmp, a, n * sizeof(int));
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - 1 - i; j++)
      if (tmp[j] > tmp[j + 1]) {
        int t = tmp[j];
        tmp[j] = tmp[j + 1];
        tmp[j + 1] = t;
      }
  return tmp[n / 2];
}

// ─── BPM ───────────────────────────────────────────
const byte RATE_N = 8;
byte rates[RATE_N];
byte rateIdx = 0;
long lastBeat = 0;
int beatAvg = 0;
bool beatFlash = false;
bool beatOn = false;
unsigned long beatAnimMs = 0;

// ─── SPO2 ──────────────────────────────────────────
int32_t spo2Val = 0;
int8_t spo2Ok = 0;
int32_t hrAlgo = 0;
int8_t hrOk = 0;
int stableHR = 0;
int stableSpO2 = 0;

// ─── QUALITY ───────────────────────────────────────
int quality = 0; // 0=no finger 1=bad 2=fair 3=good 4=great

// ─── NTP CACHE ─────────────────────────────────────
time_t ntpBase = 0;
unsigned long msBase = 0;
bool ntpOk = false;

// ─── FORWARD DECL ──────────────────────────────────
void ntpConnect();
void apStart();
void apStop();
void wifiOff();
void sensorStart();
void sensorStop();
void sensorReset();
bool btn();
void processSensor(unsigned long now);
int calcQuality(uint32_t ir);
bool getTime(struct tm *t);
void drawClock();
void drawScan(unsigned long now);
void drawResult();
void heart(int cx, int cy, bool big);
void webRoot();
void webData();
String webHtml();
void saveResult();

// ══════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  u8g2.begin();

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x13_tr);
  u8g2.drawStr(3, 30, "JamSehat v4.1");
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(3, 46, "Sinkronisasi waktu...");
  u8g2.sendBuffer();

  ntpConnect();

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(3, 38, "Init sensor...");
  u8g2.sendBuffer();

  byte retry = 0;
  while (!sensor.begin(Wire, I2C_SPEED_FAST)) {
    delay(500);
    if (++retry > 10) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x13_tr);
      u8g2.drawStr(3, 32, "SENSOR ERROR");
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(3, 48, "Cek kabel SDA/SCL");
      u8g2.sendBuffer();
      while (1)
        delay(1000);
    }
  }
  sensorStop();
  sensorReset();

  // AP langsung ON dan tetap ON
  apStart();

  state = STATE_CLOCK;
  stateMs = millis();
  lastBtnMs = millis();

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x13_tr);
  u8g2.drawStr(3, 32, "SIAP!");
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(3, 50, "JamSehat  192.168.4.1");
  u8g2.sendBuffer();
  delay(1000);
}

// ══════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();
  bool pressed = btn();

  // AP selalu dilayani kecuali STATE_SLEEP
  if (apActive)
    server.handleClient();

  // ── SLEEP ──────────────────────────────────────
  if (state == STATE_SLEEP) {
    if (pressed) {
      u8g2.setPowerSave(0);
      apStart(); // hidupkan lagi AP
      state = STATE_CLOCK;
      stateMs = now;
      lastBtnMs = now;
    }
    return;
  }

  // ── Cek idle → sleep (berlaku di CLOCK & RESULT) ──
  if (state != STATE_SCAN) {
    if (now - lastBtnMs >= SLEEP_MS) {
      sensorStop();
      apStop(); // AP mati hanya saat sleep
      u8g2.clearBuffer();
      u8g2.sendBuffer();
      u8g2.setPowerSave(1);
      state = STATE_SLEEP;
      return;
    }
  }

  // ── CLOCK ──────────────────────────────────────
  if (state == STATE_CLOCK) {
    drawClock();
    if (pressed) {
      sensorReset();
      sensorStart();
      state = STATE_SCAN;
      stateMs = now;
      lastBtnMs = now;
    }
    return;
  }

  // ── SCAN ───────────────────────────────────────
  if (state == STATE_SCAN) {
    sensor.check();
    processSensor(now);
    drawScan(now);
    if (now - stateMs >= SCAN_MS) {
      sensorStop();
      saveResult(); // simpan ke history
      state = STATE_RESULT;
      stateMs = now;
      lastBtnMs = now;
    }
    return;
  }

  // ── RESULT ─────────────────────────────────────
  if (state == STATE_RESULT) {
    drawResult();
    if (now - stateMs >= RESULT_MS || pressed) {
      state = STATE_CLOCK;
      stateMs = now;
      lastBtnMs = now;
    }
    return;
  }
}

// ══════════════════════════════════════════════════
//  SIMPAN HASIL KE HISTORY
// ══════════════════════════════════════════════════
void saveResult() {
  struct tm t;
  ScanResult r;
  r.hr = stableHR;
  r.spo2 = stableSpO2;
  if (getTime(&t))
    strftime(r.timeStr, sizeof(r.timeStr), "%H:%M:%S", &t);
  else
    strcpy(r.timeStr, "--:--:--");

  scanHistory[historyHead] = r;
  historyHead = (historyHead + 1) % HIST_MAX;
  if (historyCount < HIST_MAX)
    historyCount++;
}

// ══════════════════════════════════════════════════
//  NTP
// ══════════════════════════════════════════════════
void ntpConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASS);
  unsigned long t0 = millis();

  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 >= NTP_TIMEOUT) {
      wifiOff();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(3, 32, "WiFi gagal.");
      u8g2.drawStr(3, 44, "Jam tanpa sinkron.");
      u8g2.sendBuffer();
      delay(1500);
      return;
    }
    delay(300);
  }

  configTime(GMT_OFFSET, 0, "pool.ntp.org", "time.nist.gov");
  struct tm t;
  unsigned long t1 = millis();
  while (!getLocalTime(&t) && millis() - t1 < 8000)
    delay(200);

  if (getLocalTime(&t)) {
    ntpBase = mktime(&t);
    msBase = millis();
    ntpOk = true;

    char buf[10];
    strftime(buf, sizeof(buf), "%H:%M:%S", &t);
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x13_tr);
    u8g2.drawStr(3, 30, "Waktu OK");
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(3, 48, buf);
    u8g2.sendBuffer();
    delay(700);
  }
  wifiOff();
}

void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(50);
}

// ══════════════════════════════════════════════════
//  SOFT AP
// ══════════════════════════════════════════════════
void apStart() {
  if (apActive)
    return;
  WiFi.forceSleepWake();
  delay(50);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  server.on("/", webRoot);
  server.on("/data", webData);
  server.begin();
  apActive = true;
  Serial.println("[AP] ON " + WiFi.softAPIP().toString());
}

void apStop() {
  if (!apActive)
    return;
  server.stop();
  WiFi.softAPdisconnect(true);
  wifiOff();
  apActive = false;
  Serial.println("[AP] OFF");
}

// ══════════════════════════════════════════════════
//  WAKTU
// ══════════════════════════════════════════════════
bool getTime(struct tm *t) {
  if (!ntpOk)
    return false;
  time_t est = ntpBase + (millis() - msBase) / 1000UL;
  localtime_r(&est, t);
  return true;
}

// ══════════════════════════════════════════════════
//  WEB
// ══════════════════════════════════════════════════
void webRoot() { server.send(200, "text/html; charset=utf-8", webHtml()); }

void webData() {
  struct tm t;
  char ts[9] = "--:--:--";
  if (getTime(&t))
    strftime(ts, sizeof(ts), "%H:%M:%S", &t);

  const char *st = (state == STATE_SCAN)     ? "scan"
                   : (state == STATE_RESULT) ? "result"
                   : (state == STATE_SLEEP)  ? "sleep"
                                             : "clock";
  const char *qw[] = {"Gagal", "Jelek", "Cukup", "Baik", "Bagus"};
  int q = constrain(quality, 0, 4);

  // Buat JSON dengan history
  String json = "{";
  json += "\"hr\":" + String(stableHR);
  json += ",\"spo2\":" + String(stableSpO2);
  json += ",\"quality\":" + String(q);
  json += ",\"quality_text\":\"" + String(qw[q]) + "\"";
  json += ",\"state\":\"" + String(st) + "\"";
  json += ",\"time\":\"" + String(ts) + "\"";
  json += ",\"history\":[";
  for (int i = 0; i < historyCount; i++) {
    // Ambil dari ringbuffer, urutan terbaru dulu
    int idx = ((historyHead - 1 - i) + HIST_MAX) % HIST_MAX;
    if (i > 0)
      json += ",";
    json += "{\"hr\":" + String(scanHistory[idx].hr);
    json += ",\"spo2\":" + String(scanHistory[idx].spo2);
    json += ",\"time\":\"" + String(scanHistory[idx].timeStr) + "\"}";
  }
  json += "]}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

String webHtml() {
  struct tm t;
  char ts[9] = "--:--:--";
  if (getTime(&t))
    strftime(ts, sizeof(ts), "%H:%M:%S", &t);

  const char *qw[] = {"Gagal", "Jelek", "Cukup", "Baik", "Bagus"};
  const char *qc[] = {"#e74c3c", "#e67e22", "#f1c40f", "#27ae60", "#1abc9c"};
  int q = constrain(quality, 0, 4);

  String s2col = "";
  if (stableSpO2 >= 95)
    s2col = "color:#27ae60";
  else if (stableSpO2 >= 90)
    s2col = "color:#f39c12";
  else if (stableSpO2 > 0)
    s2col = "color:#e74c3c";

  String h = F(
      "<!DOCTYPE html><html><head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>JamSehat</title>"
      "<style>"
      "body{font-family:sans-serif;background:#0d0d0d;color:#eee;"
      "text-align:center;padding:20px;margin:0}"
      "h1{font-size:1.3em;margin:0 0 2px;letter-spacing:2px}"
      ".sub{font-size:.75em;color:#555;margin-bottom:18px}"
      ".card{background:#1a1a1a;border-radius:16px;padding:20px;"
      "margin:10px auto;max-width:300px}"
      ".val{font-size:3.5em;font-weight:700;line-height:1}"
      ".unit{font-size:.85em;color:#777;margin-top:3px}"
      ".lbl{font-size:.7em;color:#444;margin-top:8px;text-transform:uppercase;"
      "letter-spacing:1px}"
      ".badge{display:inline-block;padding:4px 14px;border-radius:20px;"
      "font-size:.8em;margin-top:8px}"
      ".prog{width:80%;height:5px;background:#222;border-radius:3px;"
      "margin:8px auto 0;overflow:hidden}"
      ".bar{height:100%;border-radius:3px;transition:width .5s}"
      "table{width:100%;border-collapse:collapse;margin-top:8px;font-size:."
      "85em}"
      "th{color:#555;font-weight:400;padding:4px 6px;border-bottom:1px solid "
      "#222}"
      "td{padding:5px 6px;border-bottom:1px solid #1a1a1a;color:#ccc}"
      "tr:first-child td{color:#fff;font-weight:600}"
      ".foot{font-size:.65em;color:#333;margin-top:18px}"
      "</style>"
      "<script>"
      "function upd(){"
      "fetch('/data').then(r=>r.json()).then(d=>{"
      "document.getElementById('hr').textContent=d.hr>0?d.hr:'--';"
      "document.getElementById('o2').textContent=d.spo2>0?d.spo2:'--';"
      "document.getElementById('qt').textContent=d.quality_text;"
      "document.getElementById('st').textContent="
      "d.state==='scan'?'Sedang scan...':"
      "d.state==='result'?'Hasil scan':"
      "d.state==='sleep'?'Standby':'Menampilkan jam';"
      "document.getElementById('ts').textContent=d.time;"
      "document.getElementById('qb').style.width=(d.quality*25)+'%';"
      // Update history tabel
      "var tb=document.getElementById('hrows');"
      "tb.innerHTML='';"
      "if(d.history&&d.history.length){"
      "d.history.forEach(function(r,i){"
      "tb.innerHTML+='<tr><td>'+r.time+'</td>"
      "<td>'+(r.hr>0?r.hr+' BPM':'--')+'</td>"
      "<td>'+(r.spo2>0?r.spo2+'%':'--')+'</td></tr>';"
      "});"
      "}else{"
      "tb.innerHTML='<tr><td colspan=3 style=color:#333>Belum ada "
      "scan</td></tr>';"
      "}"
      "}).catch(()=>{});"
      "setTimeout(upd,3000);"
      "}"
      "window.onload=upd;"
      "</script>"
      "</head><body>");

  h += "<h1>&#9829; JamSehat</h1>";
  h += "<p class='sub' id='st'>";
  h += (state == STATE_SCAN)     ? "Sedang scan..."
       : (state == STATE_RESULT) ? "Hasil scan"
                                 : "Menampilkan jam";
  h += "</p>";

  // HR card
  h += "<div class='card'>";
  h += "<div class='val' id='hr'>";
  h += stableHR > 0 ? String(stableHR) : "--";
  h += "</div><div class='unit'>BPM</div>";
  h += "<div class='lbl'>Detak Jantung</div></div>";

  // SpO2 card
  h += "<div class='card'>";
  h += "<div class='val' id='o2' style='";
  h += s2col;
  h += "'>";
  h += stableSpO2 > 0 ? String(stableSpO2) : "--";
  h += "</div><div class='unit'>%</div>";
  h += "<div class='lbl'>Saturasi Oksigen SpO2</div></div>";

  // Quality card
  h += "<div class='card'>";
  h += "<div class='lbl'>Kualitas Sinyal</div>";
  h += "<div class='badge' id='qt' style='background:";
  h += qc[q];
  h += ";color:#fff'>";
  h += qw[q];
  h += "</div>";
  h += "<div class='prog'><div class='bar' id='qb' style='background:";
  h += qc[q];
  h += ";width:";
  h += String(q * 25);
  h += "%'></div></div></div>";

  // History card
  h += "<div class='card'>";
  h += "<div class='lbl'>Riwayat Scan (10 terakhir)</div>";
  h += "<table><thead><tr><th>Waktu</th><th>HR</th><th>SpO2</th></tr></thead>";
  h += "<tbody id='hrows'>";
  if (historyCount == 0) {
    h += "<tr><td colspan='3' style='color:#333'>Belum ada scan</td></tr>";
  } else {
    for (int i = 0; i < historyCount; i++) {
      int idx = ((historyHead - 1 - i) + HIST_MAX) % HIST_MAX;
      h += "<tr><td>" + String(scanHistory[idx].timeStr) + "</td>";
      h += "<td>" +
           (scanHistory[idx].hr > 0 ? String(scanHistory[idx].hr) + " BPM"
                                    : String("--")) +
           "</td>";
      h += "<td>" +
           (scanHistory[idx].spo2 > 0 ? String(scanHistory[idx].spo2) + "%"
                                      : String("--")) +
           "</td></tr>";
    }
  }
  h += "</tbody></table></div>";

  h += "<p class='foot'>Waktu: <span id='ts'>";
  h += ts;
  h += "</span> &nbsp;|&nbsp; ";
  h += "<a href='/data' style='color:#2980b9'>JSON</a></p>";
  h += "</body></html>";
  return h;
}

// ══════════════════════════════════════════════════
//  QUALITY
//  Threshold IR diturunkan ke 30000 agar tetap detect
//  meski ada celah casing ~1mm
// ══════════════════════════════════════════════════
int calcQuality(uint32_t ir) {
  static long mn = 999999, mx = 0;
  static byte c = 0;
  if (ir < mn)
    mn = ir;
  if (ir > mx)
    mx = ir;
  if (++c >= 25) {
    long rng = mx - mn;
    long dc = (mx + mn) / 2;
    mn = ir;
    mx = ir;
    c = 0;
    if (dc < 30000)
      return 0; // ← diturunkan dari 50000 → 30000
    float r = (float)rng / dc;
    if (r < 0.005)
      return 1;
    if (r < 0.02)
      return 2;
    if (r < 0.08)
      return 4;
    return 3;
  }
  return quality;
}

// ══════════════════════════════════════════════════
//  SENSOR
//  IR threshold checkForBeat diturunkan ke 30000
//  Pulse amplitude dinaikkan ke 0x3F untuk sinyal lebih kuat
// ══════════════════════════════════════════════════
void processSensor(unsigned long now) {
  while (sensor.available()) {
    uint32_t ir = sensor.getFIFOIR();
    uint32_t red = sensor.getFIFORed();

    quality = calcQuality(ir);

    // ← Threshold IR diturunkan: 50000 → 30000
    if (ir > 30000 && checkForBeat(ir)) {
      long dt = now - lastBeat;
      lastBeat = now;
      float bpm = 60000.0f / dt;
      if (bpm > 40 && bpm < 200) {
        if (!(beatAvg > 0 && abs((int)bpm - beatAvg) > 35)) {
          rates[rateIdx++] = (byte)bpm;
          rateIdx %= RATE_N;
          int sum = 0;
          for (byte i = 0; i < RATE_N; i++)
            sum += rates[i];
          beatAvg = sum / RATE_N;
          hrHist[histIdx] = beatAvg;
          if (beatAvg > 40)
            stableHR = histFull ? medianVal(hrHist, MED_SIZE) : beatAvg;
          beatFlash = true;
        }
      }
    }

    irBuf[bufIdx] = ir;
    redBuf[bufIdx] = red;
    if (++bufIdx >= BUF_SIZE) {
      bufIdx = 0;
      histIdx = (histIdx + 1) % MED_SIZE;
      if (histIdx == 0)
        histFull = true;
      if (now - stateMs < 5000UL) {
        sensor.nextSample();
        continue;
      }

      maxim_heart_rate_and_oxygen_saturation(irBuf, BUF_SIZE, redBuf, &spo2Val,
                                             &spo2Ok, &hrAlgo, &hrOk);

      // ← quality threshold diturunkan: >= 2 tetap, tapi IR check 30000
      if (spo2Ok && spo2Val >= 85 && spo2Val <= 100 && quality >= 2) {
        o2Hist[histIdx] = spo2Val;
        int raw = histFull ? medianVal(o2Hist, MED_SIZE) : spo2Val;
        stableSpO2 = (stableSpO2 == 0) ? raw : (stableSpO2 * 6 + raw * 4) / 10;
      }
      if (hrOk && hrAlgo >= 40 && hrAlgo <= 200 && stableHR == 0 &&
          quality >= 2)
        stableHR = hrAlgo;
    }
    sensor.nextSample();
  }
  if (beatFlash) {
    beatOn = true;
    beatFlash = false;
    beatAnimMs = millis();
  }
  if (beatOn && millis() - beatAnimMs > 150)
    beatOn = false;
}

void sensorStart() {
  if (sensorActive)
    return;
  // Amplitude dinaikkan ke 0x3F (dari 0x1F) → sinyal lebih kuat untuk casing
  sensor.setup(0x7F, 4, 2, 100, 411, 4096);
  sensor.setPulseAmplitudeRed(0x3F); // ← naik dari 0x1F
  sensor.setPulseAmplitudeIR(0x3F);  // ← naik dari 0x1F
  sensor.setPulseAmplitudeGreen(0);
  bufIdx = 0;
  sensorActive = true;
}

void sensorStop() {
  sensor.setPulseAmplitudeRed(0);
  sensor.setPulseAmplitudeIR(0);
  sensor.setPulseAmplitudeGreen(0);
  sensorActive = false;
}

void sensorReset() {
  memset(irBuf, 0, sizeof(irBuf));
  memset(redBuf, 0, sizeof(redBuf));
  memset(rates, 0, sizeof(rates));
  memset(hrHist, 0, sizeof(hrHist));
  memset(o2Hist, 0, sizeof(o2Hist));
  bufIdx = 0;
  histIdx = 0;
  histFull = false;
  rateIdx = 0;
  lastBeat = 0;
  beatAvg = 0;
  beatFlash = false;
  beatOn = false;
  stableHR = 0;
  stableSpO2 = 0;
  spo2Val = 0;
  spo2Ok = 0;
  hrAlgo = 0;
  hrOk = 0;
  quality = 0;
}

// ══════════════════════════════════════════════════
//  BUTTON
// ══════════════════════════════════════════════════
bool btn() {
  unsigned long now = millis();
  bool raw = digitalRead(BUTTON_PIN);
  if (raw == LOW && lastRaw == HIGH && now - lastDebMs >= DEBOUNCE_MS) {
    lastDebMs = now;
    lastBtnMs = now;
    lastRaw = raw;
    return true;
  }
  lastRaw = raw;
  return false;
}

// ══════════════════════════════════════════════════
//  IKON JANTUNG
// ══════════════════════════════════════════════════
void heart(int cx, int cy, bool big) {
  int r = big ? 5 : 4;
  u8g2.drawDisc(cx - r, cy, r);
  u8g2.drawDisc(cx + r + 1, cy, r);
  u8g2.drawTriangle(cx - r * 2, cy + 1, cx + r * 2 + 2, cy + 1, cx, cy + r + 7);
}

// ══════════════════════════════════════════════════
//  DRAW: JAM (lebih kecil dari v4)
//
//  Perubahan:
//  - Jam pakai font fub20_tn (dari fub30_tn) → lebih compact
//  - Detik tetap 6x10_tr
//  - Tanggal tetap 6x13_tr di bawah
//  - Ditambah info AP aktif (titik kecil di pojok kanan atas)
//
//  Layout:
//  y=16  garis pemisah
//  y=44  HH:MM  font fub20_tn, tengah
//  y=44  SS     font 6x10_tr, sejajar kanan
//  y=62  tanggal tengah
// ══════════════════════════════════════════════════
void drawClock() {
  u8g2.setPowerSave(0);

  struct tm t;
  char hm[6] = "--:--";
  char ss[3] = "--";
  char date[18] = "";

  if (getTime(&t)) {
    strftime(hm, sizeof(hm), "%H:%M", &t);
    strftime(ss, sizeof(ss), "%S", &t);
    strftime(date, sizeof(date), "%a %d/%m/%Y", &t);
  }

  u8g2.clearBuffer();

  // Garis zona kuning
  u8g2.drawHLine(0, 16, 128);

  // ── HH:MM lebih kecil: font fub20_tn ──
  u8g2.setFont(u8g2_font_fub20_tn);
  int hmW = u8g2.getStrWidth(hm);
  // Geser sedikit ke kiri agar ada ruang untuk detik di kanan
  int hmX = (128 - hmW) / 2 - 8;
  if (hmX < 3)
    hmX = 3;
  u8g2.drawStr(hmX, 44, hm);

  // ── Detik kecil sejajar baseline HH:MM ──
  u8g2.setFont(u8g2_font_6x10_tr);
  int ssW = u8g2.getStrWidth(ss);
  int ssX = hmX + hmW + 4;
  if (ssX + ssW > 125)
    ssX = 125 - ssW;
  u8g2.drawStr(ssX, 44, ss);

  // ── Tanggal di bawah ──
  u8g2.setFont(u8g2_font_5x7_tr);
  int dw = u8g2.getStrWidth(date);
  int dx = (128 - dw) / 2;
  if (dx < 3)
    dx = 3;
  u8g2.drawStr(dx, 58, date);

  // ── Indikator AP aktif (titik kecil pojok kanan atas, zona kuning ok) ──
  // Label kecil "AP" di y=10, x=110 (zona kuning, tapi hanya indikator kecil)
  // u8g2.setFont(u8g2_font_4x6_tr);
  // u8g2.drawStr(104, 12, "AP:ON");

  u8g2.sendBuffer();
}

// ══════════════════════════════════════════════════
//  DRAW: SCAN
// ══════════════════════════════════════════════════
void drawScan(unsigned long now) {
  u8g2.clearBuffer();

  int sisa = (int)((SCAN_MS - (now - stateMs)) / 1000);
  if (sisa < 0)
    sisa = 0;
  bool warmup = (now - stateMs < 5000UL);

  u8g2.setFont(u8g2_font_6x13_tr);
  char hdr[12];
  sprintf(hdr, "SCAN %ds", sisa);
  u8g2.drawStr(3, 27, hdr);

  heart(114, 22, beatOn);

  for (int i = 0; i < 4; i++) {
    int bx = 3 + i * 13;
    if (i < quality)
      u8g2.drawBox(bx, 29, 10, 5);
    else
      u8g2.drawFrame(bx, 29, 10, 5);
  }
  u8g2.setFont(u8g2_font_5x7_tr);
  const char *ql[] = {"no finger", "bad", "fair", "good", "great"};
  u8g2.drawStr(58, 35, ql[constrain(quality, 0, 4)]);

  char ln[20];
  if (stableHR > 0)
    sprintf(ln, "HR : %d BPM", stableHR);
  else if (warmup)
    sprintf(ln, "HR : warmup...");
  else
    sprintf(ln, "HR : --");
  u8g2.drawStr(3, 46, ln);

  if (stableSpO2 > 0)
    sprintf(ln, "O2 : %d%%", stableSpO2);
  else if (warmup)
    sprintf(ln, "O2 : warmup...");
  else
    sprintf(ln, "O2 : --");
  u8g2.drawStr(3, 57, ln);

  int bw = (int)map(now - stateMs, 0, SCAN_MS, 0, 128);
  u8g2.drawBox(0, 61, constrain(bw, 0, 128), 3);

  u8g2.sendBuffer();
}

// ══════════════════════════════════════════════════
//  DRAW: HASIL (lebih terpusat)
//
//  Layout baru — nilai HR dan SpO2 besar di tengah:
//
//  y=16  garis
//  y=26  "HASIL SCAN"  font 6x13_tr, tengah
//  y=30  garis
//
//  y=34  "HR"          font 5x7_tr, tengah
//  y=46  nilai HR besar font fub14_tn (atau 6x13), tengah
//  y=50  "BPM"         font 4x6_tr, tengah
//
//  y=53  garis tipis
//
//  y=57  "SpO2"        font 5x7_tr, tengah
//  y=63  nilai SpO2     font 5x7_tr, tengah  (+ % dan countdown kanan)
//
//  Nilai rata-tengah, tidak pinggiran
// ══════════════════════════════════════════════════
void drawResult() {
  u8g2.clearBuffer();
  u8g2.drawHLine(0, 16, 128);

  // ── Judul tengah ──
  u8g2.setFont(u8g2_font_6x13_tr);
  const char *ttl = "HASIL SCAN";
  int tw = u8g2.getStrWidth(ttl);
  u8g2.drawStr((128 - tw) / 2, 26, ttl);
  u8g2.drawHLine(0, 30, 128);

  // ── Label "HR" tengah ──
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr((128 - u8g2.getStrWidth("HEART RATE")) / 2, 38, "HEART RATE");

  // ── Nilai HR besar, tengah ──
  char valHR[12];
  if (stableHR > 0)
    sprintf(valHR, "%d BPM", stableHR);
  else
    strcpy(valHR, "-- BPM");
  u8g2.setFont(u8g2_font_6x13_tr);
  int vwHR = u8g2.getStrWidth(valHR);
  u8g2.drawStr((128 - vwHR) / 2, 50, valHR);

  // ── Garis pemisah ──
  u8g2.drawHLine(8, 53, 112);

  // ── SpO2 di bawah, dalam 1 baris tengah ──
  char valO2[14];
  if (stableSpO2 > 0)
    sprintf(valO2, "SpO2: %d%%", stableSpO2);
  else
    strcpy(valO2, "SpO2: --");
  u8g2.setFont(u8g2_font_6x13_tr);
  int vwO2 = u8g2.getStrWidth(valO2);
  u8g2.drawStr((128 - vwO2) / 2, 63, valO2);

  // ── Countdown pojok kiri bawah ──
  unsigned long elapsed = millis() - stateMs;
  unsigned long sisa = 0;
  if (elapsed < RESULT_MS)
    sisa = (RESULT_MS - elapsed) / 1000;
  char cntStr[6];
  sprintf(cntStr, "%lus", sisa);
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(3, 63, cntStr);

  u8g2.sendBuffer();
}
