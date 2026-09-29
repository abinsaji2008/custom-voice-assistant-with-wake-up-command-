// ============================================================================
// ESP32-S3 + INMP441
// WakeNet9 "Hi ESP" -> Groq Whisper Streaming STT
//
// Hardware:
//   INMP441 VDD  -> 3.3V
//   INMP441 GND  -> GND
//   INMP441 L/R  -> GND   (LEFT channel)
//   INMP441 SD   -> GPIO16
//   INMP441 SCK  -> GPIO17
//   INMP441 WS   -> GPIO15
//
// Behavior:
//   1. Wait for "HI ESP"
//   2. Start streaming microphone directly to Groq
//   3. Wait for speech
//   4. After speech starts, stop when silence lasts 2 seconds
//   5. Hard maximum recording time = 10 seconds
//   6. Print transcript on Serial Monitor
//
// Required:
//   - ESP32 Arduino core 3.3.12
//   - ESP-SR / ESP_SR included with ESP32 package
//   - secrets.h
//
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP_I2S.h>

#include "secrets.h"

// ESP-SR
#include "model_path.h"
#include "esp_wn_models.h"
#include "esp_wn_iface.h"


// ============================================================================
// SETTINGS
// ============================================================================

#define SAMPLE_RATE             16000

// Maximum recording duration after wake word
#define MAX_RECORD_SECONDS      10

// Must hear voice within this time after wake word
#define WAIT_FOR_SPEECH_MS      4000

// Stop after this much continuous silence
#define SILENCE_TIMEOUT_MS      2000

// Voice detector threshold.
// This is measured BEFORE MIC_GAIN is applied.
#define VOICE_THRESHOLD          700

// Audio filter
#define HP_CUTOFF_HZ             120

// Keep your currently working gain
#define MIC_GAIN                 8


// ============================================================================
// INMP441 PINS
// ============================================================================

#define MIC_SD                   16
#define MIC_SCK                  17
#define MIC_WS                   15


// ============================================================================
// GROQ
// ============================================================================

#define GROQ_HOST                "api.groq.com"
#define GROQ_PORT                443

#define STT_MODEL                "whisper-large-v3-turbo"

#define BOUNDARY                 "----esp32speech"


// ============================================================================
// GLOBAL OBJECTS
// ============================================================================

static I2SClass mic;

static WiFiClientSecure net;

static bool micOK = false;


// ============================================================================
// WAKENET GLOBALS
// ============================================================================

static srmodel_list_t *srModels = nullptr;

static const esp_wn_iface_t *wakeNet = nullptr;

static model_iface_data_t *wakeNetData = nullptr;

static char *wakeModelName = nullptr;

static int wakeChunkSamples = 512;

static int wakeSampleRate = SAMPLE_RATE;


// ============================================================================
// AUDIO FILTER STATE
// ============================================================================

static float hpA = 0.0f;

static float hpY1 = 0.0f;
static float hpX1 = 0.0f;

static float hpY2 = 0.0f;
static float hpX2 = 0.0f;


// ============================================================================
// FILTER RESET
// ============================================================================

static void filterReset()
{
    float rc = 1.0f / (2.0f * PI * HP_CUTOFF_HZ);

    float dt = 1.0f / SAMPLE_RATE;

    hpA = rc / (rc + dt);

    hpY1 = 0.0f;
    hpX1 = 0.0f;

    hpY2 = 0.0f;
    hpX2 = 0.0f;
}


// ============================================================================
// FILTER + GAIN
//
// Returns the peak level BEFORE gain.
// This is important because your current MIC_GAIN=8 can make the final
// signal reach 32767 and clip. Silence detection therefore uses the
// pre-gain signal.
// ============================================================================

static uint32_t filterBlock(int16_t *samples, size_t count)
{
    uint32_t preGainPeak = 0;

    for (size_t i = 0; i < count; i++)
    {
        float x = samples[i];

        // High-pass stage 1
        float a = hpA * (hpY1 + x - hpX1);

        hpX1 = x;
        hpY1 = a;


        // High-pass stage 2
        float b = hpA * (hpY2 + a - hpX2);

        hpX2 = a;
        hpY2 = b;


        // Peak BEFORE gain
        uint32_t mag = (uint32_t)fabsf(b);

        if (mag > preGainPeak)
        {
            preGainPeak = mag;
        }


        // Gain
        int32_t v = (int32_t)(b * MIC_GAIN);


        // Clip safely to int16
        if (v > 32767)
            v = 32767;

        if (v < -32768)
            v = -32768;


        samples[i] = (int16_t)v;
    }

    return preGainPeak;
}


// ============================================================================
// MICROPHONE START
// ============================================================================

static bool startMic()
{
    mic.setPins(
        MIC_SCK,
        MIC_WS,
        -1,
        MIC_SD,
        -1
    );


    bool ok1 = mic.begin(
        I2S_MODE_STD,
        SAMPLE_RATE,
        I2S_DATA_BIT_WIDTH_32BIT,
        I2S_SLOT_MODE_MONO
    );


    bool ok2 = false;


    if (ok1)
    {
        ok2 = mic.configureRX(
            SAMPLE_RATE,
            I2S_DATA_BIT_WIDTH_32BIT,
            I2S_SLOT_MODE_MONO,
            I2S_RX_TRANSFORM_32_TO_16
        );
    }


    micOK = ok1 && ok2;


    Serial.printf(
        "[MIC] SD=%d SCK=%d WS=%d -> %s\n",
        MIC_SD,
        MIC_SCK,
        MIC_WS,
        micOK ? "started" : "FAILED"
    );


    return micOK;
}


// ============================================================================
// WAKENET START
// ============================================================================

static bool startWakeNet()
{
    Serial.println("[WAKE] Loading ESP-SR models...");


    srModels = esp_srmodel_init("model");


    if (srModels == nullptr)
    {
        Serial.println("[WAKE] ERROR: model partition could not be loaded");
        return false;
    }


    Serial.printf(
        "[WAKE] Models found: %d\n",
        srModels->num
    );


    for (int i = 0; i < srModels->num; i++)
    {
        Serial.printf(
            "[WAKE] Model: %s\n",
            srModels->model_name[i]
        );
    }


    // Select wn9_hiesp
    wakeModelName =
        esp_srmodel_filter(
            srModels,
            ESP_WN_PREFIX,
            "hiesp"
        );


    if (wakeModelName == nullptr)
    {
        Serial.println("[WAKE] ERROR: wn9_hiesp not found");
        return false;
    }


    Serial.printf(
        "[WAKE] Selected: %s\n",
        wakeModelName
    );


    wakeNet =
        esp_wn_handle_from_name(
            wakeModelName
        );


    if (wakeNet == nullptr)
    {
        Serial.println("[WAKE] ERROR: WakeNet handle not found");
        return false;
    }


    // Normal detection mode
    wakeNetData =
        wakeNet->create(
            wakeModelName,
            DET_MODE_90
        );


    if (wakeNetData == nullptr)
    {
        Serial.println("[WAKE] ERROR: WakeNet create failed");
        return false;
    }


    wakeChunkSamples =
        wakeNet->get_samp_chunksize(
            wakeNetData
        );


    wakeSampleRate =
        wakeNet->get_samp_rate(
            wakeNetData
        );


    Serial.printf(
        "[WAKE] Chunk: %d samples\n",
        wakeChunkSamples
    );


    Serial.printf(
        "[WAKE] Rate: %d Hz\n",
        wakeSampleRate
    );


    char *wakeWord =
        esp_srmodel_get_wake_words(
            srModels,
            wakeModelName
        );


    if (wakeWord != nullptr)
    {
        Serial.printf(
            "[WAKE] Wake word: %s\n",
            wakeWord
        );
    }


    return true;
}


// ============================================================================
// WAKE WORD LISTEN
//
// Reads exactly the WakeNet chunk size from the microphone.
// ============================================================================

static bool waitForWakeWord()
{
    if (!wakeNet || !wakeNetData)
    {
        return false;
    }


    static int16_t wakeBuffer[512];


    while (true)
    {
        size_t wanted =
            wakeChunkSamples * sizeof(int16_t);


        size_t got =
            mic.readBytes(
                (char *)wakeBuffer,
                wanted
            );


        if (got != wanted)
        {
            delay(1);
            continue;
        }


        int result =
            wakeNet->detect(
                wakeNetData,
                wakeBuffer
            );


        if (result > 0)
        {
            return true;
        }


        delay(0);
    }
}


// ============================================================================
// GROQ CONNECTION
// ============================================================================

static bool ensureGroqConnection()
{
    if (net.connected())
    {
        return true;
    }


    Serial.println("[GROQ] Connecting...");


    net.setInsecure();

    net.setTimeout(20);


    if (!net.connect(
            GROQ_HOST,
            GROQ_PORT
        ))
    {
        Serial.println("[GROQ] Connection failed");

        return false;
    }


    Serial.println("[GROQ] Connected");

    return true;
}


// ============================================================================
// CHUNKED HTTP SEND
// ============================================================================

static bool sendChunk(
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
    {
        return false;
    }


    if (
        net.write(
            data,
            len
        ) != len
    )
    {
        return false;
    }


    if (
        net.write(
            (const uint8_t *)"\r\n",
            2
        ) != 2
    )
    {
        return false;
    }


    return true;
}


// ============================================================================
// UNKNOWN-LENGTH WAV HEADER
// ============================================================================

static void writeWavHeader(
    uint8_t *h
)
{
    const uint32_t UNKNOWN = 0xFFFFFFFF;

    const uint32_t rate =
        SAMPLE_RATE;

    const uint32_t byteRate =
        rate * 2;


    const uint32_t fmtLen = 16;


    const uint16_t pcm = 1;

    const uint16_t channels = 1;

    const uint16_t blockAlign = 2;

    const uint16_t bits = 16;


    memcpy(
        h + 0,
        "RIFF",
        4
    );


    memcpy(
        h + 4,
        &UNKNOWN,
        4
    );


    memcpy(
        h + 8,
        "WAVEfmt ",
        8
    );


    memcpy(
        h + 16,
        &fmtLen,
        4
    );


    memcpy(
        h + 20,
        &pcm,
        2
    );


    memcpy(
        h + 22,
        &channels,
        2
    );


    memcpy(
        h + 24,
        &rate,
        4
    );


    memcpy(
        h + 28,
        &byteRate,
        4
    );


    memcpy(
        h + 32,
        &blockAlign,
        2
    );


    memcpy(
        h + 34,
        &bits,
        2
    );


    memcpy(
        h + 36,
        "data",
        4
    );


    memcpy(
        h + 40,
        &UNKNOWN,
        4
    );
}


// ============================================================================
// HTTP READ LINE
// ============================================================================

static String readLine(
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


        char c =
            net.read();


        if (c == '\n')
            break;


        if (c != '\r')
            s += c;
    }


    return s;
}


// ============================================================================
// READ EXACT BYTES
// ============================================================================

static void readBytesToString(
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


// ============================================================================
// READ GROQ HTTP RESPONSE
// ============================================================================

static int readReply(
    String *body,
    uint32_t timeoutMs
)
{
    uint32_t deadline =
        millis() + timeoutMs;


    int status = 0;

    bool chunked = false;


    // HTTP status + headers
    while (millis() < deadline)
    {
        String line =
            readLine(deadline);


        if (line.length() == 0)
            break;


        if (line.startsWith("HTTP/"))
        {
            status =
                line.substring(
                    9,
                    12
                ).toInt();
        }


        String lower = line;

        lower.toLowerCase();


        if (
            lower.startsWith(
                "transfer-encoding:"
            ) &&
            lower.indexOf(
                "chunked"
            ) >= 0
        )
        {
            chunked = true;
        }
    }


    // Chunked response
    if (chunked)
    {
        while (true)
        {
            String sizeLine =
                readLine(deadline);


            long n =
                strtol(
                    sizeLine.c_str(),
                    nullptr,
                    16
                );


            if (n <= 0)
                break;


            readBytesToString(
                *body,
                n,
                deadline
            );


            // CRLF
            readLine(deadline);
        }
    }


    // Normal response
    else
    {
        readBytesToString(
            *body,
            1024 * 1024,
            deadline
        );
    }


    return status;
}


// ============================================================================
// GROQ TRANSCRIPTION
//
// Important behavior:
//
//   WAIT_FOR_SPEECH_MS:
//       After "Hi ESP", the device waits up to 4 seconds for actual speech.
//
//   SILENCE_TIMEOUT_MS:
//       Once speech has started, 2 seconds without voice ends recording.
//
//   MAX_RECORD_SECONDS:
//       Absolute safety limit of 10 seconds.
// ============================================================================

static bool transcribe(
    String *resultText
)
{
    if (!micOK)
    {
        Serial.println("[MIC] Not running");

        return false;
    }


    if (!ensureGroqConnection())
    {
        return false;
    }


    // ------------------------------------------------------------------------
    // HTTP REQUEST
    // ------------------------------------------------------------------------

    String headers =
        String(
            "POST /openai/v1/audio/transcriptions HTTP/1.1\r\n"
            "Host: "
        ) +
        GROQ_HOST +
        "\r\n"
        "Authorization: Bearer " +
        SEED_GROQ_KEY +
        "\r\n"
        "User-Agent: ESP32-S3-Groq/1.0\r\n"
        "Content-Type: multipart/form-data; boundary=" +
        BOUNDARY +
        "\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n";


    net.print(headers);


    // ------------------------------------------------------------------------
    // FORM
    // ------------------------------------------------------------------------

    static const char *FORM =
        "--" BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n"
        "\r\n"
        STT_MODEL
        "\r\n"

        "--" BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"language\"\r\n"
        "\r\n"
        "en"
        "\r\n"

        "--" BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"response_format\"\r\n"
        "\r\n"
        "text"
        "\r\n"

        "--" BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n"
        "\r\n";


    bool ok =
        sendChunk(
            (const uint8_t *)FORM,
            strlen(FORM)
        );


    // ------------------------------------------------------------------------
    // WAV HEADER
    // ------------------------------------------------------------------------

    uint8_t wavHeader[44];

    writeWavHeader(
        wavHeader
    );


    if (ok)
    {
        ok =
            sendChunk(
                wavHeader,
                sizeof(wavHeader)
            );
    }


    if (!ok)
    {
        net.stop();

        Serial.println("[GROQ] Failed sending request");

        return false;
    }


    // ------------------------------------------------------------------------
    // RECORDING
    // ------------------------------------------------------------------------

    Serial.println();
    Serial.println("================================");
    Serial.println(" GROQ STREAMING STT");
    Serial.println("================================");

    Serial.println("[REC] Speak now...");
    Serial.println("[REC] 2 seconds silence will end recording");


    filterReset();


    uint8_t raw[1024];


    uint32_t startTime =
        millis();


    uint32_t lastVoiceTime =
        startTime;


    bool speechStarted =
        false;


    uint32_t totalBytes =
        0;


    uint32_t overallPeak =
        0;

