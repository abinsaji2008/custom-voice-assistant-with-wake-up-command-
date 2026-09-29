// ============================================================================
// V5.0 - ESP32-S3 AI Voice Assistant
// WakeNet9 "Hi ESP" + Groq Whisper streaming
//
// DESIGN REASONING
// Goal: establish V2 from the proven final V1 baseline without changing the
// working hardware/audio protocol.
//
// Kept stable:
// - ESP32-S3-WROOM-1 N16R8
// - INMP441: SD=16, SCK=17, WS=15
// - WakeNet9: wn9_hiesp / "Hi ESP"
// - Groq: whisper-large-v3-turbo
// - 16 kHz mono PCM streaming
// - 2-second silence stop
// - LED OFF while waiting, BLUE blink while recording, RED for 2 seconds
//
// V2 rule: change one major behavior at a time and record the reason/result.
// ============================================================================

#define ASSISTANT_VERSION "V5.0"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP_I2S.h>
#include "model_path.h"
#include "esp_wn_models.h"
#include "esp_wn_iface.h"
#include "secrets.h"

#define SR 16000
#define MAXSEC 10
#define SILENCE 2000
#define WAITVOICE 4000
#define THRESH 700
#define GAIN 8

#define SD 16
#define SCK 17
#define WS 15

#define HOST "api.groq.com"
#define MODEL "whisper-large-v3-turbo"
#define BOUNDARY "----esp32"

I2SClass mic;
WiFiClientSecure net;

srmodel_list_t *models;
const esp_wn_iface_t *wn;
model_iface_data_t *wnd;
char *wnName;
int chunkSize;

float hpA, hpY1, hpX1, hpY2, hpX2;


// ================= LED =================

void ledOff() {
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
}

void ledBlue() {
  rgbLedWrite(RGB_BUILTIN, 0, 0, 40);
}

void ledRed() {
  rgbLedWrite(RGB_BUILTIN, 60, 0, 0);
}

void blinkRecordingLED() {
  static uint32_t t = 0;
  static bool state = false;

  if (millis() - t >= 300) {
    t = millis();
    state = !state;

    if (state)
      ledBlue();
    else
      ledOff();
  }
}


// ================= AUDIO =================

void filterReset() {
  float rc = 1.0f / (2 * PI * 120);
  hpA = rc / (rc + 1.0f / SR);
  hpY1 = hpX1 = hpY2 = hpX2 = 0;
}

uint32_t filterAudio(int16_t *s, int n) {
  uint32_t peak = 0;

  for (int i = 0; i < n; i++) {
    float v = s[i];

    float p = hpA * (hpY1 + v - hpX1);
    hpX1 = v;
    hpY1 = p;

    float q = hpA * (hpY2 + p - hpX2);
    hpX2 = p;
    hpY2 = q;

    uint32_t m = fabsf(q);
    if (m > peak) peak = m;

    int32_t z = q * GAIN;

    if (z > 32767) z = 32767;
    if (z < -32768) z = -32768;

    s[i] = z;
  }

  return peak;
}


// ================= GROQ =================

bool groqConnect() {
  if (net.connected())
    return true;

  net.setInsecure();
  net.setTimeout(20);

  return net.connect(HOST, 443);
}

bool sendChunk(const uint8_t *p, size_t n) {
  char h[16];

  int m = snprintf(
    h,
    sizeof(h),
    "%X\r\n",
    (unsigned)n
  );

  return
    net.write((uint8_t*)h, m) == m &&
    net.write(p, n) == n &&
    net.write((uint8_t*)"\r\n", 2) == 2;
}

void wavHeader(uint8_t *h) {
  uint32_t u = 0xFFFFFFFF;
  uint32_t r = SR;
  uint32_t br = SR * 2;
  uint32_t fl = 16;

  uint16_t pcm = 1;
  uint16_t ch = 1;
  uint16_t ba = 2;
  uint16_t bits = 16;

  memcpy(h, "RIFF", 4);
  memcpy(h + 4, &u, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  memcpy(h + 16, &fl, 4);
  memcpy(h + 20, &pcm, 2);
  memcpy(h + 22, &ch, 2);
  memcpy(h + 24, &r, 4);
  memcpy(h + 28, &br, 4);
  memcpy(h + 32, &ba, 2);
  memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &u, 4);
}

String readLine() {
  String s;

  while (net.connected()) {
    if (!net.available()) {
      delay(2);
      continue;
    }

    char c = net.read();

    if (c == '\n')
      break;

    if (c != '\r')
      s += c;
  }

  return s;
}

String groqRead() {
  int status = 0;
  bool chunked = false;
  String body;

  while (1) {
    String l = readLine();

    if (!l.length())
      break;

    if (l.startsWith("HTTP/"))
      status = l.substring(9, 12).toInt();

    l.toLowerCase();

    if (
      l.startsWith("transfer-encoding:") &&
      l.indexOf("chunked") >= 0
    )
      chunked = true;
  }

  if (chunked) {
    while (1) {
      long n =
        strtol(
          readLine().c_str(),
          0,
          16
        );

      if (n <= 0)
        break;

      while (n--) {
        while (!net.available())
          delay(1);

        body += (char)net.read();
      }

      readLine();
    }
  }
  else {
    while (net.connected() || net.available()) {
      if (net.available())
        body += (char)net.read();
      else
        delay(2);
    }
  }

  Serial.printf(
    "[GROQ] HTTP %d\n",
    status
  );

  return body;
}


// ================= TRANSCRIBE =================

void transcribe() {

  if (!groqConnect()) {
    Serial.println("[GROQ] Connect failed");
    ledOff();
    return;
  }

  net.print(
    String(
      "POST /openai/v1/audio/transcriptions HTTP/1.1\r\n"
      "Host: "
    ) +
    HOST +
    "\r\nAuthorization: Bearer " +
    SEED_GROQ_KEY +
    "\r\nUser-Agent: ESP32-S3\r\n"
    "Content-Type: multipart/form-data; boundary=" BOUNDARY
    "\r\nTransfer-Encoding: chunked\r\n"
    "Connection: close\r\n\r\n"
  );

  const char *form =
    "--" BOUNDARY "\r\n"
    "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
    MODEL "\r\n"

    "--" BOUNDARY "\r\n"
    "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
    "en\r\n"

    "--" BOUNDARY "\r\n"
    "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"
    "text\r\n"

    "--" BOUNDARY "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";

  sendChunk(
    (uint8_t*)form,
    strlen(form)
  );

  uint8_t wav[44];

  wavHeader(wav);

  sendChunk(
    wav,
    44
  );

  Serial.println("[REC] Speak now...");

  filterReset();

  uint8_t raw[1024];

  uint32_t start = millis();
  uint32_t lastVoice = start;

  bool speech = false;

  while (
    millis() - start <
    MAXSEC * 1000UL
  ) {

    // BLUE BLINK = RECORDING
    blinkRecordingLED();

    size_t n =
      mic.readBytes(
        (char*)raw,
        sizeof(raw)
      );

    if (!n)
      continue;

    uint32_t peak =
      filterAudio(
        (int16_t*)raw,
        n / 2
      );

    if (peak > THRESH) {
      speech = true;
      lastVoice = millis();
    }

    if (!sendChunk(raw, n))
      break;

    // Wait for first speech
    if (
      !speech &&
      millis() - start >= WAITVOICE
    ) {
      Serial.println(
        "[REC] No speech"
      );
      break;
    }

    // 2 seconds continuous silence
    if (
      speech &&
      millis() - lastVoice >= SILENCE
    ) {
      Serial.println(
        "[REC] 2s silence -> STOP"
      );
      break;
    }
  }

  // Recording finished
  ledOff();

  // RED = recording finished
  ledRed();
  delay(2000);
  ledOff();


  // Finish multipart upload
  String end =
    String("\r\n--") +
    BOUNDARY +
    "--\r\n";

  sendChunk(
    (uint8_t*)end.c_str(),
    end.length()
  );

  net.write(
    (uint8_t*)"0\r\n\r\n",
    5
  );


  Serial.println(
    "[GROQ] Waiting..."
  );

  String text =
    groqRead();

  net.stop();

  text.trim();

  Serial.println();
  Serial.println(
    "================================"
  );

  Serial.println(
    "YOU SAID:"
  );

  Serial.println(text);

  Serial.println(
    "================================"
  );
}


// ================= WAKE WORD =================

bool wakeInit() {

  models =
    esp_srmodel_init("model");

  if (!models)
    return false;


  wnName =
    esp_srmodel_filter(
      models,
      ESP_WN_PREFIX,
      "hiesp"
    );

  if (!wnName)
    return false;


  wn =
    esp_wn_handle_from_name(
      wnName
    );

  if (!wn)
    return false;


  wnd =
    wn->create(
      wnName,
      DET_MODE_90
    );

  if (!wnd)
    return false;


  chunkSize =
    wn->get_samp_chunksize(
      wnd
    );


  Serial.printf(
    "[WAKE] Model: %s\n",
    wnName
  );

  Serial.printf(
    "[WAKE] Chunk: %d\n",
    chunkSize
  );

  return true;
}


bool waitWake() {

  static int16_t buf[512];

  while (1) {

    // LED stays OFF while waiting

    size_t n =
      mic.readBytes(
        (char*)buf,
        chunkSize * 2
      );

    if (
      n !=
      chunkSize * 2
    )
      continue;


    if (
      wn->detect(
        wnd,
        buf
      ) > 0
    ) {
      ledOff();
      return true;
    }
  }
}


// ================= SETUP =================

void setup() {

  Serial.begin(115200);

  delay(1000);

  ledOff();


  WiFi.begin(
    SEED_WIFI_SSID,
    SEED_WIFI_PASS
  );

  Serial.print(
    "[WiFi] "
  );

  while (
    WiFi.status() !=
    WL_CONNECTED
  ) {
    Serial.print(".");
    delay(300);
  }

  Serial.println();


  mic.setPins(
    SCK,
    WS,
    -1,
    SD,
    -1
  );


  if (
    !mic.begin(
      I2S_MODE_STD,
      SR,
      I2S_DATA_BIT_WIDTH_32BIT,
      I2S_SLOT_MODE_MONO
    ) ||

    !mic.configureRX(
      SR,
      I2S_DATA_BIT_WIDTH_32BIT,
      I2S_SLOT_MODE_MONO,
      I2S_RX_TRANSFORM_32_TO_16
    )
  ) {

    Serial.println(
      "[MIC] FAILED"
    );

    while (1)
      delay(1000);
  }


  if (!wakeInit()) {

    Serial.println(
      "[WAKE] FAILED"
    );

    while (1)
      delay(1000);
  }


  groqConnect();


  Serial.println();
  Serial.println(
    "READY"
  );

  Serial.println(
    "Say: HI ESP"
  );
}


// ================= LOOP =================

void loop() {

  if (waitWake()) {

    Serial.println();
    Serial.println(
      "HI ESP DETECTED!"
    );

    // LED blinking starts ONLY now
    transcribe();

    // Make absolutely sure LED is off
    ledOff();

    Serial.println();
    Serial.println(
      "Waiting for HI ESP..."
    );
  }
}