# Version 5 Development Plan

## Version rule

The current next firmware generation is V5.

Use `v5/` for the V5 firmware family and filenames that describe the feature or milestone.

Example:

    firmware/
    ├── final/
    │   └── esp32s3_groq_voice_assistant_final.ino
    └── v5/
        └── esp32s3_voice_assistant_v2.ino

## What to document for every V5 change

### 1. Goal
What the new version is intended to add or improve.

### 2. Problem
What limitation in the previous version motivates the change.

### 3. Design choice
What was changed in the firmware or hardware interface.

### 4. Reason
Why that implementation was selected.

### 5. Expected behavior
What should happen on the ESP32-S3 after the change.

### 6. Test result
Record the actual Serial Monitor output, timing, errors, and observed behavior.

### 7. Next change
Describe what remains to be improved.

## V5 development principle

V5 should be developed as an incremental improvement over the proven final V1 firmware.

Keep the following working foundation unless a V5 change explicitly requires replacing it:

- ESP32-S3-WROOM-1 N16R8
- INMP441 on GPIO16/17/15
- WakeNet9 Hi ESP
- Groq whisper-large-v3-turbo
- 16 kHz mono audio
- direct audio streaming
- 2-second silence termination
- current LED state indication
- ESP-SR 16M partition with model at 0xC10000

Each V5 change should have a clear reason and a measurable test.

## V5 change log template

    V5.x
    Goal:
    Problem:
    Change:
    Reason:
    Expected result:
    Actual result:
    Next:

## Suggested V5 workflow

1. Copy the latest known-working firmware into a new V5 file.
2. Change one major behavior at a time.
3. Compile using the same ESP32-SR build configuration.
4. Flash the known-good Hi ESP model when required.
5. Test wake word, recording, silence detection, Groq response, and LED state.
6. Record the result before starting the next change.