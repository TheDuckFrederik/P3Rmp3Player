# ESP32 MP3 Player (SD + I2S DAC)

This project is a simple MP3 player for ESP32.

## Hardware (default wiring)

- ESP32 DevKit board
- MicroSD card module (SPI)
- I2S DAC module (example: MAX98357A)
- 128x32 I2C OLED screen (SSD1306, address `0x3C`)
- 5 push buttons (Play/Pause, Next, Prev, Vol+, Vol-)
- Speaker connected to DAC output

### SD card pins

- CS -> GPIO 5
- SCK -> GPIO 18
- MISO -> GPIO 19
- MOSI -> GPIO 23

### I2S DAC pins

- BCLK -> GPIO 26
- LRC/WS -> GPIO 25
- DIN -> GPIO 22

### OLED pins (128x32 I2C)

- SDA -> GPIO 21
- SCL -> GPIO 4
- VCC -> 3.3V
- GND -> GND

### Buttons

Buttons are wired between GPIO and GND (using internal pull-ups):

- Play/Pause -> GPIO 32
- Next -> GPIO 33
- Previous -> GPIO 27
- Volume Up -> GPIO 14
- Volume Down -> GPIO 13

## SD card structure

Format SD card as FAT32 and create this folder:

- `/music`

Put your MP3 files inside `/music`.

Example:

- `/music/song1.mp3`
- `/music/song2.mp3`

## Build and upload (Arduino IDE)

1. Install Arduino IDE 2.x.
2. Open [P3Rmp3Player.ino](P3Rmp3Player.ino).
3. Install ESP32 board package:
   - Arduino IDE -> Boards Manager
   - Search `esp32`
   - Install `esp32 by Espressif Systems`
4. Install required libraries from Library Manager:
   - `ESP32-audioI2S`
   - `Adafruit GFX Library`
   - `Adafruit SSD1306`
5. Select your board and port:
   - Tools -> Board -> ESP32 Arduino -> your board (example: ESP32 Dev Module)
   - Tools -> Port -> your serial port
6. Click Upload.

## Behavior

- Starts playing first track found in `/music`.
- Next/Prev cycles through track list.
- Play/Pause toggles current playback.
- Vol+/Vol- adjusts volume.
- When a track ends, next track auto-plays.
- OLED shows song title and `mm:ss / mm:ss` (timestamp / duration).

## Notes

- If your hardware uses different pins, edit pin definitions in `src/main.cpp`.
- Some cheap SD modules are 5V; use proper level shifting or 3.3V-safe module.
