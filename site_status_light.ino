/*
 * site_status_light.ino
 * ---------------------
 * ESP32-WROOM website status indicator.
 *
 * Polls https://kylejambretz.dev and drives a common-cathode RGB LED.
 *
 *
 * STATE            COLOUR              MEANING
 * ---------------  ------------------  ------------------------------------
 * BOOTING          purple, solid       just powered on, nothing known yet
 * NO_WIFI          blue, breathing     not associated with the AP — our fault
 * NO_INTERNET      amber, breathing    WiFi up, internet unreachable — our fault
 * SITE_UNREACHABLE red, breathing      internet fine, site won't answer
 * SITE_ERROR       red, solid          site answered, but with 4xx/5xx
 * SITE_OK          green, dim solid    HTTP 2xx
 *
 * Wiring (common cathode: the long leg goes to GND):
 *   R -> GPIO 25   through a resistor (~220R)
 *   G -> GPIO 26   through a resistor (~220R)
 *   B -> GPIO 27   through a resistor (~220R)
 *   common -> GND
 *
 * If your LED is common ANODE (common leg goes to 3V3), set COMMON_ANODE true.
 *
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static const char* WIFI_SSID     = "WIFI NAME HERE";
static const char* WIFI_PASSWORD = "WIFI PASSWORD HERE";

static const char* SITE_URL      = "https://kylejambretz.dev/";
static const char* SITE_HOST     = "kylejambretz.dev";

static const char* CONTROL_IP    = "1.1.1.1";
static const uint16_t CONTROL_PORT = 443;

// Pins
static const uint8_t PIN_R = 25;
static const uint8_t PIN_G = 26;
static const uint8_t PIN_B = 27;

static const bool COMMON_ANODE = false;   // true if common leg -> 3V3

// Timing
static const uint32_t CHECK_INTERVAL_MS   = 30000;  // when things are healthy
static const uint32_t RECHECK_INTERVAL_MS = 10000;  // when something is wrong
static const uint32_t HTTP_TIMEOUT_MS     = 8000;
static const uint32_t CONTROL_TIMEOUT_MS  = 3000;
static const uint32_t WIFI_GRACE_MS       = 120000; // reboot if offline this long

static const uint8_t FAIL_THRESHOLD = 3;

static const bool VERIFY_TLS = false;
static const char* ROOT_CA_PEM = R"CERT(
-----BEGIN CERTIFICATE-----
...paste ISRG Root X1 (or your CA) here...
-----END CERTIFICATE-----
)CERT";

// ---------------------------------------------------------------------------
// Types
//
// This MUST live above the first function definition. The Arduino .ino
// preprocessor injects auto-generated prototypes just before the first
// function it finds; any type named in a signature has to already exist at
// that point, or the prototype fails to parse. (In a .cpp file this whole
// problem disappears — no prototype injection happens there.)
// ---------------------------------------------------------------------------

enum Status : uint8_t {
  BOOTING,
  NO_WIFI,
  NO_INTERNET,
  SITE_UNREACHABLE,
  SITE_ERROR,
  SITE_OK
};

// ---------------------------------------------------------------------------
// LED driver
// ---------------------------------------------------------------------------

static const uint32_t LEDC_FREQ_HZ = 5000;
static const uint8_t  LEDC_RES_BITS = 12;
static const uint16_t LEDC_MAX = (1 << LEDC_RES_BITS) - 1;

static const uint8_t CH_R = 0, CH_G = 1, CH_B = 2;

static uint16_t gammaLUT[256];

static void buildGammaLUT() {
  for (int i = 0; i < 256; i++) {
    float norm = i / 255.0f;
    gammaLUT[i] = (uint16_t)(powf(norm, 2.6f) * LEDC_MAX + 0.5f);
  }
}

static void ledcSetupPin(uint8_t pin, uint8_t channel) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)channel;
  ledcAttach(pin, LEDC_FREQ_HZ, LEDC_RES_BITS);
#else
  ledcSetup(channel, LEDC_FREQ_HZ, LEDC_RES_BITS);
  ledcAttachPin(pin, channel);
#endif
}

static void ledcWritePin(uint8_t pin, uint8_t channel, uint16_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)channel;
  ledcWrite(pin, duty);
#else
  (void)pin;
  ledcWrite(channel, duty);
#endif
}

static void setColor(uint8_t r, uint8_t g, uint8_t b, float scale = 1.0f) {
  if (scale < 0.0f) scale = 0.0f;
  if (scale > 1.0f) scale = 1.0f;

  uint16_t dr = (uint16_t)(gammaLUT[r] * scale);
  uint16_t dg = (uint16_t)(gammaLUT[g] * scale);
  uint16_t db = (uint16_t)(gammaLUT[b] * scale);

  if (COMMON_ANODE) {
    dr = LEDC_MAX - dr;
    dg = LEDC_MAX - dg;
    db = LEDC_MAX - db;
  }

  ledcWritePin(PIN_R, CH_R, dr);
  ledcWritePin(PIN_G, CH_G, dg);
  ledcWritePin(PIN_B, CH_B, db);
}

static Status currentStatus = BOOTING;
static uint8_t consecutiveFailures = 0;
static uint32_t lastCheckMs = 0;
static uint32_t lastWifiOkMs = 0;
static int lastHttpCode = 0;

static const char* statusName(Status s) {
  switch (s) {
    case BOOTING:          return "BOOTING";
    case NO_WIFI:          return "NO_WIFI";
    case NO_INTERNET:      return "NO_INTERNET";
    case SITE_UNREACHABLE: return "SITE_UNREACHABLE";
    case SITE_ERROR:       return "SITE_ERROR";
    case SITE_OK:          return "SITE_OK";
  }
  return "?";
}

static void setStatus(Status s) {
  if (s != currentStatus) {
    Serial.printf("[state] %s -> %s\n", statusName(currentStatus), statusName(s));
    currentStatus = s;
  }
}

static void renderLed() {
  float phase = (millis() % 3000) / 3000.0f;
  float breath = 0.15f + 0.85f * (0.5f * (1.0f - cosf(phase * 2.0f * PI)));

  switch (currentStatus) {
    case BOOTING:          setColor(140, 0, 200, 1.0f);   break;  // purple
    case NO_WIFI:          setColor(0, 40, 255, breath);  break;  // blue
    case NO_INTERNET:      setColor(255, 90, 0, breath);  break;  // amber
    case SITE_UNREACHABLE: setColor(255, 0, 0, breath);   break;  // red, pulsing
    case SITE_ERROR:       setColor(255, 0, 0, 1.0f);     break;  // red, solid
    case SITE_OK:          setColor(0, 255, 30, 0.25f);   break;  // green, dim
  }
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

static void wifiConnect() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("[wifi] connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

static bool internetReachable() {
  WiFiClient probe;
  probe.setTimeout(CONTROL_TIMEOUT_MS / 1000);
  bool ok = probe.connect(CONTROL_IP, CONTROL_PORT, CONTROL_TIMEOUT_MS);
  probe.stop();
  return ok;
}

// Returns the HTTP status code, or a negative HTTPClient error.
static int checkSite() {
  WiFiClientSecure client;

  if (VERIFY_TLS) {
    client.setCACert(ROOT_CA_PEM);
  } else {
    client.setInsecure();
  }
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("esp32-status-light/1.0");

  if (!http.begin(client, SITE_URL)) {
    return -1000;
  }

  // HEAD first: same status code, no body, a fraction of the bytes.
  int code = http.sendRequest("HEAD");

  if (code == HTTP_CODE_METHOD_NOT_ALLOWED || code == HTTP_CODE_NOT_IMPLEMENTED) {
    http.end();
    if (!http.begin(client, SITE_URL)) return -1000;
    code = http.GET();
  }

  http.end();
  return code;
}

static void runCheck() {
  if (WiFi.status() != WL_CONNECTED) {
    setStatus(NO_WIFI);
    consecutiveFailures = 0;
    wifiConnect();
    return;
  }

  lastWifiOkMs = millis();

  int code = checkSite();
  lastHttpCode = code;
  Serial.printf("[check] %s -> %d\n", SITE_HOST, code);

  if (code >= 200 && code < 300) {
    consecutiveFailures = 0;
    setStatus(SITE_OK);
    return;
  }

  if (code > 0) {
    consecutiveFailures = 0;
    setStatus(SITE_ERROR);
    return;
  }

  // Transport level failure. Could be us. Ask the control host.
  if (!internetReachable()) {
    Serial.println("[check] control host unreachable — blaming ourselves");
    consecutiveFailures = 0;
    setStatus(NO_INTERNET);
    return;
  }

  // Internet is fine, site won't answer.
  consecutiveFailures++;
  Serial.printf("[check] failure %u/%u\n", consecutiveFailures, FAIL_THRESHOLD);

  if (consecutiveFailures >= FAIL_THRESHOLD) {
    setStatus(SITE_UNREACHABLE);
  }
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] site status light");

  buildGammaLUT();
  ledcSetupPin(PIN_R, CH_R);
  ledcSetupPin(PIN_G, CH_G);
  ledcSetupPin(PIN_B, CH_B);

  setStatus(BOOTING);
  renderLed();

  wifiConnect();
  lastWifiOkMs = millis();
  lastCheckMs = millis() - CHECK_INTERVAL_MS;   // check immediately
}

void loop() {
  renderLed();   // animate every pass

  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiOkMs > WIFI_GRACE_MS) {
    Serial.println("[wifi] offline too long, restarting");
    delay(100);
    ESP.restart();
  }

  bool healthy = (currentStatus == SITE_OK);
  uint32_t interval = healthy ? CHECK_INTERVAL_MS : RECHECK_INTERVAL_MS;

  if (millis() - lastCheckMs >= interval) {
    lastCheckMs = millis();
    runCheck();
  }

  delay(10);
}