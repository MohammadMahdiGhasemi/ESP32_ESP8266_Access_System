#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <SPI.h>
#include <MFRC522.h>
#include <DHT.h>
#include "Core.h"

// Keep these three settings identical on both boards.
const char* WIFI_SSID = "ESP_Access_System";
const char* WIFI_PASSWORD = "AccessDemo4826";
const char* DEVICE_TOKEN = "access-node-v1-4826";
const uint16_t TCP_PORT = 4242;
const uint8_t RFID_SS = D2;       // GPIO4; RC522 SDA means SPI chip select.
const uint8_t DHT_PIN = D1;       // GPIO5
const uint8_t PIR_PIN = D0;       // GPIO16
const uint8_t GREEN_PIN = D4;     // GPIO2, ACTIVE LOW. LED anode to 3V3 via 1k.
const uint8_t RED_PIN = D8;       // GPIO15, ACTIVE HIGH. LED cathode to GND.
MFRC522 reader(RFID_SS, MFRC522::UNUSED_PIN); // Tie RC522 RST to 3V3.
DHT dht(DHT_PIN, DHT11);
WiFiClient master;
LineBuffer<128> input;
LineBuffer<32> serialInput;
CardDb database;
Window enrollment;
RisingEdge motion;
bool fsReady = false, dbReady = false, rfidReady = false, authenticated = false;
bool ledActive = false;
uint32_t lastConnect = 0, lastRx = 0, lastTx = 0, lastDht = 0, lastRfid = 0;
uint32_t ledSince = 0, bootAt = 0;
float temperature = NAN, humidity = NAN;
const char* DB_PATH = "/cards.bin";

// Large storage buffers are static: ESP8266 loop stack is only about 4 KB.
// These helpers run sequentially on the main loop; do not call from an ISR.
bool loadDb(const char* path, CardDb& result) {
  Serial.printf("[FS] Read %s\n", path);
  File f = LittleFS.open(path, "r");
  if(!f || f.size()!=CardDb::BYTES) return false;
  static uint8_t bytes[CardDb::BYTES];
  size_t got=f.read(bytes,sizeof bytes); f.close();
  return result.decode(bytes,got);
}
bool saveDb(const CardDb& candidate) {
  static uint8_t bytes[CardDb::BYTES]; candidate.encode(bytes);
  Serial.println("[FS] Write temporary database");
  File f=LittleFS.open("/cards.tmp","w");
  if(!f) return false;
  size_t written=f.write(bytes,sizeof bytes); f.flush(); f.close();
  if(written!=sizeof bytes) return false;
  static CardDb verified;
  if(!loadDb("/cards.tmp",verified)) return false;
  // LittleFS rename replaces the destination atomically; never remove old DB first.
  if(!LittleFS.rename("/cards.tmp",DB_PATH)) return false;
  return loadDb(DB_PATH,verified);
}
void initStorage() {
  LittleFSConfig cfg; cfg.setAutoFormat(false); LittleFS.setConfig(cfg);
  Serial.println("[FS] Mount begin");
  fsReady=LittleFS.begin(); dbReady=false;
  Serial.printf("[FS] Mount result: %s\n",fsReady ? "OK" : "FAIL");
  if(fsReady) {
    if(LittleFS.exists(DB_PATH)) dbReady=loadDb(DB_PATH,database);
    else {
      static CardDb empty;
      dbReady=saveDb(empty);
      if(dbReady) database=empty;
    }
  }
  Serial.printf("[FS] Database: %s\n",dbReady ? "READY" : "NOT READY");
  if(!dbReady) Serial.println("Storage unavailable/corrupt. First setup ONLY: type FORMAT and Enter. This erases all cards.");
}
void leds(bool green, bool red) {
  digitalWrite(GREEN_PIN,green ? LOW : HIGH);
  digitalWrite(RED_PIN,red ? HIGH : LOW);
}
void flashLed(bool allowed) {
  leds(allowed,!allowed); ledActive=true; ledSince=millis();
}
void sendLine(const String& s) {
  if(master.connected()) master.println(s);
}
void event(const char* name) {
  Serial.println(name);
  if(authenticated) sendLine(String("EVENT|")+name);
}
void disconnectMaster() {
  master.stop(); authenticated=false; input.reset(); enrollment.cancel();
}
void receiveLine(const char* text) {
  if(!strcmp(text,"WELCOME")) { authenticated=true; lastRx=millis(); return; }
  if(!authenticated) return;
  lastRx=millis();
  if(!strcmp(text,"PING")) sendLine("PONG");
  else if(!strcmp(text,"ENROLL")) {
    if(!dbReady) event("STORAGE_ERROR");
    else if(!rfidReady) event("RFID_ERROR");
    else if(!enrollment.active) { enrollment.start(millis()); event("ARMED"); }
  } else if(!strcmp(text,"CANCEL")) { enrollment.cancel(); event("CANCELLED"); }
}
void networkTick() {
  const uint32_t now=millis();
  if(WiFi.status()!=WL_CONNECTED) { if(master.connected() || authenticated) disconnectMaster(); return; }
  if(!master.connected()) {
    if(authenticated) disconnectMaster();
    if(elapsed(now,lastConnect,3000)) {
      lastConnect=now; input.reset();
      // Bounded connection attempt; sensor/UI state machines do not use long delays.
      master.setTimeout(200);
      if(master.connect(IPAddress(192,168,4,1),TCP_PORT)) {
        master.setNoDelay(true); lastRx=now; lastTx=now;
        sendLine(String("HELLO|1|")+DEVICE_TOKEN);
      }
    }
    return;
  }
  int budget=256;
  while(master.available() && budget-- > 0) {
    if(input.push(char(master.read()))) receiveLine(input.text());
  }
  if(elapsed(now,lastRx,6000)) { disconnectMaster(); return; }
  if(authenticated && elapsed(now,lastTx,1000)) {
    lastTx=now;
    String status=String("STATE|")+(isnan(temperature) || isnan(humidity) ? "0" : "1")+"|";
    status+=isnan(temperature) ? "0" : String(temperature,1);
    status+="|"; status+=isnan(humidity) ? "0" : String(humidity,1);
    status+="|"; status+=dbReady ? "1" : "0";
    status+="|"; status+=rfidReady ? "1" : "0";
    sendLine(status);
  }
}
void scanCard() {
  if(!rfidReady || !reader.PICC_IsNewCardPresent() || !reader.PICC_ReadCardSerial()) return;
  char uid[21]={};
  for(uint8_t i=0;i<reader.uid.size && i<10;i++) snprintf(uid+2*i,3,"%02X",reader.uid.uidByte[i]);
  reader.PICC_HaltA(); reader.PCD_StopCrypto1();
  Serial.printf("UID: %s\n",uid);
  if(!CardDb::valid(uid)) { flashLed(false); event("CARD_ERROR"); return; }
  if(!dbReady) { flashLed(false); event("STORAGE_ERROR"); return; }
  if(enrollment.active) {
    enrollment.cancel();
    if(database.contains(uid)) { flashLed(true); event("EXISTS"); return; }
    if(database.count()>=50) { flashLed(false); event("FULL"); return; }
    static CardDb candidate;
    candidate=database;
    if(candidate.add(uid) && saveDb(candidate)) {
      database=candidate; flashLed(true); event("SAVED");
    } else { flashLed(false); event("SAVE_ERROR"); }
    return;
  }
  if(database.contains(uid)) {
    flashLed(true); event("GRANTED");
    // Future relay integration belongs here, with a rated driver and timed release.
  } else { flashLed(false); event("DENIED"); }
}
void setup() {
  Serial.begin(115200);
  Serial.println("\nNode firmware: stack-fix-1");
  digitalWrite(GREEN_PIN,HIGH); pinMode(GREEN_PIN,OUTPUT);
  digitalWrite(RED_PIN,LOW); pinMode(RED_PIN,OUTPUT);
  pinMode(PIR_PIN,INPUT); bootAt=millis();
  dht.begin(); SPI.begin(); reader.PCD_Init();
  uint8_t version=reader.PCD_ReadRegister(MFRC522::VersionReg);
  rfidReady=(version!=0 && version!=0xFF);
  Serial.printf("RC522 version: 0x%02X\n",version);
  initStorage();
  WiFi.persistent(false); WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  Serial.println("Node started. PIR warm-up: 30 seconds.");
}
void loop() {
  networkTick();
  uint32_t now=millis();
  if(enrollment.expired(now)) { enrollment.cancel(); event("TIMEOUT"); }
  bool rising=motion.sample(digitalRead(PIR_PIN)==HIGH);
  if(elapsed(now,bootAt,30000) && rising) event("MOTION");
  if(ledActive && elapsed(now,ledSince,1500)) { leds(false,false); ledActive=false; }
  if(elapsed(now,lastDht,2500)) {
    lastDht=now; humidity=dht.readHumidity(); temperature=dht.readTemperature();
  }
  if(elapsed(now,lastRfid,80)) { lastRfid=now; scanCard(); }
  while(Serial.available()) {
    if(serialInput.push(char(Serial.read())) && !strcmp(serialInput.text(),"FORMAT")) {
      enrollment.cancel(); dbReady=false; database=CardDb();
      LittleFS.end(); fsReady=LittleFS.format();
      if(fsReady) initStorage();
      Serial.println(dbReady ? "Storage initialized; all cards erased." : "Storage initialization failed.");
    }
  }
  yield();
}
