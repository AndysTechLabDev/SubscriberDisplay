/*
  Subscriber Display - ESP32-C3 SuperMini + 2.79\" NV3007 142x428

  Wiring used by this sketch:
    TFT GND -> GND
    TFT VDD -> 5V
    TFT SCL -> GPIO6
    TFT SDA -> GPIO7  (SPI MOSI)
    TFT RES -> GPIO4
    TFT DC  -> GPIO5
    TFT CS  -> GPIO10
    TFT BL  -> GPIO3  (PWM)

    Button A -> GPIO0 -> button -> GND
    Button B -> GPIO1 -> button -> GND

  Required Arduino libraries:
    - Arduino_GFX by moononournation
    - U8g2 by olikraus

  ESP32 board package:
    - esp32 by Espressif Systems, current 3.x release recommended

  Notes:
    - Button 1 logical function is fixed:
        tap         = Light Sleep
        hold 5 sec  = Deep Sleep ("shutdown") after release
    - Button 2 tap toggles subscriber count format: 10.000 <-> 10k.
    - "Swap buttons" swaps GPIO0/GPIO1 logically, so wiring side does not matter.
    - Deep sleep is not a true electrical power-off; it is the ESP32's lowest-power sleep state.
*/

#include <Arduino.h>
#include <esp_arduino_version.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>
#include <esp_sleep.h>
#include <esp_idf_version.h>
#include <driver/gpio.h>
#include <time.h>

// -----------------------------------------------------------------------------
// Firmware
// -----------------------------------------------------------------------------
static const char *FW_VERSION = "0.9.4";

// -----------------------------------------------------------------------------
// Hardware pins
// -----------------------------------------------------------------------------
static constexpr uint8_t PIN_BTN_A = 0;
static constexpr uint8_t PIN_BTN_B = 1;
static constexpr uint8_t PIN_TFT_BL = 3;
static constexpr uint8_t PIN_TFT_RES = 4;
static constexpr uint8_t PIN_TFT_DC = 5;
static constexpr uint8_t PIN_TFT_SCL = 6;
static constexpr uint8_t PIN_TFT_SDA = 7;
static constexpr uint8_t PIN_TFT_CS = 10;

static constexpr uint32_t TFT_SPI_HZ = 30000000;
static constexpr uint32_t LONG_HOLD_MS = 5000;
static constexpr uint32_t DEBOUNCE_MS = 35;

// -----------------------------------------------------------------------------
// Display
// -----------------------------------------------------------------------------
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    PIN_TFT_DC,
    PIN_TFT_CS,
    PIN_TFT_SCL,
    PIN_TFT_SDA,
    GFX_NOT_DEFINED,
    FSPI,
    true);

Arduino_GFX *gfx = new Arduino_NV3007(
    bus,
    PIN_TFT_RES,
    1, // landscape: logical 428 x 142
    false,
    142,
    428,
    12,
    0,
    14,
    0,
    nv3007_279_init_operations,
    sizeof(nv3007_279_init_operations));

// -----------------------------------------------------------------------------
// Web / storage
// -----------------------------------------------------------------------------
WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------
enum NumberFormat : uint8_t {
  NUMBER_LONG = 0,
  NUMBER_SHORT = 1
};

struct AppConfig {
  bool provisioned = false;

  String wifiSsid;
  String wifiPassword;
  bool dhcp = true;
  String staticIp = "192.168.1.50";
  String gateway = "192.168.1.1";
  String subnet = "255.255.255.0";
  String dns = "1.1.1.1";
  String hostname = "subscriberdisplay";

  bool swapButtons = false;
  String youtubeChannelRef;       // @handle, UC... channel id, or legacy username
  String youtubeApiKey;
  uint32_t youtubeRefreshSec = 60;
  bool showYoutubeLogo = true;
  NumberFormat numberFormat = NUMBER_LONG;
  bool showChannelName = true;
  uint16_t channelNameEverySec = 15;

  bool showClock = false;
  String timezone = "Europe/Vienna";
  bool clockAmPm = false;
  uint16_t clockDisplaySec = 5;
  bool showDeviceName = true;
  uint8_t fontStyle = 0;

  uint32_t fontColor = 0xFFFFFF;
  uint32_t backgroundColor = 0x000000;
  uint8_t brightness = 85;
};

AppConfig cfg;

// -----------------------------------------------------------------------------
// Runtime YouTube data
// -----------------------------------------------------------------------------
struct YouTubeData {
  bool valid = false;
  String channelId;
  String channelName;
  uint64_t subscribers = 0;
  uint64_t views = 0;
  bool subscribersHidden = false;

  String lastError;
  uint32_t lastUpdateMs = 0;
};

YouTubeData yt;

// -----------------------------------------------------------------------------
// Runtime display state
// -----------------------------------------------------------------------------
uint32_t lastScreenDrawMs = 0;
uint32_t lastYoutubeFetchMs = 0;
uint32_t youtubeSlideStartedMs = 0;
uint32_t lastChannelNameSlideMs = 0;
uint8_t youtubeSlide = 0; // 0 counter, 1 channel name, 2 clock

// -----------------------------------------------------------------------------
// Button runtime
// -----------------------------------------------------------------------------
struct ButtonState {
  bool stable = HIGH;
  bool lastRaw = HIGH;
  uint32_t rawChangedMs = 0;
  uint32_t pressedMs = 0;
  bool longReached = false;
};

ButtonState physicalA;
ButtonState physicalB;

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
void saveConfig();
void loadConfig();
void startSetupPortal();
bool connectConfiguredWifi();
void startNormalWebUi();
void drawCurrentScreen(bool force = false);
void drawSetupScreen(const String &ssid);
void setBacklight(uint8_t value);
void enterLightSleep();
void enterDeepSleep();
bool fetchYouTubeData(AppConfig &sourceCfg, YouTubeData &out);
String formatCount(uint64_t value, NumberFormat mode);

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

String urlEncode(const String &input) {
  const char *hex = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < input.length(); ++i) {
    uint8_t c = (uint8_t)input[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

bool parseIp(const String &s, IPAddress &ip) {
  return ip.fromString(s);
}

String chipSuffix() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[7];
  snprintf(buf, sizeof(buf), "%06X", (uint32_t)(mac & 0xFFFFFF));
  return String(buf);
}

String normalizeHostname(String h) {
  h.trim();
  h.toLowerCase();
  h.replace(".local", "");
  String out;
  for (size_t i = 0; i < h.length(); ++i) {
    char c = h[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') out += c;
  }
  if (out.length() < 1) out = "subscriberdisplay";
  if (out.length() > 31) out = out.substring(0, 31);
  return out;
}

String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

bool jsonStringValue(const String &json, const char *key, String &value) {
  String token = "\"" + String(key) + "\"";
  int p = json.indexOf(token);
  if (p < 0) return false;
  p = json.indexOf(':', p + token.length());
  if (p < 0) return false;
  p++;
  while (p < (int)json.length() && isspace((unsigned char)json[p])) p++;
  if (p >= (int)json.length() || json[p] != '"') return false;
  p++;

  value = "";
  while (p < (int)json.length()) {
    char c = json[p++];
    if (c == '"') return true;
    if (c == '\\' && p < (int)json.length()) {
      char e = json[p++];
      switch (e) {
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        case '/': value += '/'; break;
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        default:
          value += e;
          break;
      }
    } else {
      value += c;
    }
  }
  return false;
}

bool jsonBoolValue(const String &json, const char *key, bool &value) {
  String token = "\"" + String(key) + "\"";
  int p = json.indexOf(token);
  if (p < 0) return false;
  p = json.indexOf(':', p + token.length());
  if (p < 0) return false;
  p++;
  while (p < (int)json.length() && isspace((unsigned char)json[p])) p++;
  if (json.startsWith("true", p)) { value = true; return true; }
  if (json.startsWith("false", p)) { value = false; return true; }
  return false;
}

// -----------------------------------------------------------------------------
// Preferences
// -----------------------------------------------------------------------------
void loadConfig() {
  prefs.begin("subdisplay", true);

  cfg.provisioned = prefs.getBool("prov", false);
  cfg.wifiSsid = prefs.getString("ssid", "");
  cfg.wifiPassword = prefs.getString("wpass", "");
  cfg.dhcp = prefs.getBool("dhcp", true);
  cfg.staticIp = prefs.getString("ip", "192.168.1.50");
  cfg.gateway = prefs.getString("gw", "192.168.1.1");
  cfg.subnet = prefs.getString("mask", "255.255.255.0");
  cfg.dns = prefs.getString("dns", "1.1.1.1");
  cfg.hostname = prefs.getString("host", "subscriberdisplay");

  cfg.swapButtons = prefs.getBool("swap", false);
  cfg.youtubeChannelRef = prefs.getString("ytref", "");
  cfg.youtubeApiKey = prefs.getString("ytkey", "");
  cfg.youtubeRefreshSec = prefs.getUInt("ytrefresh", 60);
  cfg.showYoutubeLogo = prefs.getBool("ytlogo", true);
  cfg.numberFormat = (NumberFormat)prefs.getUChar("numfmt", NUMBER_LONG);
  cfg.showChannelName = prefs.getBool("ytname", true);
  cfg.channelNameEverySec = prefs.getUShort("nameint", 15);
  cfg.showClock = prefs.getBool("clock", false);
  cfg.timezone = prefs.getString("tz", "Europe/Vienna");
  cfg.clockAmPm = prefs.getBool("ampm", false);
  cfg.clockDisplaySec = prefs.getUShort("clocksec", 5);
  cfg.showDeviceName = prefs.getBool("devname", true);
  cfg.fontStyle = prefs.getUChar("fontstyle", 0);
  if (cfg.fontStyle > 4) cfg.fontStyle = 0;
  cfg.fontColor = prefs.getUInt("fontc", 0xFFFFFF);
  cfg.backgroundColor = prefs.getUInt("bgc", 0x000000);
  cfg.brightness = prefs.getUChar("bright", 85);
  if (cfg.brightness > 100) cfg.brightness = (uint8_t)map(cfg.brightness, 1, 255, 1, 100);
  cfg.brightness = constrain(cfg.brightness, 1, 100);

  prefs.end();
  cfg.hostname = normalizeHostname(cfg.hostname);
}

void saveConfig() {
  prefs.begin("subdisplay", false);

  prefs.putBool("prov", cfg.provisioned);
  prefs.putString("ssid", cfg.wifiSsid);
  prefs.putString("wpass", cfg.wifiPassword);
  prefs.putBool("dhcp", cfg.dhcp);
  prefs.putString("ip", cfg.staticIp);
  prefs.putString("gw", cfg.gateway);
  prefs.putString("mask", cfg.subnet);
  prefs.putString("dns", cfg.dns);
  prefs.putString("host", cfg.hostname);

  prefs.putBool("swap", cfg.swapButtons);
  prefs.putString("ytref", cfg.youtubeChannelRef);
  prefs.putString("ytkey", cfg.youtubeApiKey);
  prefs.putUInt("ytrefresh", cfg.youtubeRefreshSec);
  prefs.putBool("ytlogo", cfg.showYoutubeLogo);
  prefs.putUChar("numfmt", (uint8_t)cfg.numberFormat);
  prefs.putBool("ytname", cfg.showChannelName);
  prefs.putUShort("nameint", cfg.channelNameEverySec);
  prefs.putBool("clock", cfg.showClock);
  prefs.putString("tz", cfg.timezone);
  prefs.putBool("ampm", cfg.clockAmPm);
  prefs.putUShort("clocksec", cfg.clockDisplaySec);
  prefs.putBool("devname", cfg.showDeviceName);
  prefs.putUChar("fontstyle", cfg.fontStyle);
  prefs.putUInt("fontc", cfg.fontColor);
  prefs.putUInt("bgc", cfg.backgroundColor);
  prefs.putUChar("bright", cfg.brightness);

  prefs.end();
}

void factoryReset() {
  prefs.begin("subdisplay", false);
  prefs.clear();
  prefs.end();
  delay(250);
  ESP.restart();
}

// -----------------------------------------------------------------------------
// Backlight
// -----------------------------------------------------------------------------
void initBacklight() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_TFT_BL, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_TFT_BL, 0);
#endif
}

void setBacklight(uint8_t percent) {
  uint8_t duty = 0;
  if (percent > 0) {
    percent = constrain(percent, 1, 100);
    duty = (uint8_t)map(percent, 1, 100, 1, 255);
  }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_TFT_BL, duty);
#else
  ledcWrite(0, duty);
#endif
}

// -----------------------------------------------------------------------------
// Display helpers
// -----------------------------------------------------------------------------
void centerText(const String &text, int y, uint8_t size, uint16_t color = RGB565_WHITE) {
  gfx->setFont((const GFXfont *)nullptr);
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int x = ((int)gfx->width() - (int)w) / 2;
  if (x < 0) x = 0;
  gfx->setCursor(x, y);
  gfx->print(text);
}

void drawYoutubeLogo(int x, int y) {
  gfx->fillRoundRect(x, y, 54, 36, 9, RGB565_RED);
  int16_t px = x + 22;
  int16_t py = y + 9;
  gfx->fillTriangle(px, py, px, py + 18, px + 16, py + 9, RGB565_WHITE);
}

void drawSetupScreen(const String &ssid) {
  gfx->fillScreen(RGB565_BLACK);
  centerText("SETUP", 6, 3, RGB565_WHITE);
  centerText("Connect to Wi-Fi:", 42, 1, RGB565_LIGHTGREY);
  centerText(ssid, 59, 2, RGB565_CYAN);
  centerText("If setup does not open automatically:", 94, 1, RGB565_LIGHTGREY);
  centerText("open 192.168.4.1 in your browser", 111, 1, RGB565_WHITE);
}

void drawMessage(const String &line1, const String &line2 = "") {
  gfx->fillScreen(RGB565_BLACK);
  centerText(line1, 44, 2, RGB565_WHITE);
  if (line2.length()) centerText(line2, 82, 1, RGB565_LIGHTGREY);
}

String formatLong(uint64_t value) {
  String raw = String(value);
  String out;
  int first = raw.length() % 3;
  if (first == 0) first = 3;
  for (int i = 0; i < (int)raw.length(); ++i) {
    if (i > 0 && ((i - first) % 3 == 0)) out += '.';
    out += raw[i];
  }
  return out;
}

String formatShort(uint64_t value) {
  if (value < 1000) return String(value);
  const char *suffixes[] = {"", "k", "M", "B", "T"};
  double v = (double)value;
  int idx = 0;
  while (v >= 1000.0 && idx < 4) {
    v /= 1000.0;
    idx++;
  }
  char buf[24];
  if (v >= 100.0 || fabs(v - round(v)) < 0.05) snprintf(buf, sizeof(buf), "%.0f%s", v, suffixes[idx]);
  else if (v >= 10.0) snprintf(buf, sizeof(buf), "%.1f%s", v, suffixes[idx]);
  else snprintf(buf, sizeof(buf), "%.2f%s", v, suffixes[idx]);
  return String(buf);
}

String formatCount(uint64_t value, NumberFormat mode) {
  return mode == NUMBER_SHORT ? formatShort(value) : formatLong(value);
}

void drawYoutubeLogoSized(int x, int y, int w, int h, uint16_t body = RGB565_RED, uint16_t play = RGB565_WHITE) {
  int radius = max(4, h / 4);
  gfx->fillRoundRect(x, y, w, h, radius, body);
  int triH = max(10, h / 2);
  int triW = max(8, triH * 3 / 4);
  int px = x + (w - triW) / 2 - 1;
  int py = y + (h - triH) / 2;
  gfx->fillTriangle(px, py, px, py + triH, px + triW, py + triH / 2, play);
}

void setValueFont(uint8_t sizeHint) {
  if (cfg.fontStyle == 0) {
    gfx->setFont((const GFXfont *)nullptr);
    gfx->setTextSize(sizeHint);
    return;
  }

  uint8_t scale = 1;
  bool medium = sizeHint >= 4 && sizeHint <= 5;
  bool large = sizeHint >= 6;

  switch (cfg.fontStyle) {
    case 1:
      gfx->setFont(medium ? u8g2_font_helvB18_tf : u8g2_font_helvB24_tf);
      break;
    case 2:
      gfx->setFont(medium ? u8g2_font_courB18_tf : u8g2_font_courB24_tf);
      break;
    case 3:
      gfx->setFont(medium ? u8g2_font_ncenB18_tf : u8g2_font_ncenB24_tf);
      break;
    case 4:
      gfx->setFont(large ? u8g2_font_mystery_quest_32_tr :
                   (medium ? u8g2_font_mystery_quest_28_tr : u8g2_font_mystery_quest_24_tr));
      break;
    default:
      gfx->setFont((const GFXfont *)nullptr);
      gfx->setTextSize(sizeHint);
      return;
  }

  if (large || medium) scale = 2;
  gfx->setTextSize(scale);
}

void drawCenteredLogoText(const String &text, int centerY, uint8_t size, uint16_t color,
                          uint16_t logoBody = RGB565_RED, uint16_t logoPlay = RGB565_WHITE) {
  uint8_t fittedSize = size;
  int16_t x1 = 0, y1 = 0;
  uint16_t tw = 0, th = 0;
  int logoH = 0, logoW = 0, totalW = 0;
  const int gap = 10;
  const int maxWidth = gfx->width() - 8;

  while (true) {
    setValueFont(fittedSize);
    gfx->getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
    logoH = cfg.showYoutubeLogo ? max(24, (int)th) : 0;
    logoW = cfg.showYoutubeLogo ? logoH * 3 / 2 : 0;
    totalW = (int)tw + (cfg.showYoutubeLogo ? logoW + gap : 0);
    if (totalW <= maxWidth || fittedSize <= 1) break;
    --fittedSize;
  }

  int textY = centerY - ((int)th / 2) - y1;

  if (!cfg.showYoutubeLogo) {
    int x = ((int)gfx->width() - (int)tw) / 2 - x1;
    if (x < 0) x = 0;
    gfx->setTextColor(color);
    gfx->setCursor(x, textY);
    gfx->print(text);
    return;
  }

  int startX = ((int)gfx->width() - totalW) / 2;
  if (startX < 0) startX = 0;

  int logoY = centerY - logoH / 2;
  drawYoutubeLogoSized(startX, logoY, logoW, logoH, logoBody, logoPlay);

  gfx->setTextColor(color);
  gfx->setCursor(startX + logoW + gap - x1, textY);
  gfx->print(text);
}

String timezoneToPosix(String zone) {
  zone.trim();
  if (!zone.length()) zone = "Europe/Vienna";

  String z = zone;
  z.toLowerCase();

  if (z == "europe/vienna") return "CET-1CEST,M3.5.0/2,M10.5.0/3";

  // hi mom
  if (z == "europe/berlin" || z == "europe/paris" || z == "europe/rome" ||
      z == "europe/prague" || z == "europe/warsaw" || z == "europe/zurich")
    return "CET-1CEST,M3.5.0/2,M10.5.0/3";
  if (z == "europe/london") return "GMT0BST,M3.5.0/1,M10.5.0/2";
  if (z == "utc" || z == "etc/utc" || z == "gmt") return "UTC0";

  return zone;
}

void applyTimezone() {
  String tz = timezoneToPosix(cfg.timezone);
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

uint16_t color565(uint32_t rgb) {
  uint8_t r = (rgb >> 16) & 0xFF;
  uint8_t g = (rgb >> 8) & 0xFF;
  uint8_t b = rgb & 0xFF;
  return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

uint16_t blendedColor565(uint32_t from, uint32_t to, int step, int steps) {
  uint8_t fr = (from >> 16) & 0xFF, fg = (from >> 8) & 0xFF, fb = from & 0xFF;
  uint8_t tr = (to >> 16) & 0xFF, tg = (to >> 8) & 0xFF, tb = to & 0xFF;
  uint8_t r = fr + ((int)(tr - fr) * step) / steps;
  uint8_t g = fg + ((int)(tg - fg) * step) / steps;
  uint8_t b = fb + ((int)(tb - fb) * step) / steps;
  return color565(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
}

String colorHex(uint32_t rgb) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%06lX", (unsigned long)(rgb & 0xFFFFFF));
  return String(buf);
}

uint32_t parseHexColor(String value, uint32_t fallback) {
  value.trim();
  if (value.startsWith("#")) value.remove(0, 1);
  if (value.length() != 6) return fallback;
  for (size_t i = 0; i < value.length(); ++i) {
    if (!isxdigit((unsigned char)value[i])) return fallback;
  }
  return strtoul(value.c_str(), nullptr, 16) & 0xFFFFFF;
}

int contentCenterY() {
  return cfg.showDeviceName ? 61 : (gfx->height() / 2);
}

int contentBottomY() {
  return cfg.showDeviceName ? 119 : gfx->height();
}

void clearMainContent() {
  gfx->fillRect(0, 0, gfx->width(), contentBottomY(), color565(cfg.backgroundColor));
}

void drawDeviceNameFooter() {
  if (!cfg.showDeviceName) return;
  gfx->fillRect(0, 120, gfx->width(), gfx->height() - 120, color565(cfg.backgroundColor));
  String footer = WiFi.status() == WL_CONNECTED ? cfg.hostname + ".local" : "offline";
  centerText(footer, 128, 1, blendedColor565(cfg.backgroundColor, cfg.fontColor, 7, 16));
}

void drawYoutubeCounter(bool drawFooter = true) {
  clearMainContent();
  String count = yt.valid ? (yt.subscribersHidden ? "hidden" : formatCount(yt.subscribers, cfg.numberFormat)) : "--";
  uint8_t size = 7;
  if (count.length() > 10) size = 4;
  else if (count.length() > 8) size = 5;
  else if (count.length() > 6) size = 6;
  drawCenteredLogoText(count, contentCenterY(), size, color565(cfg.fontColor));
  if (drawFooter) drawDeviceNameFooter();
}

void drawYoutubeChannelName(bool drawFooter = true) {
  clearMainContent();
  String name = yt.channelName.length() ? yt.channelName : "--";
  uint8_t size = 4;
  if (name.length() > 24) size = 2;
  else if (name.length() > 15) size = 3;
  drawCenteredLogoText(name, contentCenterY(), size, color565(cfg.fontColor));
  if (drawFooter) drawDeviceNameFooter();
}

void drawClockValue(uint16_t color) {
  const int cy = contentCenterY();
  struct tm t;
  if (!getLocalTime(&t, 50)) {
    centerText("Waiting for time", cy - 8, 2, color);
    return;
  }

  char timeBuf[8];
  strftime(timeBuf, sizeof(timeBuf), cfg.clockAmPm ? "%I:%M" : "%H:%M", &t);
  String timeText(timeBuf);
  if (cfg.clockAmPm && timeText.startsWith("0")) timeText.remove(0, 1);

  setValueFont(7);
  int16_t x1, y1; uint16_t tw, th;
  gfx->getTextBounds(timeText, 0, 0, &x1, &y1, &tw, &th);

  if (!cfg.clockAmPm) {
    int x = ((int)gfx->width() - (int)tw) / 2 - x1;
    int y = cy - ((int)th / 2) - y1;
    gfx->setTextColor(color);
    gfx->setCursor(x, y);
    gfx->print(timeText);
    return;
  }

  char apBuf[4];
  strftime(apBuf, sizeof(apBuf), "%p", &t);
  gfx->setFont((const GFXfont *)nullptr);
  gfx->setTextSize(2);
  int16_t ax1, ay1; uint16_t aw, ah;
  gfx->getTextBounds(String(apBuf), 0, 0, &ax1, &ay1, &aw, &ah);

  int gap = 10;
  int total = (int)tw + gap + (int)aw;
  int x = (gfx->width() - total) / 2;
  int y = cy - ((int)th / 2) - y1;

  setValueFont(7);
  gfx->setTextColor(color);
  gfx->setCursor(x - x1, y);
  gfx->print(timeText);

  gfx->setFont((const GFXfont *)nullptr);
  gfx->setTextSize(2);
  gfx->setCursor(x + tw + gap - ax1, cy - ah / 2 - ay1 + 8);
  gfx->print(apBuf);
}

void drawClockScreen(bool drawFooter = true) {
  clearMainContent();
  drawClockValue(color565(cfg.fontColor));
  if (drawFooter) drawDeviceNameFooter();
}

void drawSlideWithBlend(uint8_t slide, int step, int steps, bool clearFirst, bool drawFooter) {
  if (clearFirst) clearMainContent();

  uint16_t textColor = blendedColor565(cfg.backgroundColor, cfg.fontColor, step, steps);
  uint16_t logoBody = blendedColor565(cfg.backgroundColor, 0xFF0000, step, steps);
  uint16_t logoPlay = blendedColor565(cfg.backgroundColor, 0xFFFFFF, step, steps);

  if (slide == 1) {
    String name = yt.channelName.length() ? yt.channelName : "--";
    uint8_t size = name.length() > 24 ? 2 : (name.length() > 15 ? 3 : 4);
    drawCenteredLogoText(name, contentCenterY(), size, textColor, logoBody, logoPlay);
  } else if (slide == 2) {
    drawClockValue(textColor);
  } else {
    String count = yt.valid ? (yt.subscribersHidden ? "hidden" : formatCount(yt.subscribers, cfg.numberFormat)) : "--";
    uint8_t size = 7;
    if (count.length() > 10) size = 4;
    else if (count.length() > 8) size = 5;
    else if (count.length() > 6) size = 6;
    drawCenteredLogoText(count, contentCenterY(), size, textColor, logoBody, logoPlay);
  }

  if (drawFooter) drawDeviceNameFooter();
}

void animateYoutubeTransition(uint8_t fromSlide, uint8_t toSlide) {
  const int frames = 16;
  const int blendSteps = 256;
  const int frameDelayMs = 8;

  for (int i = frames; i >= 0; --i) {
    int t = (i * 256) / frames;
    int eased = (t * t * (768 - 2 * t)) >> 16; // smoothstep, 0..256
    drawSlideWithBlend(fromSlide, eased, blendSteps, false, false);
    delay(frameDelayMs);
  }

  clearMainContent();

  for (int i = 0; i <= frames; ++i) {
    int t = (i * 256) / frames;
    int eased = (t * t * (768 - 2 * t)) >> 16; // smoothstep, 0..256
    drawSlideWithBlend(toSlide, eased, blendSteps, false, false);
    delay(frameDelayMs);
  }
}

void drawCurrentScreen(bool force) {
  uint32_t now = millis();
  uint8_t previousSlide = youtubeSlide;
  bool slideChanged = false;

  if (yt.valid) {
    uint32_t elapsedMs = now - youtubeSlideStartedMs;

    if (youtubeSlide == 0) {
      uint32_t nextSec = 0;
      if (cfg.showChannelName) nextSec = cfg.channelNameEverySec;
      else if (cfg.showClock) nextSec = cfg.clockDisplaySec;

      if (nextSec > 0 && elapsedMs >= nextSec * 1000UL) {
        youtubeSlide = cfg.showChannelName ? 1 : 2;
        youtubeSlideStartedMs = now;
        slideChanged = true;
      }
    } else if (youtubeSlide == 1) {
      // Channel screen stays visible for its configured interval.
      if (elapsedMs >= (uint32_t)cfg.channelNameEverySec * 1000UL) {
        youtubeSlide = cfg.showClock ? 2 : 0;
        youtubeSlideStartedMs = now;
        slideChanged = true;
      }
    } else if (youtubeSlide == 2) {
      // Clock screen stays visible for its configured interval.
      if (elapsedMs >= (uint32_t)cfg.clockDisplaySec * 1000UL) {
        youtubeSlide = 0;
        youtubeSlideStartedMs = now;
        slideChanged = true;
      }
    }
  }

  if (!force && !slideChanged) return;
  lastScreenDrawMs = now;

  if (!yt.valid) {
    gfx->fillScreen(RGB565_BLACK);
    if (cfg.showYoutubeLogo) drawYoutubeLogo(12, 12);
    centerText("YouTube", 18, 2, RGB565_WHITE);
    centerText(yt.lastError.length() ? yt.lastError : "Not configured", 63, 1, RGB565_LIGHTGREY);
    centerText("Open web interface:", 93, 1, RGB565_DARKGREY);
    centerText(cfg.hostname + ".local", 111, 2, RGB565_WHITE);
    return;
  }

  if (slideChanged && !force) {
    animateYoutubeTransition(previousSlide, youtubeSlide);
  } else if (youtubeSlide == 1) {
    drawYoutubeChannelName();
  } else if (youtubeSlide == 2) {
    drawClockScreen();
  } else {
    drawYoutubeCounter();
  }
}
// -----------------------------------------------------------------------------
// HTTPS helpers
// -----------------------------------------------------------------------------
bool httpsGet(const String &url, String &body, int &status) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    status = -1;
    return false;
  }
  status = http.GET();
  if (status > 0) body = http.getString();
  http.end();
  return status >= 200 && status < 300;
}

// -----------------------------------------------------------------------------
// YouTube Data API
// -----------------------------------------------------------------------------
bool parseYouTubeChannelResponse(const String &payload, YouTubeData &out) {
  String id, title, subs, views;
  if (!jsonStringValue(payload, "id", id)) {
    out.lastError = "Channel not found";
    return false;
  }
  if (!jsonStringValue(payload, "title", title)) title = "";
  if (!jsonStringValue(payload, "subscriberCount", subs)) subs = "0";
  if (!jsonStringValue(payload, "viewCount", views)) views = "0";

  bool hidden = false;
  jsonBoolValue(payload, "hiddenSubscriberCount", hidden);

  out.channelId = id;
  out.channelName = title;
  out.subscribersHidden = hidden;
  out.subscribers = strtoull(subs.c_str(), nullptr, 10);
  out.views = strtoull(views.c_str(), nullptr, 10);
  out.valid = true;
  out.lastError = "";
  return true;
}

bool fetchPublicYouTube(AppConfig &sourceCfg, YouTubeData &out) {
  if (WiFi.status() != WL_CONNECTED) {
    out.lastError = "WiFi offline";
    return false;
  }
  if (sourceCfg.youtubeApiKey.length() < 10 || sourceCfg.youtubeChannelRef.length() < 1) {
    out.lastError = "YouTube not configured";
    return false;
  }

  String ref = sourceCfg.youtubeChannelRef;
  ref.trim();

  String filter;
  if (ref.startsWith("UC") && ref.length() >= 20) {
    filter = "id=" + urlEncode(ref);
  } else {
    filter = "forHandle=" + urlEncode(ref);
  }

  String url = "https://www.googleapis.com/youtube/v3/channels?part=snippet%2Cstatistics&" +
               filter + "&key=" + urlEncode(sourceCfg.youtubeApiKey);

  String body;
  int status = 0;
  bool ok = httpsGet(url, body, status);
  if (!ok) {
    out.lastError = "YouTube HTTP " + String(status);
    return false;
  }
  if (parseYouTubeChannelResponse(body, out)) return true;

  // Compatibility fallback for old legacy usernames.
  if (!ref.startsWith("UC")) {
    url = "https://www.googleapis.com/youtube/v3/channels?part=snippet%2Cstatistics&forUsername=" +
          urlEncode(ref) + "&key=" + urlEncode(sourceCfg.youtubeApiKey);
    body = "";
    ok = httpsGet(url, body, status);
    if (ok && parseYouTubeChannelResponse(body, out)) return true;
  }

  return false;
}

bool fetchYouTubeData(AppConfig &sourceCfg, YouTubeData &out) {
  YouTubeData fresh;
  bool ok = fetchPublicYouTube(sourceCfg, fresh);
  if (!ok) {
    out.lastError = fresh.lastError;
    return false;
  }

  fresh.lastUpdateMs = millis();
  out = fresh;
  return true;
}

// -----------------------------------------------------------------------------
// WiFi / mDNS
// -----------------------------------------------------------------------------
bool connectConfiguredWifi() {
  if (!cfg.provisioned || !cfg.wifiSsid.length()) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.hostname.c_str());

  if (!cfg.dhcp) {
    IPAddress ip, gw, mask, dns;
    if (!parseIp(cfg.staticIp, ip) || !parseIp(cfg.gateway, gw) ||
        !parseIp(cfg.subnet, mask) || !parseIp(cfg.dns, dns)) {
      Serial.println("Static IP configuration invalid");
      return false;
    }
    if (!WiFi.config(ip, gw, mask, dns)) {
      Serial.println("WiFi.config failed");
      return false;
    }
  }

  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPassword.c_str());
  drawMessage("Connecting WiFi", cfg.wifiSsid);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) return false;

  if (MDNS.begin(cfg.hostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  applyTimezone();

  return true;
}

// -----------------------------------------------------------------------------
// Setup captive portal HTML
// -----------------------------------------------------------------------------
String commonCss() {
  return R"CSS(
<style>
:root{color-scheme:dark;--bg:#0d1117;--card:#161b22;--line:#30363d;--fg:#f0f6fc;--muted:#8b949e;--accent:#2f81f7;--danger:#da3633}
*{box-sizing:border-box} body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg);margin:0;padding:18px}
main{max-width:720px;margin:0 auto} .card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:18px;margin:0 0 14px}
h1{font-size:25px;margin:0 0 8px} h2{font-size:19px;margin:0 0 14px} h3{font-size:16px;margin:18px 0 8px} p,.hint{color:var(--muted);line-height:1.45}.row{display:grid;grid-template-columns:1fr 1fr;gap:12px}
label{display:block;font-weight:600;margin:10px 0 5px}input,select{width:100%;padding:11px;border-radius:9px;border:1px solid var(--line);background:#0d1117;color:var(--fg);font-size:15px}
input[type=checkbox],input[type=radio]{width:auto;margin-right:7px}.check{font-weight:400}.buttons{display:flex;gap:10px;flex-wrap:wrap;margin-top:16px}
input[type=color]{height:48px;padding:4px;cursor:pointer;background:#0d1117}
input[type=color]::-webkit-color-swatch-wrapper{padding:0}input[type=color]::-webkit-color-swatch{border:0;border-radius:6px}input[type=color]::-moz-color-swatch{border:0;border-radius:6px}
button,.btn{border:0;border-radius:9px;padding:11px 15px;background:var(--accent);color:white;font-weight:700;text-decoration:none;cursor:pointer}.secondary{background:#30363d}.danger{background:var(--danger)}
nav{position:sticky;top:0;background:rgba(13,17,23,.92);backdrop-filter:blur(8px);padding:8px 0 12px;z-index:5}nav a{color:#c9d1d9;text-decoration:none;margin-right:16px;font-weight:700}
.status{padding:10px;border-radius:9px;background:#0d1117;border:1px solid var(--line);margin-top:10px}.ok{border-color:#238636}.bad{border-color:#da3633}.hidden{display:none}
small{color:var(--muted)}.save-row{margin-bottom:22px}.save-status{align-self:center;color:#3fb950;font-weight:700;opacity:0;transition:opacity .2s ease}.save-status.show{opacity:1}@media(max-width:600px){.row{grid-template-columns:1fr}}
</style>
)CSS";
}

String setupPage() {
  int n = WiFi.scanNetworks(false, true);
  String options;
  for (int i = 0; i < n; ++i) {
    String ssid = WiFi.SSID(i);
    options += "<option value=\"" + htmlEscape(ssid) + "\">" + htmlEscape(ssid) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }

  String html = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Subscriber Display Setup</title>" + commonCss() + "</head><body><main>";
  html += "<div class='card'><h1>Subscriber Display</h1><p>Initial setup</p></div>";
  html += "<form method='POST' action='/setup/save'>";
  html += "<div class='card'><h2>Wi-Fi</h2><label>Network</label><select name='ssid' required>" + options + "</select>";
  html += "<label>Password</label><input type='password' name='password' autocomplete='current-password'>";
  html += "<h3>IP configuration</h3>";
  html += "<label class='check'><input type='radio' name='ipmode' value='dhcp' checked onchange='ipMode()'>DHCP</label>";
  html += "<label class='check'><input type='radio' name='ipmode' value='static' onchange='ipMode()'>Static IP</label>";
  html += "<div id='staticFields' class='hidden'><div class='row'>";
  html += "<div><label>IP address</label><input name='ip' value='192.168.1.50'></div>";
  html += "<div><label>Gateway</label><input name='gateway' value='192.168.1.1'></div>";
  html += "<div><label>Subnet</label><input name='subnet' value='255.255.255.0'></div>";
  html += "<div><label>DNS</label><input name='dns' value='1.1.1.1'></div></div></div>";
  html += "<h3>Device name</h3><label>Hostname</label><input id='hostname' name='hostname' value='subscriberdisplay' oninput='hostChanged()'>";
  html += "<p>Later available at <strong id='localName'>subscriberdisplay.local</strong></p>";
  html += "<div class='buttons'><button type='submit'>Save &amp; restart</button></div></div></form>";
  html += R"JS(<script>
function ipMode(){const s=document.querySelector('input[name=ipmode]:checked').value==='static';document.getElementById('staticFields').classList.toggle('hidden',!s)}
function hostChanged(){let h=document.getElementById('hostname').value.toLowerCase().replace(/\.local$/,'').replace(/[^a-z0-9-]/g,'');document.getElementById('localName').textContent=(h||'subscriberdisplay')+'.local'}
</script>)JS";
  html += "</main></body></html>";
  return html;
}

void captiveRedirect() {
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

void startSetupPortal() {
  WiFi.disconnect(true, true);
  delay(150);
  WiFi.mode(WIFI_AP_STA);

  String apSsid = "SubscriberDisplay-" + chipSuffix();
  WiFi.softAP(apSsid.c_str());
  dnsServer.start(53, "*", WiFi.softAPIP());

  drawSetupScreen(apSsid);

  server.on("/", HTTP_GET, []() { server.send(200, "text/html; charset=utf-8", setupPage()); });
  server.on("/setup/save", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String host = normalizeHostname(server.arg("hostname"));
    bool dhcp = server.arg("ipmode") != "static";

    if (!ssid.length()) {
      server.send(400, "text/plain", "SSID missing");
      return;
    }

    if (!dhcp) {
      IPAddress tmp;
      if (!parseIp(server.arg("ip"), tmp) || !parseIp(server.arg("gateway"), tmp) ||
          !parseIp(server.arg("subnet"), tmp) || !parseIp(server.arg("dns"), tmp)) {
        server.send(400, "text/plain", "Invalid static IP configuration");
        return;
      }
    }

    cfg.wifiSsid = ssid;
    cfg.wifiPassword = server.arg("password");
    cfg.dhcp = dhcp;
    cfg.staticIp = server.arg("ip");
    cfg.gateway = server.arg("gateway");
    cfg.subnet = server.arg("subnet");
    cfg.dns = server.arg("dns");
    cfg.hostname = host;
    cfg.provisioned = true;
    saveConfig();

    String html = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>" + commonCss() + "</head><body><main><div class='card'><h1>Saved</h1><p>The device is restarting.</p><p>Then open: <strong>http://" + htmlEscape(host) + ".local</strong></p></div></main></body></html>";
    server.send(200, "text/html; charset=utf-8", html);
    delay(1200);
    ESP.restart();
  });

  // Captive portal detection.
  server.on("/hotspot-detect.html", HTTP_ANY, []() { server.send(200, "text/html; charset=utf-8", setupPage()); });
  server.on("/library/test/success.html", HTTP_ANY, []() { server.send(200, "text/html; charset=utf-8", setupPage()); });
  server.on("/canonical.html", HTTP_ANY, []() { server.send(200, "text/html; charset=utf-8", setupPage()); });
  server.on("/success.txt", HTTP_ANY, []() { server.send(200, "text/html; charset=utf-8", setupPage()); });
  server.on("/generate_204", HTTP_ANY, captiveRedirect);
  server.on("/gen_204", HTTP_ANY, captiveRedirect);
  server.on("/connecttest.txt", HTTP_ANY, captiveRedirect);
  server.on("/ncsi.txt", HTTP_ANY, captiveRedirect);
  server.on("/fwlink", HTTP_ANY, captiveRedirect);
  server.onNotFound(captiveRedirect);
  server.begin();

  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(2);
  }
}

// -----------------------------------------------------------------------------
// Normal web UI
// -----------------------------------------------------------------------------
String normalPage() {
  String html = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Subscriber Display</title>" + commonCss() + "</head><body><main>";
  html += "<div class='card'><h1>Subscriber Display</h1><p>Hostname: " + htmlEscape(cfg.hostname) + ".local</p></div>";

  html += "<form id='settingsForm' method='POST' action='/save' onsubmit='saveSettings(event)'>";

  // YouTube
  html += "<section id='youtube' class='card'><h2>YouTube</h2>";
  html += "<label>Channel / @Handle / Channel ID</label><input name='ytref' value='" + htmlEscape(cfg.youtubeChannelRef) + "' placeholder='@mychannel or UC...'>";
  html += "<label>API Key</label><input type='password' name='ytkey' value='' placeholder='saved - enter a new value only to change it' autocomplete='off'>";
  html += "<div class='row'><div><label>API Update</label><input type='number' min='30' max='3600' name='ytrefresh' value='" + String(cfg.youtubeRefreshSec) + "'><small>seconds</small></div>";
  html += "<div><label>Connection test</label><button type='button' style='width:100%' onclick='testYoutube()'>Test YouTube</button></div></div>";
  html += "<div id='ytTest' class='status'>Not tested yet.</div>";
  html += "</section>";

  // Screen
  html += "<section id='screen' class='card'><h2>Screen</h2>";
  html += "<div class='row'><div><label>Brightness (%)</label><input type='range' min='1' max='100' name='brightness' value='" + String(cfg.brightness) + "' oninput='brightValue.textContent=this.value'><small>Value: <span id='brightValue'>" + String(cfg.brightness) + "</span>%</small></div>";
  html += "<div><label>Sub counter format</label><label class='check'><input type='radio' name='numfmt' value='long'" + String(cfg.numberFormat == NUMBER_LONG ? " checked" : "") + ">Long: 10.000</label><label class='check'><input type='radio' name='numfmt' value='short'" + String(cfg.numberFormat == NUMBER_SHORT ? " checked" : "") + ">Short: 10k</label></div></div>";
  html += "<label class='check'><input type='checkbox' name='ytlogo'" + String(cfg.showYoutubeLogo ? " checked" : "") + ">Show YouTube logo</label>";
  html += "<label class='check'><input id='ytname' type='checkbox' name='ytname' onchange='channelNameChanged()'" + String(cfg.showChannelName ? " checked" : "") + ">Show channel name</label>";
  html += "<label>Channel display duration</label><input id='nameint' type='number' min='5' max='600' name='nameint' value='" + String(cfg.channelNameEverySec) + "'" + String(cfg.showChannelName ? "" : " disabled") + "><small>seconds</small>";
  html += "<label class='check'><input type='checkbox' name='devname'" + String(cfg.showDeviceName ? " checked" : "") + ">Show hostname</label>";
  html += "</section>";

  // Clock
  html += "<section id='clock' class='card'><h2>Clock</h2>";
  html += "<label class='check'><input id='showclock' type='checkbox' name='showclock' onchange='clockChanged()'" + String(cfg.showClock ? " checked" : "") + ">Show clock</label>";
  html += "<label>Time zone</label><input id='timezone' name='timezone' value='" + htmlEscape(cfg.timezone) + "' placeholder='Europe/Vienna'" + String(cfg.showClock ? "" : " disabled") + "><small>Example: Europe/Vienna (DST is handled automatically)</small>";
  html += "<label class='check'><input id='ampm' type='checkbox' name='ampm'" + String(cfg.clockAmPm ? " checked" : "") + String(cfg.showClock ? "" : " disabled") + ">Use 12-hour AM/PM format</label>";
  html += "<label>Clock display duration</label><input id='clocksec' type='number' min='2' max='60' name='clocksec' value='" + String(cfg.clockDisplaySec) + "'" + String(cfg.showClock ? "" : " disabled") + "><small>seconds</small>";
  html += "</section>";

  // Colors
  html += "<section id='colors' class='card'><h2>Colors</h2>";
  html += "<div class='row'><div><label>Font</label><input id='fontPicker' type='color' value='" + colorHex(cfg.fontColor) + "' oninput='fontHex.value=this.value.toUpperCase()'><input id='fontHex' name='fontcolor' value='" + colorHex(cfg.fontColor) + "' maxlength='7' pattern='#[0-9A-Fa-f]{6}' oninput='syncHex(\"fontHex\",\"fontPicker\")'></div>";
  html += "<div><label>Background</label><input id='bgPicker' type='color' value='" + colorHex(cfg.backgroundColor) + "' oninput='bgHex.value=this.value.toUpperCase()'><input id='bgHex' name='bgcolor' value='" + colorHex(cfg.backgroundColor) + "' maxlength='7' pattern='#[0-9A-Fa-f]{6}' oninput='syncHex(\"bgHex\",\"bgPicker\")'></div></div>";
  html += "<div class='buttons'><button type='button' class='secondary' onclick='resetColors()'>Reset colors</button></div>";
  html += "</section>";

  html += "<section id='font' class='card'><h2>Font</h2>";
  html += "<label>Display font</label><select name='fontstyle'>";
  html += "<option value='0'" + String(cfg.fontStyle == 0 ? " selected" : "") + ">Classic</option>";
  html += "<option value='1'" + String(cfg.fontStyle == 1 ? " selected" : "") + ">Helvetica</option>";
  html += "<option value='2'" + String(cfg.fontStyle == 2 ? " selected" : "") + ">Courier</option>";
  html += "<option value='3'" + String(cfg.fontStyle == 3 ? " selected" : "") + ">New Century</option>";
  html += "<option value='4'" + String(cfg.fontStyle == 4 ? " selected" : "") + ">Comic / Handwritten</option>";
  html += "</select><small>Applies to subscriber count, channel name and clock.</small></section>";

  // Buttons
  html += "<section id='buttons' class='card'><h2>Buttons</h2>";
  html += "<h3>Button 1</h3><div class='status'><strong>Fixed function</strong><br>Tap: Sleep<br>Hold 5 sec: Shutdown / Deep Sleep</div>";
  html += "<h3>Button 2</h3><div class='status'><strong>YouTube Counter</strong><br>Tap: number format 10.000 &harr; 10k</div>";
  html += "<h3>Button mapping</h3><label class='check'><input type='checkbox' name='swap'" + String(cfg.swapButtons ? " checked" : "") + ">Swap buttons (logically swap left / right)</label>";
  html += "</section>";

  html += "<div class='buttons save-row'><button type='submit'>Save settings</button><span id='saveStatus' class='save-status'>Saved</span></div></form>";

  html += "<section id='system' class='card'><h2>System</h2>";
  html += "<div class='status'>Firmware: " + String(FW_VERSION) + "<br>Hostname: " + htmlEscape(cfg.hostname) + ".local<br>IP: " + WiFi.localIP().toString() + "<br>Wi-Fi: " + htmlEscape(WiFi.SSID()) + "<br>Signal: " + String(WiFi.RSSI()) + " dBm</div>";
  html += "<div class='buttons'><form method='POST' action='/system/reboot'><button type='submit' class='secondary'>Reboot</button></form>";
  html += "<form method='POST' action='/system/reset' onsubmit='return confirm(\"Delete all settings and restart setup?\")'><button type='submit' class='danger'>Factory Reset</button></form></div></section>";

  html += R"JS(<script>
async function testYoutube(){
 const box=document.getElementById('ytTest'); box.className='status'; box.textContent='Testing...';
 const form=document.querySelector('form[action="/save"]'); const fd=new FormData(form);
 try{const r=await fetch('/api/youtube/test',{method:'POST',body:fd});const j=await r.json();
   if(j.ok){box.className='status ok';box.innerHTML='<strong>OK</strong><br>Channel: '+escapeHtml(j.channel)+'<br>Subscribers: '+escapeHtml(j.subscribers)+'<br>Views: '+escapeHtml(j.views);}
   else{box.className='status bad';box.textContent='Error: '+(j.error||'unknown');}
 }catch(e){box.className='status bad';box.textContent='Error: '+e;}
}
function channelNameChanged(){const cb=document.getElementById('ytname');const input=document.getElementById('nameint');if(cb&&input)input.disabled=!cb.checked;}
function clockChanged(){const on=document.getElementById('showclock')?.checked;['timezone','ampm','clocksec'].forEach(id=>{const e=document.getElementById(id);if(e)e.disabled=!on;});}
function syncHex(textId,pickerId){const t=document.getElementById(textId),p=document.getElementById(pickerId);if(/^#[0-9a-fA-F]{6}$/.test(t.value)){p.value=t.value;t.value=t.value.toUpperCase();}}
function resetColors(){const fp=document.getElementById('fontPicker'),fh=document.getElementById('fontHex'),bp=document.getElementById('bgPicker'),bh=document.getElementById('bgHex');if(fp)fp.value='#ffffff';if(fh)fh.value='#FFFFFF';if(bp)bp.value='#000000';if(bh)bh.value='#000000';}
async function saveSettings(event){
 event.preventDefault();
 const form=document.getElementById('settingsForm');
 const status=document.getElementById('saveStatus');
 const button=form.querySelector('button[type="submit"]');
 button.disabled=true;
 try{
   const response=await fetch('/save',{method:'POST',body:new FormData(form)});
   if(!response.ok)throw new Error('save failed');
   status.textContent='Saved';
   status.classList.add('show');
   setTimeout(()=>status.classList.remove('show'),1800);
 }catch(e){
   status.textContent='Save failed';
   status.classList.add('show');
   setTimeout(()=>status.classList.remove('show'),2500);
 }finally{button.disabled=false;}
}
function escapeHtml(v){return String(v).replace(/[&<>'"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt',"'":'&#39;','"':'&quot;'}[c]))}
channelNameChanged();
clockChanged();
</script>)JS";
  html += "</main></body></html>";
  return html;
}

void applyWebFormToConfig(AppConfig &target) {
  target.swapButtons = server.hasArg("swap");
  target.youtubeChannelRef = server.arg("ytref");
  if (server.arg("ytkey").length()) target.youtubeApiKey = server.arg("ytkey");
  target.youtubeRefreshSec = constrain(server.arg("ytrefresh").toInt(), 30, 3600);
  target.showYoutubeLogo = server.hasArg("ytlogo");
  target.showDeviceName = server.hasArg("devname");
  target.numberFormat = server.arg("numfmt") == "short" ? NUMBER_SHORT : NUMBER_LONG;
  target.showChannelName = server.hasArg("ytname");
  if (server.hasArg("nameint")) {
    target.channelNameEverySec = constrain(server.arg("nameint").toInt(), 5, 600);
  }
  target.showClock = server.hasArg("showclock");
  if (server.hasArg("timezone") && server.arg("timezone").length()) target.timezone = server.arg("timezone");
  target.clockAmPm = server.hasArg("ampm");
  if (server.hasArg("clocksec")) target.clockDisplaySec = constrain(server.arg("clocksec").toInt(), 2, 60);
  target.fontColor = parseHexColor(server.arg("fontcolor"), target.fontColor);
  target.backgroundColor = parseHexColor(server.arg("bgcolor"), target.backgroundColor);
  if (server.hasArg("fontstyle")) target.fontStyle = constrain(server.arg("fontstyle").toInt(), 0, 4);
  target.brightness = constrain(server.arg("brightness").toInt(), 1, 100);
}

void startNormalWebUi() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html; charset=utf-8", normalPage());
  });

  server.on("/save", HTTP_POST, []() {
    applyWebFormToConfig(cfg);
    saveConfig();
    setBacklight(cfg.brightness);
    applyTimezone();
    youtubeSlide = 0;
    youtubeSlideStartedMs = millis();
    lastChannelNameSlideMs = millis();
    drawCurrentScreen(true);
    server.send(200, "text/plain", "saved");
  });

  server.on("/api/youtube/test", HTTP_POST, []() {
    AppConfig testCfg = cfg;
    applyWebFormToConfig(testCfg);
    YouTubeData testData;
    bool ok = fetchYouTubeData(testCfg, testData);

    String json = "{\"ok\":" + String(ok ? "true" : "false");
    if (ok) {
      json += ",\"channel\":\"" + jsonEscape(testData.channelName) + "\"";
      json += ",\"subscribers\":\"" + jsonEscape(testData.subscribersHidden ? "hidden" : formatCount(testData.subscribers, testCfg.numberFormat)) + "\"";
      json += ",\"views\":\"" + jsonEscape(formatLong(testData.views)) + "\"";
    } else {
      json += ",\"error\":\"" + jsonEscape(testData.lastError) + "\"";
    }
    json += "}";
    server.send(ok ? 200 : 400, "application/json", json);
  });

  server.on("/system/reboot", HTTP_POST, []() {
    server.send(200, "text/html; charset=utf-8", "<html><body>Reboot...</body></html>");
    delay(300);
    ESP.restart();
  });

  server.on("/system/reset", HTTP_POST, []() {
    server.send(200, "text/html; charset=utf-8", "<html><body>Reset...</body></html>");
    delay(300);
    factoryReset();
  });

  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
}

// -----------------------------------------------------------------------------
// Button handling / sleep
// -----------------------------------------------------------------------------
uint8_t logicalSleepPin() {
  return cfg.swapButtons ? PIN_BTN_B : PIN_BTN_A;
}

uint8_t logicalModePin() {
  return cfg.swapButtons ? PIN_BTN_A : PIN_BTN_B;
}

void prepareWakePinsLightSleep() {
  gpio_wakeup_enable((gpio_num_t)PIN_BTN_A, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_BTN_B, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
}

void enterLightSleep() {
  drawMessage("SLEEP", "press any button to wake");
  delay(120);
  setBacklight(0);
  prepareWakePinsLightSleep();
  delay(50);
  esp_light_sleep_start();
  // resumes here
  gpio_wakeup_disable((gpio_num_t)PIN_BTN_A);
  gpio_wakeup_disable((gpio_num_t)PIN_BTN_B);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  // Consume the wake press so it does not immediately trigger another action.
  while (digitalRead(PIN_BTN_A) == LOW || digitalRead(PIN_BTN_B) == LOW) delay(10);
  physicalA.stable = physicalA.lastRaw = HIGH;
  physicalB.stable = physicalB.lastRaw = HIGH;
  setBacklight(cfg.brightness);
  delay(100);
  drawCurrentScreen(true);
}

void enterDeepSleep() {
  drawMessage("SHUTDOWN", "press a button to start");
  delay(300);
  setBacklight(0);

  uint64_t mask = (1ULL << PIN_BTN_A) | (1ULL << PIN_BTN_B);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
  esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
    mask,
    ESP_GPIO_WAKEUP_GPIO_LOW
  );
#else
  esp_deep_sleep_enable_gpio_wakeup(
    mask,
    ESP_GPIO_WAKEUP_GPIO_LOW
  );
#endif

  delay(50);
  esp_deep_sleep_start();
}

void onLogicalSleepReleased(uint32_t heldMs, bool longReached) {
  if (longReached || heldMs >= LONG_HOLD_MS) enterDeepSleep();
  else if (heldMs >= DEBOUNCE_MS) enterLightSleep();
}

void onLogicalModeTap() {
  cfg.numberFormat = (cfg.numberFormat == NUMBER_LONG) ? NUMBER_SHORT : NUMBER_LONG;
  youtubeSlide = 0;
  youtubeSlideStartedMs = millis();
  drawCurrentScreen(true);
}

void processPhysicalButton(uint8_t pin, ButtonState &b) {
  bool raw = digitalRead(pin);
  uint32_t now = millis();

  if (raw != b.lastRaw) {
    b.lastRaw = raw;
    b.rawChangedMs = now;
  }
  if (now - b.rawChangedMs < DEBOUNCE_MS || raw == b.stable) return;

  b.stable = raw;
  bool isSleepLogical = (pin == logicalSleepPin());
  bool isModeLogical = (pin == logicalModePin());

  if (raw == LOW) {
    b.pressedMs = now;
    b.longReached = false;
  } else {
    uint32_t held = now - b.pressedMs;
    if (isSleepLogical) {
      bool wasLong = b.longReached;
      b.longReached = false;
      onLogicalSleepReleased(held, wasLong);
    } else if (isModeLogical && held >= DEBOUNCE_MS && held < LONG_HOLD_MS) {
      onLogicalModeTap();
    }
  }
}

void updateLongHoldFeedback(uint8_t pin, ButtonState &b) {
  if (b.stable == LOW && pin == logicalSleepPin() && !b.longReached) {
    uint32_t held = millis() - b.pressedMs;
    if (held >= LONG_HOLD_MS) {
      b.longReached = true;
      drawMessage("SHUTDOWN", "release button");
    }
  }
}

void handleButtons() {
  processPhysicalButton(PIN_BTN_A, physicalA);
  processPhysicalButton(PIN_BTN_B, physicalB);
  updateLongHoldFeedback(PIN_BTN_A, physicalA);
  updateLongHoldFeedback(PIN_BTN_B, physicalB);
}

// -----------------------------------------------------------------------------
// Arduino setup / loop
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(150);

  pinMode(PIN_BTN_A, INPUT_PULLUP);
  pinMode(PIN_BTN_B, INPUT_PULLUP);

  initBacklight();
  setBacklight(70);

  if (!gfx->begin(TFT_SPI_HZ)) {
    Serial.println("Display init failed");
  }
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextWrap(false);

  loadConfig();
  setBacklight(cfg.brightness);

  // Deep-sleep wake is a reboot
  while (digitalRead(PIN_BTN_A) == LOW || digitalRead(PIN_BTN_B) == LOW) delay(10);
  physicalA.stable = physicalA.lastRaw = HIGH;
  physicalB.stable = physicalB.lastRaw = HIGH;

  if (!cfg.provisioned) startSetupPortal();

  if (!connectConfiguredWifi()) {
    // if saved WiFi cannot be reached, reopen setup portal
    startSetupPortal();
  }

  startNormalWebUi();
  drawMessage("Connected", cfg.hostname + ".local");
  delay(700);

  fetchYouTubeData(cfg, yt);
  lastYoutubeFetchMs = millis();
  youtubeSlideStartedMs = millis();
  lastChannelNameSlideMs = millis();
  drawCurrentScreen(true);
}

void loop() {
  server.handleClient();
  handleButtons();

  uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED &&
      now - lastYoutubeFetchMs >= cfg.youtubeRefreshSec * 1000UL) {
    lastYoutubeFetchMs = now;

    bool oldValid = yt.valid;
    bool oldHidden = yt.subscribersHidden;
    uint64_t oldSubscribers = yt.subscribers;
    String oldChannelName = yt.channelName;
    String oldError = yt.lastError;

    fetchYouTubeData(cfg, yt);

    // Only redraw after an API update if something visible actually changed
    if (oldValid != yt.valid ||
        oldHidden != yt.subscribersHidden ||
        oldSubscribers != yt.subscribers ||
        oldChannelName != yt.channelName ||
        oldError != yt.lastError) {
      drawCurrentScreen(true);
    }
  }

  if (now - lastScreenDrawMs >= 250) drawCurrentScreen(false);

  delay(5);
}