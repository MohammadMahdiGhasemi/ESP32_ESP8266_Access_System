#include <WiFi.h>
#include <Wire.h>
#include <Preferences.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#define EXCLUDE_EXOTIC_PROTOCOLS
#include <IRremote.hpp>
#include "Core.h"

const char* WIFI_SSID = "ESP_Access_System";
const char* WIFI_PASSWORD = "AccessDemo4826";
const char* DEVICE_TOKEN = "access-node-v1-4826";
const uint16_t TCP_PORT = 4242;
const uint8_t LCD_SDA=21, LCD_SCL=22, IR_PIN=27, BUZZER_PIN=25;
// Two pins alone do not identify active/passive. Match this to your buzzer.
const bool ACTIVE_BUZZER = false;
WiFiServer server(TCP_PORT);
WiFiClient node;
hd44780_I2Cexp lcd;
Preferences prefs;
LineBuffer<128> input;
bool lcdReady=false, prefsReady=false, authenticated=false, hadClient=false, receivedState=false;
bool learning=false, irConfigured=false, dhtValid=false, storageOk=false, rfidOk=false;
uint16_t irProtocol=0, irAddress=0, irCommand=0;
uint32_t lastRx=0,lastPing=0,lastUi=0,lastData=0,lastKey=0;
uint32_t requestAt=0,learnAt=0;
bool awaitingArm=false;
Window enrollment;
float temperature=0,humidity=0;
char noticeTop[17]={},noticeBottom[17]={};
uint32_t noticeAt=0,noticeDuration=0;
uint8_t noticePriority=0;
char previousRows[2][17] = {};

// Non-blocking buzzer pattern; a card result takes precedence over motion.
bool sounding=false,beepOn=false;
uint8_t beepsLeft=0,soundPriority=0;
uint16_t soundFrequency=2000,soundOnMs=100,soundOffMs=100;
uint32_t soundAt=0;
void buzzerOutput(bool on) {
  if(ACTIVE_BUZZER) digitalWrite(BUZZER_PIN,on ? HIGH : LOW);
  else if(on) tone(BUZZER_PIN,soundFrequency);
  else noTone(BUZZER_PIN);
}
void beep(uint8_t count,uint16_t onMs,uint16_t offMs,uint16_t frequency,uint8_t priority) {
  if(sounding && priority<soundPriority) return;
  buzzerOutput(false);
  beepsLeft=count; soundOnMs=onMs; soundOffMs=offMs; soundFrequency=frequency;
  soundPriority=priority;sounding=true;beepOn=true;soundAt=millis();buzzerOutput(true);
}
void buzzerTick() {
  if(!sounding) return;
  if(!elapsed(millis(),soundAt,beepOn ? soundOnMs : soundOffMs)) return;
  soundAt=millis();
  if(beepOn) {
    buzzerOutput(false);beepOn=false;
    if(--beepsLeft==0) sounding=false;
  } else {beepOn=true;buzzerOutput(true);}
}
void notify(const char* top,const char* bottom,uint32_t duration,uint8_t priority) {
  if(noticeDuration && !elapsed(millis(),noticeAt,noticeDuration) && priority<noticePriority) return;
  snprintf(noticeTop,sizeof noticeTop,"%s",top);snprintf(noticeBottom,sizeof noticeBottom,"%s",bottom);
  noticeAt=millis();noticeDuration=duration;noticePriority=priority;
  Serial.printf("LCD: %s / %s\n",top,bottom);
}
void row(uint8_t number,const char* text) {
  char padded[17]; memset(padded,' ',16);padded[16]=0;
  size_t len=strlen(text);if(len>16)len=16;memcpy(padded,text,len);
  if(lcdReady && strcmp(previousRows[number],padded)) {
    lcd.setCursor(0,number);lcd.print(padded);strcpy(previousRows[number],padded);
  }
}
void uiTick() {
  if(!elapsed(millis(),lastUi,100))return;lastUi=millis();
  if(noticeDuration && elapsed(millis(),noticeAt,noticeDuration))noticeDuration=0;
  if(noticeDuration) {row(0,noticeTop);row(1,noticeBottom);return;}
  if(learning) {row(0,"Teach remote");row(1,"Press button 1");return;}
  if(!authenticated) {row(0,"Node offline");row(1,"Waiting WiFi...");return;}
  if(awaitingArm) {row(0,"Request sent");row(1,"Please wait...");return;}
  if(enrollment.active) {row(0,"Scan new card");row(1,"Within 20 sec");return;}
  if(!receivedState) {row(0,"Node connected");row(1,"Reading sensors");return;}
  if(!storageOk) {row(0,"Storage error");row(1,"Check node USB");return;}
  if(!rfidOk) {row(0,"RC522 error");row(1,"Check wiring");return;}
  if(!dhtValid || elapsed(millis(),lastData,6000)) {row(0,"DHT11 no data");row(1,"Check sensor");return;}
  char text[32];snprintf(text,sizeof text,"Temp: %.1f C",temperature);row(0,text);
  snprintf(text,sizeof text,"Humidity: %.0f%%",humidity);row(1,text);
}
void dropNode() {
  node.stop();authenticated=false;hadClient=false;input.reset();
  awaitingArm=false;enrollment.cancel();dhtValid=false;storageOk=false;rfidOk=false;receivedState=false;
  // Remove an obsolete success/scan message after disconnection.
  noticeDuration=0;
}
void eventReceived(const char* name) {
  Serial.printf("EVENT: %s\n",name);
  if(!strcmp(name,"MOTION")) {
    beep(1,100,100,2300,1);
    notify("Motion detected",enrollment.active ? "Scan new card" : "PIR active",1800,1);
    return;
  }
  if(!strcmp(name,"ARMED")) {
    if(!awaitingArm) {node.println("CANCEL");return;}
    awaitingArm=false;enrollment.start(millis());noticeDuration=0;
    beep(1,70,100,1800,2);return;
  }
  if(!strcmp(name,"GRANTED")) {notify("Access Granted","Welcome",2500,3);beep(1,100,100,2600,3);return;}
  if(!strcmp(name,"DENIED")) {notify("Access Denied","Unknown card",2500,3);beep(3,150,100,800,3);return;}
  awaitingArm=false;enrollment.cancel();
  if(!strcmp(name,"SAVED")) {notify("Card saved","Registration OK",3000,3);beep(2,100,100,2600,3);}
  else if(!strcmp(name,"EXISTS")) {notify("Already saved","Use this card",2500,3);beep(1,150,100,1800,3);}
  else if(!strcmp(name,"FULL")) {notify("Card memory full","Limit: 50 cards",3000,3);beep(3,150,100,800,3);}
  else if(!strcmp(name,"TIMEOUT") || !strcmp(name,"CANCELLED")) {notify("Enroll cancelled","Try button 1",2500,3);}
  else if(!strcmp(name,"RFID_ERROR")) {notify("RC522 error","Check wiring",3000,3);beep(3,150,100,800,3);}
  else if(!strcmp(name,"CARD_ERROR")) {notify("Card read error","Try again",3000,3);beep(3,150,100,800,3);}
  else {notify("Storage error","Card NOT saved",3000,3);beep(3,150,100,800,3);}
}
void receiveLine(const char* line) {
  if(!authenticated) {
    String expected=String("HELLO|1|")+DEVICE_TOKEN;
    if(expected==line) {authenticated=true;lastRx=millis();node.println("WELCOME");}
    else dropNode();
    return;
  }
  if(!strcmp(line,"PONG")) {lastRx=millis();return;}
  if(!strncmp(line,"EVENT|",6)) {lastRx=millis();eventReceived(line+6);return;}
  int valid,fs,rfid;float t,h;
  if(sscanf(line,"STATE|%d|%f|%f|%d|%d",&valid,&t,&h,&fs,&rfid)==5) {
    lastRx=millis();lastData=lastRx;receivedState=true;storageOk=(fs==1);rfidOk=(rfid==1);
    dhtValid=(valid==1 && isfinite(t) && isfinite(h) && t>=-20 && t<=80 && h>=0 && h<=100);
    temperature=t;humidity=h;
  }
}
void networkTick() {
  if(hadClient && !node.connected()) dropNode();
  WiFiClient incoming=server.accept();
  if(incoming) {
    if(hadClient) incoming.stop();
    else {node=incoming;node.setNoDelay(true);hadClient=true;authenticated=false;input.reset();lastRx=millis();}
  }
  if(!hadClient)return;
  int budget=256;
  while(node.available() && budget-- > 0) if(input.push(char(node.read()))) receiveLine(input.text());
  uint32_t now=millis();
  if(elapsed(now,lastRx,authenticated ? 6000 : 3000)) {dropNode();return;}
  if(authenticated && elapsed(now,lastPing,1000)) {lastPing=now;node.println("PING");}
  if(awaitingArm && elapsed(now,requestAt,3000)) {
    // Closing the session also cancels a request whose response was lost.
    dropNode();notify("No response","Please retry",2500,3);
  }
  if(enrollment.active && elapsed(now,enrollment.since,22000)) {
    node.println("CANCEL");enrollment.cancel();notify("Enroll cancelled","Please retry",2500,3);
  }
}
void startLearning() {
  if(awaitingArm || enrollment.active) {
    if(authenticated)node.println("CANCEL");awaitingArm=false;enrollment.cancel();
  }
  learning=true;learnAt=millis();noticeDuration=0;
  Serial.println("Point remote at receiver and press button 1 once. Send L to restart learning.");
}
void requestEnrollment() {
  if(!authenticated) {notify("Node offline","Cannot enroll",2500,3);beep(2,150,100,800,3);return;}
  if(enrollment.active || awaitingArm)return;
  awaitingArm=true;requestAt=millis();noticeDuration=0;node.println("ENROLL");
}
void remoteTick() {
  while(Serial.available()) {char c=Serial.read();if(c=='L' || c=='l') startLearning();}
  if(learning && elapsed(millis(),learnAt,60000)) {
    learning=false;notify("IR not changed","USB: L to teach",3000,3);
  }
  if(!IrReceiver.decode())return;
  const auto data=IrReceiver.decodedIRData;
  IrReceiver.printIRResultShort(&Serial);Serial.println();IrReceiver.resume();
  if(data.protocol==UNKNOWN || (data.flags & IRDATA_FLAGS_IS_REPEAT))return;
  if(!elapsed(millis(),lastKey,400))return;lastKey=millis();
  if(learning) {
    // Single NVS blob: protocol/address/command are updated together.
    uint16_t key[3]={uint16_t(data.protocol),data.address,data.command};
    if(prefsReady && prefs.putBytes("key",key,sizeof key)==sizeof key) {
      irProtocol=key[0];irAddress=key[1];irCommand=key[2];irConfigured=true;learning=false;
      notify("Remote key saved","Press 1 to enroll",2500,3);beep(2,80,100,2600,3);
    } else {notify("IR save error","Check ESP32",3000,3);}
  } else if(irConfigured && uint16_t(data.protocol)==irProtocol && data.address==irAddress && data.command==irCommand) requestEnrollment();
}
void setup() {
  Serial.begin(115200);pinMode(BUZZER_PIN,OUTPUT);digitalWrite(BUZZER_PIN,LOW);
  Wire.begin(LCD_SDA,LCD_SCL);lcdReady=(lcd.begin(16,2)==0);
  if(!lcdReady)Serial.println("LCD init failed; check level shifter, wiring, contrast and I2C backpack.");
  prefsReady=prefs.begin("access-ir",false);
  uint16_t key[3];
  if(prefsReady && prefs.getBytesLength("key")==sizeof key && prefs.getBytes("key",key,sizeof key)==sizeof key) {
    irProtocol=key[0];irAddress=key[1];irCommand=key[2];irConfigured=true;
  }
  IrReceiver.begin(IR_PIN,DISABLE_LED_FEEDBACK);
  WiFi.mode(WIFI_AP);WiFi.softAPConfig(IPAddress(192,168,4,1),IPAddress(192,168,4,1),IPAddress(255,255,255,0));
  if(!WiFi.softAP(WIFI_SSID,WIFI_PASSWORD,1,0,1))Serial.println("AP startup failed.");
  server.begin();Serial.print("AP IP: ");Serial.println(WiFi.softAPIP());
  if(!irConfigured)startLearning();
}
void loop() {
  networkTick();remoteTick();buzzerTick();uiTick();delay(1);
}
