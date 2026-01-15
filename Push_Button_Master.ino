#include <ESP8266WiFi.h>
#include <espnow.h>

/* ================= DEBUG CONTROL ================= */
#define DEBUG_ENABLE 1
#if DEBUG_ENABLE
#define DBG_BEGIN() Serial.begin(74880)
#define DBG_PRINT(x) Serial.print(x)
#define DBG_PRINTLN(x) Serial.println(x)
#else
#define DBG_BEGIN()
#define DBG_PRINT(x)
#define DBG_PRINTLN(x)
#endif
/* ================================================= */

#define BUTTON_PIN D2
#define LED_PIN D4 // active LOW

// A8:48:FA:C0:A1:FB <- Slave Mac
// 44:17:93:15:F0:CA <- Master Mac

uint8_t relayMAC[] = {0xA8, 0x48, 0xFA, 0xC0, 0xA1, 0xFB};

/* ================= PROTOCOL ================= */
struct Message
{
  bool relayState;
  bool sync;
  uint8_t seq;
};

Message msg;

volatile bool rxFlag = false;
volatile bool txFlag = false;
volatile bool slaveOnline = false;
volatile bool lastSlaveOnline = false;
volatile bool syncRxFlag = false;

/* ================= HEALTH ================= */
unsigned long lastRxTime = 0;
const unsigned long HEALTH_INTERVAL = 5000;
const unsigned long OFFLINE_TIMEOUT = 15000; // 3× interval
unsigned long lastSlaveRx = 0;

/* ================= LED ================= */
void blinkReceive()
{
  for (int i = 0; i < 2; i++)
  {
    digitalWrite(LED_PIN, LOW);
    delay(100);
    digitalWrite(LED_PIN, HIGH);
    delay(100);
  }
}
void blinkSend()
{
  digitalWrite(LED_PIN, LOW);
  delay(400);
  digitalWrite(LED_PIN, HIGH);
}

/* ================= CALLBACKS ================= */
void onReceive(uint8_t *, uint8_t *data, uint8_t len)
{
  if (len == sizeof(Message))
  {
    Message incoming;
    memcpy(&incoming, data, sizeof(Message));

    // ignore stale packets
    if (!incoming.sync && (int8_t)(incoming.seq - msg.seq) < 0)
    {
      return;
    }

    if (incoming.sync)
    {
      syncRxFlag = true;
    }

    msg.relayState = incoming.relayState;
    msg.seq = incoming.seq;

    lastRxTime = millis();
    rxFlag = true;
    lastSlaveRx = millis();
    slaveOnline = true;
  }
}

void onSend(uint8_t *, uint8_t)
{
  txFlag = true;
}

/* ================= SEND ================= */
void sendPacket(bool sync = false)
{
  msg.sync = sync;

  if (!msg.sync)
  {
    msg.seq++;
  }
  esp_now_send(relayMAC, (uint8_t *)&msg, sizeof(Message));
}

/* ================= SETUP ================= */
void setup()
{
  DBG_BEGIN();
  DBG_PRINTLN("\n[BOOT] MASTER");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  WiFi.mode(WIFI_STA);
  DBG_PRINT("[WIFI] MAC: ");
  DBG_PRINTLN(WiFi.macAddress());

  esp_now_init();
  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_add_peer(relayMAC, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
  esp_now_register_recv_cb(onReceive);
  esp_now_register_send_cb(onSend);

  // Initial sync
  msg.seq = 0;
  msg.relayState = false;
  sendPacket(true);
}

/* ================= LOOP ================= */
void loop()
{

  // ----- SLAVE ONLINE CHECK -----
  if (slaveOnline && (millis() - lastSlaveRx > OFFLINE_TIMEOUT))
  {
    slaveOnline = false;
  }

  // ----- PRINT STATUS ONLY ON CHANGE -----
  if (slaveOnline != lastSlaveOnline)
  {
    lastSlaveOnline = slaveOnline;
    DBG_PRINTLN(slaveOnline ? "[SLAVE] ONLINE" : "[SLAVE] OFFLINE");
  }

  if (rxFlag)
  {
    rxFlag = false;
    blinkReceive();

    DBG_PRINT("[RX] seq=");
    DBG_PRINT(msg.seq);
    DBG_PRINT(" relay=");
    DBG_PRINTLN(msg.relayState ? "ON" : "OFF");
  }

  if (txFlag)
  {
    txFlag = false;
    blinkSend();
  }

  // Button (unchanged, as requested)
  if (digitalRead(BUTTON_PIN) == LOW)
  {
    delay(30);

    // Step 1: request sync
    syncRxFlag = false;
    DBG_PRINTLN("[BTN] Sync before toggle");
    sendPacket(true);
    unsigned long waitStart = millis();
    while (!syncRxFlag && millis() - waitStart < 1000)
    {
      yield();
    }

    if (!syncRxFlag)
    {
      DBG_PRINTLN("[BTN] Sync failed");
    }
    else
    {
      syncRxFlag = false;

      // Step 2: toggle
      msg.relayState = !msg.relayState;
      DBG_PRINTLN("[BTN] Toggle send");
      sendPacket(false);
      while (digitalRead(BUTTON_PIN) == LOW)
      {
        yield();
      }
    }
  }

  // Health check (MASTER ONLY)
  if (millis() - lastRxTime > HEALTH_INTERVAL)
  {
    DBG_PRINTLN("[HEALTH] Resync");
    sendPacket(true);
    lastRxTime = millis();
  }
}
