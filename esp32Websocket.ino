/******************** LIBRARIES ********************/
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

/******************** CONFIG ********************/
#define UART_TEST 1
#define BUFF_LEN  256
#define NUM_RELAY 4

#define UART_LOOPBACK_TEST  1

const char* ssid = "REPLACE_WITH_YOUR_SSID";
const char* password = "REPLACE_WITH_YOUR_PASSWORD";

/******************** UART ********************/
HardwareSerial UART(1);

/******************** DATA ********************/
float temp = 0;
float volt = 0;
int   rssi = 0;

/******************** RELAY ********************/
const int relayPins[NUM_RELAY] = {4, 5, 18, 19};
bool relayState[NUM_RELAY] = {0};

/******************** SERVER ********************/
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

/******************** RTOS ********************/
QueueHandle_t uartQueue;

/******************** STRUCT ********************/
typedef struct {
  float temp;
  float volt;
  int   rssi;
} uart_msg_t;

/******************** HTML ********************/
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
html { font-family: Arial; text-align:center; }
button { padding:15px; font-size:20px; margin:5px; }
</style>
</head>
<body>
<h2>ESP32 WebSocket RTOS</h2>
<p>Temp: <span id="temp">0</span></p>
<p>Volt: <span id="volt">0</span></p>
<p>RSSI: <span id="rssi">0</span></p>

<button id="relay0" onclick="toggleRelay(0)">Relay 1: OFF</button><br>
<button id="relay1" onclick="toggleRelay(1)">Relay 2: OFF</button><br>
<button id="relay2" onclick="toggleRelay(2)">Relay 3: OFF</button><br>
<button id="relay3" onclick="toggleRelay(3)">Relay 4: OFF</button>

<script>
var ws = new WebSocket(`ws://${location.hostname}/ws`);
ws.onmessage = e => {
  let d = JSON.parse(e.data);
  if(d.temp)  temp.innerHTML  = d.temp;
  if(d.volt)  volt.innerHTML  = d.volt;
  if(d.rssi)  rssi.innerHTML  = d.rssi;
  if(d.relay){
    d.relay.forEach((s,i)=>{
      document.getElementById("relay"+i).innerText =
        "Relay "+(i+1)+": "+(s?"ON":"OFF");
    });
  }
};
function toggleRelay(i){
  ws.send(JSON.stringify({relay:i,state:
    !document.getElementById("relay"+i).innerText.includes("ON")}));
}
</script>
</body>
</html>
)rawliteral";

/******************** FUNCTIONS ********************/
void sendUARTData() {
  StaticJsonDocument<BUFF_LEN> doc;
  doc["temp"]  = temp;
  doc["volt"]  = volt;
  doc["rssi"]  = rssi;

  String json;
  serializeJson(doc, json);
  ws.textAll(json);
}

void sendRelayStatus() {
  StaticJsonDocument<BUFF_LEN> doc;
  JsonArray a = doc.createNestedArray("relay");
  for (int i=0;i<NUM_RELAY;i++) a.add(relayState[i]);

  String json;
  serializeJson(doc, json);
  ws.textAll(json);
}

/******************** WEBSOCKET ********************/
void handleWS(void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  if (!info->final || info->opcode != WS_TEXT) return;

  StaticJsonDocument<BUFF_LEN> doc;
  if (deserializeJson(doc, data)) return;

  if (doc.containsKey("relay")) {
    int id = doc["relay"];
    bool st = doc["state"];
    if (id >= 0 && id < NUM_RELAY) {
      relayState[id] = st;
      digitalWrite(relayPins[id], st);
      Serial.printf("Relay %d state %s\n", id+1, st ? "HIGH":"LOW");
      sendRelayStatus();
    }
  }
}

void wsEvent(AsyncWebSocket *s, AsyncWebSocketClient *c,
             AwsEventType t, void *arg, uint8_t *data, size_t len) {
  switch (t) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", c->id(), c->remoteIP().toString().c_str());
      sendUARTData();
      sendRelayStatus();
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", c->id());
      break;
    case WS_EVT_DATA:
      handleWS(arg, data, len);
      break;
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}

/******************** RTOS TASKS ********************/
void uartLoopbackTestTask(void *pv) {
  for (;;) {
#if UART_LOOPBACK_TEST
    float t = random(200, 350) / 10.0;
    float v = random(115, 130) / 10.0;
    int   r = random(-85, -40);

    UART.printf("TEMP=%.1f\n", t);
    UART.printf("VOLT=%.1f\n", v);
    UART.printf("RSSI=%d\n", r);

    // Serial.println("[UART LOOPBACK] Sent test data");
#endif
    vTaskDelay(10000 / portTICK_PERIOD_MS);
  }
}

void uartTask(void *pv) {
  String line;
  uart_msg_t msg;

  for (;;) {
    while (UART.available()) {
      char c = UART.read();
      if (c == '\n') {
        line.trim();
        if (line.startsWith("TEMP=")) msg.temp = line.substring(5).toFloat();
        if (line.startsWith("VOLT=")) msg.volt = line.substring(5).toFloat();
        if (line.startsWith("RSSI=")) msg.rssi = line.substring(5).toInt();
        xQueueOverwrite(uartQueue, &msg);
        line="";
      } else line+=c;
    }
    vTaskDelay(1);
  }
}

void wifiTask(void *pv) {
  uart_msg_t msg;
  for (;;) {
    ws.cleanupClients();
    if (xQueueReceive(uartQueue, &msg, 0)) {
      temp = msg.temp;
      volt = msg.volt;
      rssi = msg.rssi;
      sendUARTData();
    }
    vTaskDelay(10);
  }
}


/******************** SETUP ********************/
void setup() {
  Serial.begin(115200);
  UART.begin(115200, SERIAL_8N1, 16, 17);

  for (int i=0;i<NUM_RELAY;i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
  }

  WiFi.begin(ssid, password);
  while (WiFi.status()!=WL_CONNECTED) {
    delay(500);
    Serial.println("Connecting to WiFi..");
  }
  Serial.println(WiFi.localIP());

  ws.onEvent(wsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET,
    [](AsyncWebServerRequest *r){ r->send(200,"text/html",index_html); });
  server.begin();

  uartQueue = xQueueCreate(1, sizeof(uart_msg_t));

  xTaskCreatePinnedToCore(uartTask, "uart", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(wifiTask, "wifi", 4096, NULL, 3, NULL, 0);
#if UART_LOOPBACK_TEST
  xTaskCreatePinnedToCore(uartLoopbackTestTask, "UART_LOOP_TEST", 2048, NULL, 1, NULL, 1);
#endif
}

/******************** LOOP ********************/
void loop() {
  vTaskDelay(portMAX_DELAY);
}
