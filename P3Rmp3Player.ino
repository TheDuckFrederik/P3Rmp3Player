/**
 * @file    P3Rmp3Player.ino
 * @brief   P3R Music Player — Dark Hour Edition
 *
 * @details Standalone ESP32 MP3 player inspired by Persona 3 Reload.
 *          Streams MP3 files from a FAT32 MicroSD card through an I2S DAC
 *          to a speaker, while rendering real-time track information on a
 *          128×32 SSD1306 OLED display. Five debounced push buttons provide
 *          transport and volume control.
 *
 * @section hardware Hardware
 *   - ESP32 DevKit (any variant)
 *   - MicroSD module (SPI, 3.3 V–safe)
 *   - I2S DAC module (PCM5102A — 3.3 V, stereo line-level output)
 *   - 128×32 OLED display (I2C, SSD1306, address 0x3C)
 *   - 5× tactile push buttons (active-low via internal pull-ups)
 *
 * @section libraries Required Libraries
 *   - ESP32-audioI2S  (schreibfaul1)
 *   - Adafruit GFX Library
 *   - Adafruit SSD1306
 *
 * @section structure SD Card Structure
 *   Format as FAT32.  Place MP3 files inside a top-level /music folder.
 *   Files are sorted alphabetically before playback begins.
 */
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <vector>
#include <algorithm>

#include <Audio.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

/**
 * @namespace Pins
 * @brief     GPIO pin assignments for all peripherals.
 *
 * @details   Centralises every hardware connection into a single enum-like
 *            namespace so that re-wiring the board only ever requires editing
 *            this block — no magic numbers elsewhere in the code.
 */
namespace Pins {
constexpr int SD_CS   = 5;    ///< SPI chip-select for the MicroSD module
constexpr int SD_SCK  = 18;   ///< SPI clock
constexpr int SD_MISO = 19;   ///< SPI MISO (Master In, Slave Out)
constexpr int SD_MOSI = 23;   ///< SPI MOSI (Master Out, Slave In)

constexpr int I2S_BCLK = 26;  ///< I2S bit clock output to DAC
constexpr int I2S_LRC  = 25;  ///< I2S word-select / left-right clock output to DAC
constexpr int I2S_DOUT = 15;  ///< I2S serial data output to DAC

constexpr int BTN_PLAY_PAUSE = 32; ///< Play / Pause toggle button (active-low)
constexpr int BTN_NEXT       = 33; ///< Skip to next track (active-low)
constexpr int BTN_PREV       = 27; ///< Skip to previous track (active-low)
constexpr int BTN_VOL_UP     = 14; ///< Increase volume (active-low)
constexpr int BTN_VOL_DOWN   = 13; ///< Decrease volume (active-low)

constexpr int OLED_SDA = 21;  ///< I2C data line for the SSD1306 OLED
constexpr int OLED_SCL = 22;  ///< I2C clock line for the SSD1306 OLED (standard ESP32 I2C SCL)
} // namespace Pins

/**
 * @namespace PlayerConfig
 * @brief     Runtime behaviour constants for the MP3 player.
 */
namespace PlayerConfig {
constexpr const char*  MUSIC_DIR          = "/music"; ///< SD card path scanned for .mp3 files
constexpr int          DEBOUNCE_MS         = 35;       ///< Button debounce window in milliseconds
constexpr uint8_t      START_VOLUME        = 12;       ///< Volume level on first boot (range 0–21)
constexpr uint8_t      MIN_VOLUME          = 0;        ///< Minimum allowed volume level
constexpr uint8_t      MAX_VOLUME          = 21;       ///< Maximum allowed volume level
constexpr unsigned long DISPLAY_REFRESH_MS = 250;      ///< Minimum interval between OLED redraws (ms)
} // namespace PlayerConfig

/**
 * @namespace DisplayConfig
 * @brief     Geometry and address constants for the SSD1306 OLED panel.
 */
namespace DisplayConfig {
constexpr int     WIDTH         = 128;  ///< Panel width in pixels
constexpr int     HEIGHT        = 32;   ///< Panel height in pixels
constexpr int     RESET_PIN     = -1;   ///< Reset pin; -1 = shared with MCU reset line
constexpr uint8_t I2C_ADDRESS   = 0x3C; ///< Default I2C address (try 0x3D if display is absent)
constexpr int     CHARS_PER_LINE = 21;  ///< Characters that fit on one line at text size 1
} // namespace DisplayConfig

/**
 * @struct DebouncedButton
 * @brief  Reusable GPIO button with software debounce.
 *
 * @details Reads a digital input configured with INPUT_PULLUP and returns true
 *          on a confirmed falling-edge transition (button press). The stable
 *          state must persist for at least PlayerConfig::DEBOUNCE_MS milliseconds
 *          before a new event is emitted, preventing false triggers from contact
 *          bounce or electrical noise.
 *
 * @note    Instantiate one DebouncedButton per physical button.  Call begin()
 *          once during setup(), then poll pressed() inside loop().
 */
struct DebouncedButton {
  int pin;                      ///< GPIO pin number this button is attached to
  bool stableState   = HIGH;    ///< Last debounced (confirmed) state
  bool lastRawState  = HIGH;    ///< Most recent raw digitalRead() result
  unsigned long lastChangeMs = 0; ///< millis() timestamp of the last raw-state change

  /// @brief Constructs a DebouncedButton bound to @p buttonPin.
  explicit DebouncedButton(int buttonPin) : pin(buttonPin) {}

  /**
   * @brief Configures the GPIO pin and seeds the debounce state machine.
   *        Must be called once from setup() before any call to pressed().
   */
  void begin() {
    pinMode(pin, INPUT_PULLUP);
    stableState  = digitalRead(pin);
    lastRawState = stableState;
    lastChangeMs = millis();
  }

  /**
   * @brief  Polls the button and returns whether a debounced press occurred.
   *
   * @details Should be called every loop() iteration. Returns @c true exactly
   *          once per physical press — on the transition from HIGH to LOW —
   *          after the input has been stable for PlayerConfig::DEBOUNCE_MS ms.
   *
   * @return @c true  on a confirmed button-press event (falling edge).
   * @return @c false while the state is stable or still within the debounce window.
   */
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

// ---------------------------------------------------------------------------
// Global peripheral instances
// ---------------------------------------------------------------------------
Audio audio;  ///< ESP32-audioI2S instance — handles MP3 decoding and I2S DMA output
Adafruit_SSD1306 display(DisplayConfig::WIDTH, DisplayConfig::HEIGHT, &Wire, DisplayConfig::RESET_PIN); ///< SSD1306 OLED driver

// ---------------------------------------------------------------------------
// Player state
// ---------------------------------------------------------------------------
std::vector<String> tracks;                          ///< Sorted list of discovered MP3 file paths on the SD card
int            currentTrackIndex = 0;                ///< Index into @c tracks for the currently active song
uint8_t        currentVolume     = PlayerConfig::START_VOLUME; ///< Current volume level (0–21)
bool           playbackStarted   = false;            ///< True once a track has been loaded and started
bool           playbackPaused    = false;             ///< True while playback is paused
volatile bool  trackFinished     = false;             ///< Set by audio_eof_mp3() ISR-style callback; checked in loop()
bool           displayReady      = false;             ///< True if OLED initialised successfully
unsigned long  lastDisplayUpdateMs = 0;               ///< millis() at the last OLED redraw

DebouncedButton btnPlayPause(Pins::BTN_PLAY_PAUSE);
DebouncedButton btnNext(Pins::BTN_NEXT);
DebouncedButton btnPrev(Pins::BTN_PREV);
DebouncedButton btnVolUp(Pins::BTN_VOL_UP);
DebouncedButton btnVolDown(Pins::BTN_VOL_DOWN);

// ---------------------------------------------------------------------------
// Helper utilities
// ---------------------------------------------------------------------------

/**
 * @brief  Extracts a display-friendly track name from a full SD file path.
 *
 * @details Strips the leading directory component (everything up to and
 *          including the last '/') and the file extension (everything from
 *          the last '.' onward), returning just the bare filename stem.
 *
 * @param  path  Full path as stored in the @c tracks vector (e.g. "/music/01_Song.mp3").
 * @return Human-readable track name (e.g. "01_Song").
 */
String basenameNoExtension(const String& path) {
  int slash = path.lastIndexOf('/');
  String file = slash >= 0 ? path.substring(slash + 1) : path;
  int dot = file.lastIndexOf('.');
  if (dot > 0) {
    file = file.substring(0, dot);
  }
  return file;
}

/**
 * @brief  Zero-pads an integer to at least two digits.
 *
 * @param  value  Non-negative integer value to format.
 * @return String of at least two characters (e.g. 5 → "05", 12 → "12").
 */
String pad2(uint32_t value) {
  if (value < 10) {
    return "0" + String(value);
  }
  return String(value);
}

/**
 * @brief  Formats a duration in seconds as a "mm:ss" string.
 *
 * @param  totalSeconds  Duration to format.
 * @return Formatted string, e.g. 125 → "2:05".
 */
String formatMmSs(uint32_t totalSeconds) {
  uint32_t minutes = totalSeconds / 60;
  uint32_t seconds = totalSeconds % 60;
  return String(minutes) + ":" + pad2(seconds);
}

/**
 * @brief  Truncates a string to fit within a maximum character width.
 *
 * @details If @p text exceeds @p maxChars, the returned string is shortened
 *          and an ellipsis ("...") is appended so the total length equals
 *          @p maxChars.  Strings that already fit are returned unchanged.
 *
 * @param  text      Source string to (optionally) truncate.
 * @param  maxChars  Maximum number of characters in the returned string.
 * @return Fitted string, at most @p maxChars characters long.
 */
String fitLine(const String& text, int maxChars) {
  if (text.length() <= maxChars) {
    return text;
  }
  if (maxChars <= 3) {
    return text.substring(0, maxChars);
  }
  return text.substring(0, maxChars - 3) + "...";
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

/**
 * @brief  Redraws the OLED with the current track title and playback progress.
 *
 * @details Renders up to two lines of track title (truncated with "..." if
 *          necessary) and a timestamp line in the format "mm:ss / mm:ss" at
 *          the bottom of the 128×32 panel.  Redraws are rate-limited to
 *          PlayerConfig::DISPLAY_REFRESH_MS to avoid starving the audio loop.
 *
 * @param  force  When @c true, bypasses the rate-limit and redraws immediately.
 *                Pass @c true after state changes (track switch, pause, etc.).
 */
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

// ---------------------------------------------------------------------------
// SD card / track management
// ---------------------------------------------------------------------------

/**
 * @brief  Returns @c true if @p path ends with ".mp3" (case-insensitive).
 *
 * @param  path  File path or name to test.
 * @return @c true if the path has an .mp3 extension.
 */
bool hasMp3Extension(const String& path) {
  String lower = path;
  lower.toLowerCase();
  return lower.endsWith(".mp3");
}

/**
 * @brief  Scans the SD card music directory and builds the sorted track list.
 *
 * @details Opens PlayerConfig::MUSIC_DIR, iterates all top-level entries,
 *          and appends any file whose name ends in ".mp3" (case-insensitive)
 *          to the global @c tracks vector.  The vector is then sorted in
 *          lexicographic ascending order so that zero-padded filenames play
 *          in the intended sequence.  Results are echoed to Serial.
 *
 * @post    @c tracks contains every discovered MP3 path, sorted alphabetically.
 *          @c tracks is empty if the directory is missing or contains no MP3 files.
 */
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

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------

/**
 * @brief  Stops any current playback and starts the track at @p index.
 *
 * @details Performs range-wrapping on @p index (negative wraps to the last
 *          track; past-end wraps to the first), updates @c currentTrackIndex,
 *          stops the current audio stream, and calls audio.connecttoFS() to
 *          begin streaming from the SD card.  Triggers an immediate display
 *          refresh on success.
 *
 * @param  index  Desired track index in the @c tracks vector.  Out-of-range
 *                values are silently wrapped.
 * @return @c true  if the track was loaded and playback started.
 * @return @c false if the track list is empty or the file could not be opened.
 */
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

/**
 * @brief  Advances to the next track, wrapping from the last track to the first.
 */
void playNextTrack() {
  if (tracks.empty()) {
    return;
  }
  playTrack(currentTrackIndex + 1);
}

/**
 * @brief  Steps back to the previous track, wrapping from the first track to the last.
 */
void playPreviousTrack() {
  if (tracks.empty()) {
    return;
  }
  playTrack(currentTrackIndex - 1);
}

/**
 * @brief  Toggles between playing and paused states.
 *
 * @details If no track has been loaded yet, starts playback from
 *          @c currentTrackIndex instead.  Otherwise, calls
 *          audio.pauseResume() and flips @c playbackPaused.
 */
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

// ---------------------------------------------------------------------------
// Volume control
// ---------------------------------------------------------------------------

/**
 * @brief  Increments the volume by one step, up to PlayerConfig::MAX_VOLUME.
 *         The new level is applied immediately via audio.setVolume().
 */
void volumeUp() {
  if (currentVolume < PlayerConfig::MAX_VOLUME) {
    currentVolume++;
    audio.setVolume(currentVolume);
    Serial.printf("[VOL] %u\n", currentVolume);
  }
}

/**
 * @brief  Decrements the volume by one step, down to PlayerConfig::MIN_VOLUME.
 *         The new level is applied immediately via audio.setVolume().
 */
void volumeDown() {
  if (currentVolume > PlayerConfig::MIN_VOLUME) {
    currentVolume--;
    audio.setVolume(currentVolume);
    Serial.printf("[VOL] %u\n", currentVolume);
  }
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

/**
 * @brief  Polls all five buttons and dispatches the corresponding action.
 *
 * @details Should be called every loop() iteration.  Each DebouncedButton
 *          returns @c true at most once per physical press, so actions are
 *          triggered exactly once per press regardless of how long the button
 *          is held down.
 */
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

// ---------------------------------------------------------------------------
// ESP32-audioI2S callbacks
// ---------------------------------------------------------------------------

/**
 * @brief  Called by the ESP32-audioI2S library when the current MP3 stream ends.
 *
 * @details Sets the @c trackFinished flag, which is checked in loop() to
 *          trigger auto-advance to the next track.  This callback executes in
 *          the audio task context, so only a volatile flag write is performed
 *          here — no direct playback calls.
 *
 * @param  info  Informational string provided by the library (unused).
 */
void audio_eof_mp3(const char* info) {
  (void)info;
  trackFinished = true;
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

/**
 * @brief  One-time initialisation: configures all peripherals in dependency order.
 *
 * @details Execution order:
 *   1. Serial (115200 baud) — enables debug output for all subsequent steps.
 *   2. I2C + SSD1306 OLED — shows a boot splash ("Dark Hour ready").
 *   3. All five DebouncedButton instances — configures INPUT_PULLUP on each pin.
 *   4. SPI + SD card — halts with an error loop if the card is not detected.
 *   5. I2S DAC (Audio) — sets pin-out and initial volume.
 *   6. Track list scan — loads and sorts MP3 paths from the SD card.
 *   7. Playback — starts the first track automatically if any are found.
 */
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

/**
 * @brief  Main execution loop — runs continuously after setup() returns.
 *
 * @details Each iteration:
 *   1. audio.loop()       — feeds the I2S DMA buffer and drives the decoder.
 *   2. handleButtons()    — polls debounced inputs and dispatches actions.
 *   3. updateDisplay()    — redraws the OLED if the refresh interval has elapsed.
 *   4. Auto-advance check — if @c trackFinished was set by the EOF callback,
 *                           clears the flag and begins the next track.
 *
 * @note   audio.loop() must be called as frequently as possible to prevent
 *         audio buffer underruns and playback interruptions.  Avoid adding
 *         blocking delays inside loop().
 */
void loop() {
  audio.loop();
  handleButtons();
  updateDisplay();

  if (trackFinished) {
    trackFinished = false;
    playNextTrack();
  }
}
