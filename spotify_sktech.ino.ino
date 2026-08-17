#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

// ---------- USER CONFIG ----------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* CLIENT_ID     = "YOUR_SPOTIFY_CLIENT_ID";
const char* CLIENT_SECRET = "YOUR_SPOTIFY_CLIENT_SECRET";
const char* REFRESH_TOKEN = "YOUR_SPOTIFY_REFRESH_TOKEN";
// ----------------------------------

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Button pins
#define BTN_PREV 32
#define BTN_PLAY 33
#define BTN_NEXT 25

// Potentiometer pin
#define POT_PIN 34

String accessToken = "";
unsigned long tokenExpiryMillis = 0;

unsigned long lastPollMillis = 0;
const unsigned long POLL_INTERVAL = 5000; // refresh "now playing" every 5s

// Debounce state
unsigned long lastPrevPress = 0, lastPlayPress = 0, lastNextPress = 0;
const unsigned long DEBOUNCE_MS = 300;

// Volume state
int lastSentVolume = -1;
unsigned long lastVolumeCheck = 0;
const unsigned long VOLUME_CHECK_INTERVAL = 200; // how often we sample the pot
const int VOLUME_DEADZONE = 2; // percent change required before sending a new command

void setup() {
  Serial.begin(115200);

  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BTN_PLAY, INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);
  analogReadResolution(12); // ESP32 ADC is 12-bit (0-4095)

  Wire.begin(21, 22); // SDA, SCL
  if (!display.begin(0x3C, true)) {
    Serial.println("OLED init failed");
    for (;;);
  }
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  showMessage("Connecting WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  refreshAccessToken();
}

void loop() {
  handleButtons();
  handleVolume();

  if (millis() - lastPollMillis > POLL_INTERVAL) {
    lastPollMillis = millis();
    if (millis() > tokenExpiryMillis) refreshAccessToken();
    fetchNowPlaying();
  }
}

// ---------- Token handling ----------
void refreshAccessToken() {
  WiFiClientSecure client;
  client.setInsecure(); // for simplicity; use certificate pinning for production
  HTTPClient https;

  https.begin(client, "https://accounts.spotify.com/api/token");
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "grant_type=refresh_token&refresh_token=" + String(REFRESH_TOKEN) +
                "&client_id=" + String(CLIENT_ID) +
                "&client_secret=" + String(CLIENT_SECRET);

  int code = https.POST(body);
  if (code == 200) {
    String payload = https.getString();
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, payload);
    accessToken = doc["access_token"].as<String>();
    int expiresIn = doc["expires_in"] | 3600;
    tokenExpiryMillis = millis() + (expiresIn - 60) * 1000UL; // refresh 1 min early
    Serial.println("Access token refreshed");
  } else {
    Serial.printf("Token refresh failed: %d\n", code);
  }
  https.end();
}

// ---------- Now Playing ----------
void fetchNowPlaying() {
  if (accessToken == "") return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  https.begin(client, "https://api.spotify.com/v1/me/player/currently-playing");
  https.addHeader("Authorization", "Bearer " + accessToken);

  int code = https.GET();
  if (code == 200) {
    String payload = https.getString();
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      String track  = doc["item"]["name"] | "Unknown";
      String artist = doc["item"]["artists"][0]["name"] | "Unknown";
      long progress = doc["progress_ms"] | 0;
      long duration = doc["item"]["duration_ms"] | 1;
      bool playing  = doc["is_playing"] | false;
      updateDisplay(track, artist, progress, duration, playing);
    }
  } else if (code == 204) {
    showMessage("Nothing playing");
  } else {
    Serial.printf("Now playing fetch failed: %d\n", code);
  }
  https.end();
}

void updateDisplay(String track, String artist, long progress, long duration, bool playing) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(playing ? "Playing:" : "Paused:");

  display.setCursor(0, 14);
  display.println(track.substring(0, 21));

  display.setCursor(0, 26);
  display.println(artist.substring(0, 21));

  // Progress bar
  int barWidth = map(progress, 0, duration, 0, SCREEN_WIDTH - 4);
  display.drawRect(0, 45, SCREEN_WIDTH, 8, SH110X_WHITE);
  display.fillRect(2, 47, barWidth, 4, SH110X_WHITE);

  display.display();
}

void showMessage(String msg) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 28);
  display.println(msg);
  display.display();
}

// ---------- Buttons ----------
void handleButtons() {
  if (digitalRead(BTN_PREV) == LOW && millis() - lastPrevPress > DEBOUNCE_MS) {
    lastPrevPress = millis();
    sendCommand("previous", "POST");
  }
  if (digitalRead(BTN_PLAY) == LOW && millis() - lastPlayPress > DEBOUNCE_MS) {
    lastPlayPress = millis();
    togglePlayPause();
  }
  if (digitalRead(BTN_NEXT) == LOW && millis() - lastNextPress > DEBOUNCE_MS) {
    lastNextPress = millis();
    sendCommand("next", "POST");
  }
}

// ---------- Volume (potentiometer) ----------
void handleVolume() {
  if (millis() - lastVolumeCheck < VOLUME_CHECK_INTERVAL) return;
  lastVolumeCheck = millis();

  int raw = analogRead(POT_PIN);           // 0-4095
  int volumePercent = map(raw, 0, 4095, 0, 100);
  volumePercent = constrain(volumePercent, 0, 100);

  if (lastSentVolume == -1 || abs(volumePercent - lastSentVolume) >= VOLUME_DEADZONE) {
    lastSentVolume = volumePercent;
    setVolume(volumePercent);
  }
}

void setVolume(int percent) {
  if (millis() > tokenExpiryMillis) refreshAccessToken();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = "https://api.spotify.com/v1/me/player/volume?volume_percent=" + String(percent);
  https.begin(client, url);
  https.addHeader("Authorization", "Bearer " + accessToken);
  https.addHeader("Content-Length", "0");

  int code = https.PUT("");
  Serial.printf("Set volume %d%% -> %d\n", percent, code);
  https.end();
}

void togglePlayPause() {
  // Quick check of current state, then toggle
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://api.spotify.com/v1/me/player/currently-playing");
  https.addHeader("Authorization", "Bearer " + accessToken);
  int code = https.GET();
  bool playing = false;
  if (code == 200) {
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, https.getString());
    playing = doc["is_playing"] | false;
  }
  https.end();

  sendCommand(playing ? "pause" : "play", "PUT");
}

void sendCommand(String endpoint, String method) {
  if (millis() > tokenExpiryMillis) refreshAccessToken();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = "https://api.spotify.com/v1/me/player/" + endpoint;
  https.begin(client, url);
  https.addHeader("Authorization", "Bearer " + accessToken);
  https.addHeader("Content-Length", "0");

  int code;
  if (method == "POST") code = https.POST("");
  else code = https.PUT("");

  Serial.printf("%s -> %d\n", endpoint.c_str(), code);
  https.end();

  delay(200);
  fetchNowPlaying(); // refresh display right after a command
}