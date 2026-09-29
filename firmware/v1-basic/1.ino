#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "ESP_I2S.h"

#include "model_path.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"

#include "secrets.h"

// ============================================================
// INMP441
// ============================================================
#define PIN_MIC_SCK  17
#define PIN_MIC_WS   15
#define PIN_MIC_SD   16

#define SAMPLE_RATE 16000

// ============================================================
// GROQ
// ============================================================
#define GROQ_HOST   "api.groq.com"
#define GROQ_PORT   443
#define GROQ_PATH   "/openai/v1/audio/transcriptions"

#define STT_MODEL   "whisper-large-v3-turbo"
#define BOUNDARY    "----ESP32WakeSTT"

// Maximum recording after "Hi ESP"
#define MAX_RECORD_SECONDS 10

// Don't stop before this
#define MIN_RECORD_MS 800

// Silence detection
#define SILENCE_PEAK 350
#define SILENCE_HOLD_MS 1200

// ============================================================
// I2S
// ============================================================
I2SClass mic;

// ============================================================
// WiFi / TLS
// ============================================================
WiFiClientSecure net;

// ============================================================
// WakeNet
// ============================================================
srmodel_list_t *models = nullptr;

const esp_wn_iface_t *wakenet = nullptr;
model_iface_data_t *wakeData = nullptr;

char *wakeModelName = nullptr;

int wakeChunkSize = 0;

// ============================================================
// AUDIO FILTER
// Taken from your streaming STT architecture
// ============================================================
#define HP_CUTOFF_HZ 120
#define MIC_GAIN     8

static float hpA;
static float hpY1;
static float hpX1;
static float hpY2;
static float hpX2;

void filterReset()
{
    float rc =
        1.0f / (2.0f * PI * HP_CUTOFF_HZ);

    float dt =
        1.0f / SAMPLE_RATE;

    hpA =
        rc / (rc + dt);

    hpY1 = 0;
    hpX1 = 0;
    hpY2 = 0;
    hpX2 = 0;
}

void filterBlock(
    int16_t *samples,
    size_t count
)
{
    for (size_t i = 0; i < count; i++)
    {
        float x = samples[i];

        float a =
            hpA * (hpY1 + x - hpX1);

        hpX1 = x;
        hpY1 = a;

        float b =
            hpA * (hpY2 + a - hpX2);

        hpX2 = a;
        hpY2 = b;

        int32_t v =
            (int32_t)(b * MIC_GAIN);

        if (v > 32767)
            v = 32767;

        if (v < -32768)
            v = -32768;

        samples[i] =
            (int16_t)v;
    }
}

// ============================================================
// CONNECT TO GROQ
// Keep connection alive while idle so wake response is fast.
// ============================================================
bool ensureGroqLink()
{
    if (net.connected())
        return true;

    net.setInsecure();
    net.setTimeout(20);

    Serial.println("[GROQ] Connecting...");

    if (!net.connect(GROQ_HOST, GROQ_PORT))
    {
        Serial.println("[GROQ] Connection failed");
        return false;
    }

    Serial.println("[GROQ] Connected");

    return true;
}

// ============================================================
// SEND ONE HTTP CHUNK
// ============================================================
bool sendChunk(
    const uint8_t *data,
    size_t len
)
{
    char header[20];

    int headerLen =
        snprintf(
            header,
            sizeof(header),
            "%X\r\n",
            (unsigned)len
        );

    if (
        net.write(
            (const uint8_t *)header,
            headerLen
        ) != (size_t)headerLen
    )
        return false;

    if (
        net.write(
            data,
            len
        ) != len
    )
        return false;

    if (
        net.write(
            (const uint8_t *)"\r\n",
            2
        ) != 2
    )
        return false;

    return true;
}

// ============================================================
// WAV HEADER
// Unknown length because we are streaming live.
// ============================================================
void writeWavHeader(
    uint8_t *h
)
{
    const uint32_t UNKNOWN =
        0xFFFFFFFF;

    const uint32_t rate =
        SAMPLE_RATE;

    const uint32_t byteRate =
        rate * 2;

    const uint32_t fmtLen =
        16;

    const uint16_t pcm =
        1;

    const uint16_t channels =
        1;

    const uint16_t blockAlign =
        2;

    const uint16_t bits =
        16;

    memcpy(h + 0, "RIFF", 4);
    memcpy(h + 4, &UNKNOWN, 4);

    memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, &fmtLen, 4);

    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &channels, 2);

    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byteRate, 4);

    memcpy(h + 32, &blockAlign, 2);
    memcpy(h + 34, &bits, 2);

    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &UNKNOWN, 4);
}

// ============================================================
// READ ONE HTTP LINE
// ============================================================
String readLine(
    uint32_t deadline
)
{
    String s;

    while (millis() < deadline)
    {
        if (!net.available())
        {
            if (!net.connected())
                break;

            delay(2);
            continue;
        }

        char ch =
            net.read();

        if (ch == '\n')
            break;

        if (ch != '\r')
            s += ch;
    }

    return s;
}

// ============================================================
// READ BYTES
// ============================================================
void readBytesToString(
    String &out,
    long n,
    uint32_t deadline
)
{
    while (
        n > 0 &&
        millis() < deadline
    )
    {
        if (!net.available())
        {
            if (!net.connected())
                break;

            delay(2);
            continue;
        }

        out +=
            (char)net.read();

        n--;
    }
}

// ============================================================
// READ GROQ RESPONSE
// Handles chunked HTTP response.
// ============================================================
int readGroqReply(
    String &body,
    uint32_t timeoutMs
)
{
    uint32_t deadline =
        millis() + timeoutMs;

    int status = 0;

    bool chunked = false;

    // ------------------------------
    // Read headers
    // ------------------------------
    for (
        String line = readLine(deadline);
        line.length();
        line = readLine(deadline)
    )
    {
        if (line.startsWith("HTTP/"))
        {
            status =
                line.substring(9, 12).toInt();
        }

        String lower =
            line;

        lower.toLowerCase();

        if (
            lower.startsWith("transfer-encoding:") &&
            lower.indexOf("chunked") >= 0
        )
        {
            chunked = true;
        }
    }

    // ------------------------------
    // Read chunked body
    // ------------------------------
    if (chunked)
    {
        while (true)
        {
            String line =
                readLine(deadline);

            long n =
                strtol(
                    line.c_str(),
                    nullptr,
                    16
                );

            if (n <= 0)
                break;

            readBytesToString(
                body,
                n,
                deadline
            );

            // trailing CRLF
            readLine(deadline);
        }
    }
    else
    {
        readBytesToString(
            body,
            1024 * 1024,
            deadline
        );
    }

    return status;
}

// ============================================================
// STREAM MIC AUDIO TO GROQ
// ============================================================
bool streamAudioToGroq(
    String &transcript
)
{
    if (!ensureGroqLink())
        return false;

    Serial.println();
    Serial.println("================================");
    Serial.println(" GROQ STREAMING STT");
    Serial.println("================================");

    // ========================================================
    // HTTP request headers
    // ========================================================
    net.print(
        String(
            "POST /openai/v1/audio/transcriptions HTTP/1.1\r\n"
            "Host: api.groq.com\r\n"
            "Authorization: Bearer "
        )
        + SEED_GROQ_KEY +
        "\r\n"
        "User-Agent: ESP32-WakeSTT/1.0\r\n"
        "Content-Type: multipart/form-data; boundary="
        BOUNDARY
        "\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n"
    );

    // ========================================================
    // Multipart form
    // ========================================================
    static const char *FORM =
        "--" BOUNDARY
        "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n"
        "\r\n"
        STT_MODEL
        "\r\n"

        "--" BOUNDARY
        "\r\n"
        "Content-Disposition: form-data; name=\"language\"\r\n"
        "\r\n"
        "en"
        "\r\n"

        "--" BOUNDARY
        "\r\n"
        "Content-Disposition: form-data; name=\"response_format\"\r\n"
        "\r\n"
        "text"
        "\r\n"

        "--" BOUNDARY
        "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav"
        "\r\n"
        "\r\n";

    if (
        !sendChunk(
            (const uint8_t *)FORM,
            strlen(FORM)
        )
    )
    {
        Serial.println(
            "[GROQ] Failed sending form"
        );

        net.stop();
        return false;
    }

    // ========================================================
    // Send WAV header
    // ========================================================
    uint8_t wavHeader[44];

    writeWavHeader(wavHeader);

    if (
        !sendChunk(
            wavHeader,
            sizeof(wavHeader)
        )
    )
    {
        Serial.println(
            "[GROQ] Failed sending WAV header"
        );

        net.stop();
        return false;
    }

    // ========================================================
    // Record and stream at the same time
    // ========================================================
    Serial.println(
        "[REC] Speak now..."
    );

    Serial.println(
        "[REC] Streaming microphone directly to Groq"
    );

    filterReset();

    uint8_t raw[1024];

    uint32_t start =
        millis();

    uint32_t lastSpeech =
        millis();

    uint32_t totalBytes =
        0;

    uint32_t peak =
        0;

    bool gotSpeech =
        false;

    while (
        millis() - start <
        MAX_RECORD_SECONDS * 1000UL
    )
    {
        // -----------------------------------------------
        // Read already-transformed 16-bit mono PCM
        // -----------------------------------------------
        size_t n =
            mic.readBytes(
                (char *)raw,
                sizeof(raw)
            );

        if (n == 0)
        {
            delay(1);
            continue;
        }

        int16_t *samples =
            (int16_t *)raw;

        size_t count =
            n / sizeof(int16_t);

        // -----------------------------------------------
        // Filter + gain
        // -----------------------------------------------
        filterBlock(
            samples,
            count
        );

        // -----------------------------------------------
        // Check speech energy
        // -----------------------------------------------
        uint32_t blockPeak =
            0;

        for (size_t i = 0; i < count; i++)
        {
            int32_t v =
                samples[i];

            uint32_t mag =
                v < 0 ? -v : v;

            if (mag > blockPeak)
                blockPeak = mag;

            if (mag > peak)
                peak = mag;
        }

        if (blockPeak > SILENCE_PEAK)
        {
            gotSpeech = true;
            lastSpeech = millis();
        }

        // -----------------------------------------------
        // Stream this chunk immediately
        // -----------------------------------------------
        if (
            !sendChunk(
                raw,
                n
            )
        )
        {
            Serial.println(
                "[GROQ] Audio upload failed"
            );

            net.stop();
            return false;
        }

        totalBytes += n;

        // -----------------------------------------------
        // Debug every ~1 second
        // -----------------------------------------------
        static uint32_t lastPrint = 0;

        if (
            millis() - lastPrint >
            1000
        )
        {
            lastPrint =
                millis();

            Serial.printf(
                "[REC] %.1fs  bytes=%u  peak=%u\n",
                (millis() - start) / 1000.0f,
                totalBytes,
                peak
            );
        }

        // -----------------------------------------------
        // Automatic end of speech
        // -----------------------------------------------
        uint32_t elapsed =
            millis() - start;

        if (
            gotSpeech &&
            elapsed > MIN_RECORD_MS &&
            millis() - lastSpeech >
                SILENCE_HOLD_MS
        )
        {
            Serial.println(
                "[REC] Speech ended"
            );

            break;
        }
    }

    Serial.printf(
        "[REC] Total streamed: %u bytes\n",
        totalBytes
    );

    Serial.printf(
        "[REC] Peak: %u\n",
        peak
    );

    if (!gotSpeech)
    {
        Serial.println(
            "[REC] No speech detected"
        );
    }

    // ========================================================
    // Finish multipart request
    // ========================================================
    String tail =
        "\r\n--" BOUNDARY
        "--\r\n";

    if (
        !sendChunk(
            (const uint8_t *)tail.c_str(),
            tail.length()
        )
    )
    {
        net.stop();
        return false;
    }

    // Zero-length HTTP chunk
    if (
        net.write(
            (const uint8_t *)"0\r\n\r\n",
            5
        ) != 5
    )
    {
        net.stop();
        return false;
    }

    // ========================================================
    // Read Groq
    // ========================================================
    Serial.println(
        "[GROQ] Waiting for transcription..."
    );

    String body;

    int status =
        readGroqReply(
            body,
            20000
        );

    net.stop();

    Serial.printf(
        "[GROQ] HTTP %d\n",
        status
    );

    if (status != 200)
    {
        Serial.println(
            "[GROQ] Server response:"
        );

        Serial.println(
            body
        );

        return false;
    }

    body.trim();

    transcript =
        body;

    return transcript.length() > 0;
}

// ============================================================
// START MICROPHONE
// ============================================================
bool micStart()
{
    // SCK, WS, DOUT, DIN, MCLK
    mic.setPins(
        PIN_MIC_SCK,
        PIN_MIC_WS,
        -1,
        PIN_MIC_SD,
        -1
    );

    bool ok =
        mic.begin(
            I2S_MODE_STD,
            SAMPLE_RATE,
            I2S_DATA_BIT_WIDTH_32BIT,
            I2S_SLOT_MODE_MONO
        );

    if (!ok)
    {
        Serial.println(
            "[MIC] I2S begin FAILED"
        );

        return false;
    }

    // Convert 32-bit INMP441 slots to
    // normal 16-bit mono PCM.
    ok =
        mic.configureRX(
            SAMPLE_RATE,
            I2S_DATA_BIT_WIDTH_32BIT,
            I2S_SLOT_MODE_MONO,
            I2S_RX_TRANSFORM_32_TO_16
        );

    Serial.printf(
        "[MIC] SD=%d SCK=%d WS=%d -> %s\n",
        PIN_MIC_SD,
        PIN_MIC_SCK,
        PIN_MIC_WS,
        ok ? "started" : "RX CONFIG FAILED"
    );

    return ok;
}

// ============================================================
// SETUP
// ============================================================
void setup()
{
    Serial.begin(115200);

    delay(1500);

    Serial.println();
    Serial.println(
        "=========================================="
    );

    Serial.println(
        " ESP32-S3 + INMP441"
    );

    Serial.println(
        " Hi ESP -> Groq Streaming STT"
    );

    Serial.println(
        "=========================================="
    );

    // ========================================================
    // WiFi
    // ========================================================
    WiFi.mode(WIFI_STA);

    WiFi.begin(
        SEED_WIFI_SSID,
        SEED_WIFI_PASS
    );

    Serial.print(
        "[WiFi] Connecting"
    );

    while (
        WiFi.status() != WL_CONNECTED
    )
    {
        Serial.print(".");
        delay(300);
    }

    Serial.println();

    Serial.print(
        "[WiFi] IP: "
    );

    Serial.println(
        WiFi.localIP()
    );

    // ========================================================
    // Microphone
    // ========================================================
    if (!micStart())
    {
        while (true)
            delay(1000);
    }

    // ========================================================
    // WakeNet model
    // ========================================================
    models =
        esp_srmodel_init("model");

    if (!models)
    {
        Serial.println(
            "[WAKE] Model partition failed"
        );

        while (true)
            delay(1000);
    }

    Serial.printf(
        "[WAKE] Models found: %d\n",
        models->num
    );

    // Find Hi ESP
    wakeModelName =
        esp_srmodel_filter(
            models,
            ESP_WN_PREFIX,
            "hiesp"
        );

    if (!wakeModelName)
    {
        Serial.println(
            "[WAKE] wn9_hiesp not found!"
        );

        while (true)
            delay(1000);
    }

    Serial.print(
        "[WAKE] Model: "
    );

    Serial.println(
        wakeModelName
    );

    // ========================================================
    // WakeNet interface
    // ========================================================
    wakenet =
        esp_wn_handle_from_name(
            wakeModelName
        );

    if (!wakenet)
    {
        Serial.println(
            "[WAKE] WakeNet handle failed"
        );

        while (true)
            delay(1000);
    }

    wakeData =
        wakenet->create(
            wakeModelName,
            DET_MODE_95
        );

    if (!wakeData)
    {
        Serial.println(
            "[WAKE] WakeNet create failed"
        );

        while (true)
            delay(1000);
    }

    wakeChunkSize =
        wakenet->get_samp_chunksize(
            wakeData
        );

    Serial.printf(
        "[WAKE] Chunk: %d samples\n",
        wakeChunkSize
    );

    Serial.printf(
        "[WAKE] Rate: %d Hz\n",
        wakenet->get_samp_rate(
            wakeData
        )
    );

    // ========================================================
    // Pre-connect to Groq
    // ========================================================
    ensureGroqLink();

    Serial.println();
    Serial.println(
        "=========================================="
    );

    Serial.println(
        "READY"
    );

    Serial.println(
        "Say: HI ESP"
    );

    Serial.println(
        "Then speak your command."
    );

    Serial.println(
        "=========================================="
    );
}

// ============================================================
// LOOP
// ============================================================
void loop()
{
    // --------------------------------------------------------
    // Keep Groq connection warm
    // --------------------------------------------------------
    static uint32_t nextLinkTry = 0;

    if (