#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <vector>
#include <algorithm>

#include <Audio.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

namespace Pins {
constexpr int SD_CS = 5;
constexpr int SD_SCK = 18;
constexpr int SD_MISO = 19;
constexpr int SD_MOSI = 23;

constexpr int I2S_BCLK = 26;
constexpr int I2S_LRC = 25;
constexpr int I2S_DOUT = 22;

constexpr int BTN_PLAY_PAUSE = 32;
constexpr int BTN_NEXT = 33;
constexpr int BTN_PREV = 27;
constexpr int BTN_VOL_UP = 14;
constexpr int BTN_VOL_DOWN = 13;

constexpr int OLED_SDA = 21;
constexpr int OLED_SCL = 4;
} // namespace Pins

namespace PlayerConfig {
constexpr const char* MUSIC_DIR = "/music";
constexpr int DEBOUNCE_MS = 35;
constexpr uint8_t START_VOLUME = 12;
constexpr uint8_t MIN_VOLUME = 0;
constexpr uint8_t MAX_VOLUME = 21;
constexpr unsigned long DISPLAY_REFRESH_MS = 250;
} // namespace PlayerConfig

namespace DisplayConfig {
constexpr int WIDTH = 128;
constexpr int HEIGHT = 32;
constexpr int RESET_PIN = -1;
constexpr uint8_t I2C_ADDRESS = 0x3C;
constexpr int CHARS_PER_LINE = 21;
} // namespace DisplayConfig

struct DebouncedButton {
  int pin;
  bool stableState = HIGH;
  bool lastRawState = HIGH;
  unsigned long lastChangeMs = 0;

  explicit DebouncedButton(int buttonPin) : pin(buttonPin) {}

  void begin() {
    pinMode(pin, INPUT_PULLUP);
    stableState = digitalRead(pin);
    lastRawState = stableState;
    lastChangeMs = millis();
  }

  bool pressed() {
    bool raw = digitalRead(pin);
    if (raw != lastRawState) {
      lastRawState = raw;
      lastChangeMs = millis();
    }

    if ((millis() - lastChangeMs) >= PlayerConfig::DEBOUNCE_MS && stableState != raw) {
      stableState = raw;
      return stableState == LOW;
    }

    return false;
  }
};

Audio audio;
Adafruit_SSD1306 display(DisplayConfig::WIDTH, DisplayConfig::HEIGHT, &Wire, DisplayConfig::RESET_PIN);
std::vector<String> tracks;
int currentTrackIndex = 0;
uint8_t currentVolume = PlayerConfig::START_VOLUME;
bool playbackStarted = false;
bool playbackPaused = false;
volatile bool trackFinished = false;
bool displayReady = false;
unsigned long lastDisplayUpdateMs = 0;

DebouncedButton btnPlayPause(Pins::BTN_PLAY_PAUSE);
DebouncedButton btnNext(Pins::BTN_NEXT);
DebouncedButton btnPrev(Pins::BTN_PREV);
DebouncedButton btnVolUp(Pins::BTN_VOL_UP);
DebouncedButton btnVolDown(Pins::BTN_VOL_DOWN);

String basenameNoExtension(const String& path) {
  int slash = path.lastIndexOf('/');
  String file = slash >= 0 ? path.substring(slash + 1) : path;
  int dot = file.lastIndexOf('.');
  if (dot > 0) {
    file = file.substring(0, dot);
  }
  return file;
}

String pad2(uint32_t value) {
  if (value < 10) {
    return "0" + String(value);
  }
  return String(value);
}

String formatMmSs(uint32_t totalSeconds) {
  uint32_t minutes = totalSeconds / 60;
  uint32_t seconds = totalSeconds % 60;
  return String(minutes) + ":" + pad2(seconds);
}

String fitLine(const String& text, int maxChars) {
  if (text.length() <= maxChars) {
    return text;
  }
  if (maxChars <= 3) {
    return text.substring(0, maxChars);
  }
  return text.substring(0, maxChars - 3) + "...";
}

void updateDisplay(bool force = false) {
  if (!displayReady) {
    return;
  }

  unsigned long nowMs = millis();
  if (!force && (nowMs - lastDisplayUpdateMs) < PlayerConfig::DISPLAY_REFRESH_MS) {
    return;
  }
  lastDisplayUpdateMs = nowMs;

  String title = "No song";
  if (!tracks.empty()) {
    title = basenameNoExtension(tracks[currentTrackIndex]);
  }

  String titleLine1 = title;
  String titleLine2 = "";
  if (title.length() > DisplayConfig::CHARS_PER_LINE) {
    titleLine1 = title.substring(0, DisplayConfig::CHARS_PER_LINE);
    titleLine2 = fitLine(title.substring(DisplayConfig::CHARS_PER_LINE), DisplayConfig::CHARS_PER_LINE);
  }

  uint32_t currentSeconds = audio.getAudioCurrentTime();
  uint32_t totalSeconds = audio.getAudioFileDuration();
  String timeLine;
  if (playbackStarted && totalSeconds > 0) {
    timeLine = formatMmSs(currentSeconds) + " / " + formatMmSs(totalSeconds);
  } else if (playbackStarted) {
    timeLine = formatMmSs(currentSeconds) + " / --:--";
  } else {
    timeLine = "--:-- / --:--";
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println(fitLine(titleLine1, DisplayConfig::CHARS_PER_LINE));

  display.setCursor(0, 8);
  display.println(fitLine(titleLine2, DisplayConfig::CHARS_PER_LINE));

  display.setCursor(0, 24);
  display.println(fitLine(timeLine, DisplayConfig::CHARS_PER_LINE));

  display.display();
}

bool hasMp3Extension(const String& path) {
  String lower = path;
  lower.toLowerCase();
  return lower.endsWith(".mp3");
}

void loadTrackList() {
  tracks.clear();

  File dir = SD.open(PlayerConfig::MUSIC_DIR);
  if (!dir || !dir.isDirectory()) {
    Serial.println("[ERR] /music folder not found on SD card");
    return;
  }

  while (true) {
    File entry = dir.openNextFile();
    if (!entry) {
      break;
    }

    if (!entry.isDirectory()) {
      String name = String(entry.name());
      if (hasMp3Extension(name)) {
        tracks.push_back(name);
      }
    }

    entry.close();
  }

  dir.close();

  std::sort(tracks.begin(), tracks.end(), [](const String& a, const String& b) {
    return a < b;
  });

  Serial.printf("[OK] Found %d MP3 file(s)\n", static_cast<int>(tracks.size()));
  for (size_t i = 0; i < tracks.size(); ++i) {
    Serial.printf("  %d: %s\n", static_cast<int>(i), tracks[i].c_str());
  }
}

bool playTrack(int index) {
  if (tracks.empty()) {
    Serial.println("[WARN] No tracks available");
    return false;
  }

  if (index < 0) {
    index = static_cast<int>(tracks.size()) - 1;
  }
  if (index >= static_cast<int>(tracks.size())) {
    index = 0;
  }

  currentTrackIndex = index;
  String path = tracks[currentTrackIndex];

  audio.stopSong();
  bool ok = audio.connecttoFS(SD, path.c_str());
  if (!ok) {
    Serial.printf("[ERR] Could not play: %s\n", path.c_str());
    playbackStarted = false;
    playbackPaused = false;
    return false;
  }

  playbackStarted = true;
  playbackPaused = false;
  trackFinished = false;

  Serial.printf("[PLAY] %s\n", path.c_str());
  updateDisplay(true);
  return true;
}

void playNextTrack() {
  if (tracks.empty()) {
    return;
  }
  playTrack(currentTrackIndex + 1);
}

void playPreviousTrack() {
  if (tracks.empty()) {
    return;
  }
  playTrack(currentTrackIndex - 1);
}

void togglePlayPause() {
  if (tracks.empty()) {
    return;
  }

  if (!playbackStarted) {
    playTrack(currentTrackIndex);
    return;
  }

  audio.pauseResume();
  playbackPaused = !playbackPaused;
  Serial.println(playbackPaused ? "[STATE] Paused" : "[STATE] Resumed");
  updateDisplay(true);
}

void volumeUp() {
  if (currentVolume < PlayerConfig::MAX_VOLUME) {
    currentVolume++;
    audio.setVolume(currentVolume);
    Serial.printf("[VOL] %u\n", currentVolume);
  }
}

void volumeDown() {
  if (currentVolume > PlayerConfig::MIN_VOLUME) {
    currentVolume--;
    audio.setVolume(currentVolume);
    Serial.printf("[VOL] %u\n", currentVolume);
  }
}

void handleButtons() {
  if (btnPlayPause.pressed()) {
    togglePlayPause();
  }
  if (btnNext.pressed()) {
    playNextTrack();
  }
  if (btnPrev.pressed()) {
    playPreviousTrack();
  }
  if (btnVolUp.pressed()) {
    volumeUp();
  }
  if (btnVolDown.pressed()) {
    volumeDown();
  }
}

void audio_eof_mp3(const char* info) {
  (void)info;
  trackFinished = true;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Wire.begin(Pins::OLED_SDA, Pins::OLED_SCL);
  displayReady = display.begin(SSD1306_SWITCHCAPVCC, DisplayConfig::I2C_ADDRESS);
  if (!displayReady) {
    Serial.println("[WARN] OLED init failed");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("P3R Music Player");
    display.setCursor(0, 16);
    display.println("Dark Hour ready");
    display.display();
  }

  btnPlayPause.begin();
  btnNext.begin();
  btnPrev.begin();
  btnVolUp.begin();
  btnVolDown.begin();

  SPI.begin(Pins::SD_SCK, Pins::SD_MISO, Pins::SD_MOSI, Pins::SD_CS);
  if (!SD.begin(Pins::SD_CS)) {
    Serial.println("[ERR] SD card init failed");
    while (true) {
      delay(1000);
    }
  }

  audio.setPinout(Pins::I2S_BCLK, Pins::I2S_LRC, Pins::I2S_DOUT);
  audio.setVolume(currentVolume);

  loadTrackList();
  if (!tracks.empty()) {
    playTrack(0);
  } else {
    updateDisplay(true);
  }

  Serial.println("[READY] ESP32 MP3 player started");
}

void loop() {
  audio.loop();
  handleButtons();
  updateDisplay();

  if (trackFinished) {
    trackFinished = false;
    playNextTrack();
  }
}
