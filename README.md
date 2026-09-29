# ESP32-S3 AI Voice Assistant — WakeNet9 + Groq Whisper

A compact voice-assistant platform built around an ESP32-S3 and INMP441 I2S microphone.

The project evolves through four firmware stages, ending with a wake-word activated, streaming speech-to-text system:

- **Wake word:** `Hi ESP` using ESP-SR WakeNet9 (`wn9_hiesp`)
- **Speech-to-text:** Groq `whisper-large-v3-turbo`
- **Audio:** INMP441, 16 kHz mono
- **Recording stop:** 2 seconds of continuous silence
- **Maximum recording:** 10 seconds
- **Status LED:** off while waiting, blue blinking during recording, red for 2 seconds after recording

## Hardware

### ESP32-S3

Tested configuration:

- ESP32-S3-WROOM-1 N16R8
- 16 MB flash
- 8 MB OPI PSRAM

### INMP441 wiring

| INMP441 | ESP32-S3 |
|---|---|
| VDD | 3.3V |
| GND | GND |
| L/R | GND |
| SD | GPIO16 |
| SCK | GPIO17 |
| WS | GPIO15 |

## Arduino IDE configuration

Use **ESP32 Arduino core 3.3.12**.

Set:

- Board: **ESP32S3 Dev Module**
- Flash Size: **16MB**
- PSRAM: **OPI PSRAM**
- Partition Scheme: **ESP SR 16M**
- USB Mode: **Hardware CDC and JTAG**
- USB CDC On Boot: **Enabled**
- Upload Mode: **UART0 / Hardware CDC**

The ESP SR 16M partition provides a 3 MB application area and a model partition of about 3.9 MB.

## Partition layout

The repository includes [config/partitions.csv](config/partitions.csv).

Important addresses:

| Partition | Offset | Size |
|---|---:|---:|
| app0 | 0x10000 | 0x300000 |
| app1 | 0x310000 | 0x300000 |
| spiffs | 0x610000 | 0x600000 |
| model | 0xC10000 | 0x3E0000 |
| coredump | 0xFF0000 | 0x10000 |

## Firmware versions

### V1 — Basic firmware

Path:

`firmware/v1-basic/01_basic_wake_test.ino`

Initial ESP32-S3 / microphone / speech-to-text development version.

### V2 — WakeNet + Groq

Path:

`firmware/v2-wakenet-groq/02_hiesp_groq_streaming.ino`

Adds:

- WakeNet9
- `Hi ESP`
- direct Groq audio streaming

### V3 — Silence detection

Path:

`firmware/v3-silence/03_silence_detection.ino`

Adds automatic recording termination after **2 seconds of silence**.

### V4 — Final LED version

Path:

`firmware/v4-final/04_final_voice_assistant.ino`

Adds the final recording-state LED behavior:

1. Waiting for `Hi ESP` → **LED OFF**
2. `Hi ESP` detected → **BLUE blinking**
3. Recording stops → **RED for 2 seconds**
4. Processing/waiting → **LED OFF**

## ESP-SR model

The project uses the Hi ESP WakeNet model:

`wn9_hiesp`

The model binary must be flashed to:

`0xC10000`

The standard model file supplied by the ESP32 package is:

`srmodels.bin`

## Groq credentials

Copy:

`secrets.example.h`

to:

`secrets.h`

and fill in:

```cpp
#define SEED_WIFI_SSID "YOUR_WIFI_NAME"
#define SEED_WIFI_PASS "YOUR_WIFI_PASSWORD"
#define SEED_GROQ_KEY  "YOUR_GROQ_API_KEY"
```

**Never commit `secrets.h` or real API keys to the repository.**

## PowerShell build

The repository was developed with Arduino CLI and ESP32 core 3.3.12.

Set:

```powershell
$sketch = "C:\Users\user\Documents\Arduino\4"
$build  = "$env:LOCALAPPDATA\arduino\sketches\4_build"
$cli    = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
```

Compile:

```powershell
& $cli compile `
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=esp_sr_16,USBMode=hwcdc,CDCOnBoot=cdc" `
  --build-path $build `
  $sketch
```

For faster repeated builds, do **not** use `--clean`.

## Copy the Hi ESP model

```powershell
$hiesp = "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\esp32s3-libs\3.3.12\esp_sr\srmodels.bin"

Copy-Item $hiesp "$build\srmodels.bin" -Force
```

Expected size:

`3340296` bytes.

## Manual flashing

Because the ESP-SR post-build upload hook may fail to locate the temporary `srmodels.bin`, the reliable workflow is to flash the firmware and model manually with esptool.

```powershell
$esptool = "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\esptool_py\5.3.1\esptool.exe"

& $esptool --port COM9 write-flash `
0x0000 "$build\4.ino.bootloader.bin" `
0x8000 "$build\4.ino.partitions.bin" `
0xE000 "$build\boot_app0.bin" `
0x10000 "$build\4.ino.bin" `
0xC10000 "$build\srmodels.bin"
```

Open the serial monitor at **115200 baud**.

## Runtime flow

```text
Power on
   ↓
Wi-Fi
   ↓
ESP-SR / WakeNet9
   ↓
Wait for "HI ESP"
   ↓
Wake word detected
   ↓
BLUE LED blinking
   ↓
Record + stream PCM to Groq
   ↓
2 seconds continuous silence
   ↓
Stop recording
   ↓
RED LED for 2 seconds
   ↓
Groq Whisper transcription
   ↓
Print "YOU SAID"
   ↓
Wait for "HI ESP"
```

### Demo secrets files

Each firmware folder contains its own `secrets.example.h` so the sketch can be copied and configured independently:

- `firmware/v1-basic/secrets.example.h`
- `firmware/v2-wakenet-groq/secrets.example.h`
- `firmware/v3-silence/secrets.example.h`
- `firmware/v4-final/secrets.example.h`

Copy the matching demo file to `secrets.h` and replace the placeholders. Never commit the real `secrets.h`.

## Troubleshooting

### Model not found

Verify that:

- the **ESP SR 16M** partition is selected;
- `srmodels.bin` exists;
- the model is flashed at `0xC10000`.

### Sketch too large

Make sure the selected partition scheme is:

`esp_sr_16`

not the default 4 MB partition.

### Fast compilation

Keep the same build directory and avoid:

`--clean`

### Groq authentication

Check `secrets.h` and make sure the API key is valid.

## Project structure

```text
.
├── README.md
├── secrets.example.h
├── config/
│   └── partitions.csv
└── firmware/
    ├── v1-basic/
    │   └── 1.ino
    ├── v2-wakenet-groq/
    │   └── 2.ino
    ├── v3-silence/
    │   └── 3.ino
    └── v4-final/
        └── 4.ino
```
