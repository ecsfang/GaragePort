#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <RFIDRdm630.h>
#include "idcard.h"
#include "mySSID.h"

#define RX_DEBUG 1
//#define USE_INTERRUPT

#define failPin   D5 // 14 // Red LED
#define passPin   D6 // 12 // Green LED
#define relayPin  4 //D2 //  4 // Relay
#define rfidPinRx 5 // D1 //  5 // RDM630 Reader
#define rfidPinTx 0 //D3 //  0 // RDM630 Reader
#ifdef USE_INTERRUPT
#  define cardInt   D8 // 15 // RDM630 interrupt
#endif
#define sensorPin D7 // 13 // Door switch

#define CARD_LEN    5

enum Mode_enum {IDLE, PROGRAM, DELETE, WIPE};
enum Door_enum {UNKNOWN, DOOR_OPEN, DOOR_CLOSED};

uint8_t mode = IDLE;
bool bNewMode = false;

int alarm = 0; // Extra Security

int doorStatus = UNKNOWN;

#define PULSE_TIME 1000 // Heartbeat - 1 Hz
#define BLINK_DLY 200
#define TIMEOUT_DLY 5000

unsigned int  heartBeat = 0;
unsigned long beatStart = 0;        // the time the delay started
bool          beatRunning = false;  // true if still waiting for delay to finish

void checkDoor();
void openDoor( int setDelay );
void flashLed(char *ledPgm, int loop, int dly);
void rfidloop();

RFIDRdm630 reader = RFIDRdm630(rfidPinRx,rfidPinTx);    // the reader object.

WiFiClient espClient;
PubSubClient client(espClient);

long lastMsg = 0;
long tmOut = 0;

#define MSG_LEN 50
char msg[MSG_LEN];

long g_now = 0;
long g_tmo = 0;

void setup() {
  pinMode(passPin,   OUTPUT); // Connected to Green on tri-color LED to indicate user is valid
  pinMode(failPin,   OUTPUT); // Connected to Red on tri-color LED to indicate user is NOT valid or read failed
  pinMode(relayPin,  OUTPUT); // Connected to relay to activate the door lock
  pinMode(sensorPin, INPUT);  // Connected to magnetic switch on the door
#ifdef USE_INTERRUPT
  pinMode(cardInt,   INPUT);  // Connected to RDM630 interrupt pin
#endif

  digitalWrite(failPin,  LOW);
  digitalWrite(passPin,  LOW);
  digitalWrite(relayPin, LOW);

  alarm = 0;

  Serial.begin(115200);

#ifdef RX_DEBUG
  delay(2500);
  Serial.println("\n\nBooting ...");
  Serial.print("\n\nDoor pin: ");
  Serial.println(relayPin);
  flashLed("G0R0", 10, BLINK_DLY/2);
#endif

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

#ifdef RX_DEBUG
  Serial.print("Looking for '");
  Serial.print(ssid);
  Serial.println("' ...");
#endif

  g_tmo = millis();
  while (WiFi.status() != WL_CONNECTED) {
#ifdef RX_DEBUG
    Serial.print(".");
#endif
    flashLed("R0", 1, 150);  // Blink with red/internal LED
    if ((millis() - g_tmo) > (TIMEOUT_DLY*2)) {
#ifdef RX_DEBUG
      Serial.println("Connection Failed! Rebooting...");
#endif
      digitalWrite(failPin, HIGH); // Turn on red LED
      delay(TIMEOUT_DLY);
      ESP.restart();
    }
  }

  if( !initEEPROM() ) {
    // Memory was re-initialized ...
    digitalWrite(failPin, HIGH); // Blink with red/internal LED
    delay(2000);
    digitalWrite(failPin, LOW);
  }

  ArduinoOTA.setHostname("garageDoor");
  ArduinoOTA.setPassword(flashpw);

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
#ifdef RX_DEBUG
  Serial.println("GaragePort v3 -- Ready!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
#endif
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  // Once connected, publish an announcement...
  sendMsg("ready");
  listID();
  g_now = millis();
  beatStart = millis();
  g_tmo = 0;  // WiFi timeout ...
  beatRunning = true;

  // Start keeping an eye on the door-switch ...
  attachInterrupt(digitalPinToInterrupt(sensorPin), checkDoor, CHANGE);
#ifdef USE_INTERRUPT
  // ... and the RFID-reader ...
  attachInterrupt(digitalPinToInterrupt(cardInt), rfidloop, FALLING);
#endif
}

void callback(char* topic, byte* payload, unsigned int length) {
  String strTopic = String((char*)topic);

#ifdef RX_DEBUG
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
#endif

 if(strcmp(topic, "toGarage/door/status") == 0)
 {
  doorStatus = UNKNOWN;
  checkDoor();
 }

 if(strcmp(topic, "toGarage/door/open") == 0)
 {
  openDoor(1);
 }

 if(strcmp(topic, "toGarage/door/erase") == 0)
 {
  openDoor(1);
 }
}

void reconnect() {
  // Loop until we're reconnected
  if(!client.connected()) {
#ifdef RX_DEBUG
    Serial.print("Attempting another MQTT connection...");
#endif
    // Attempt to connect
    if (client.connect("theGarageClient")) {
      digitalWrite(passPin, HIGH); // Turn on green LED
      Serial.println("connected");
      // Once connected, publish an announcement...
      sendMsg("ready");
      // ... and resubscribe
      client.subscribe("toGarage/#");
      delay(250);
      digitalWrite(passPin, LOW); // Turn off green LED
    } else {
      digitalWrite(failPin, HIGH); // Turn on red LED
#ifdef RX_DEBUG
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
#endif
      delay(500);
      digitalWrite(failPin, LOW); // Turn off red LED
    }
  }
}

void sendMsg(const char *topic, const char *m)
{
  int n = 0;
  if (client.connected()) {
    n = snprintf (msg, MSG_LEN, "fromGarage");
    if( topic )
      n += snprintf (msg+n, MSG_LEN-n, "/%s", topic);
    client.publish(msg, m);
#ifdef RX_DEBUG
    Serial.print("Publish message: ");
    Serial.print(msg);
    Serial.print(" ");
    Serial.println(m);
#endif
  }
}

void sendMsg(const char *m)
{
  sendMsg(NULL, m);
}

void sendKey( const char *m, RFIDtag card )
{
//  static char msgBuf[MSG_LEN];
//  snprintf (msgBuf, MSG_LEN, "%s %s", m, card.getTag());
  sendMsg(m, card.getTag());
}

void checkDoor() {
  uint8_t b = digitalRead(sensorPin) ? DOOR_OPEN : DOOR_CLOSED;
  if( b != doorStatus ) {
    switch(b) {
      case DOOR_OPEN:
        Serial.println("Door is open!");
        sendMsg("status", "open");
        break;
      case DOOR_CLOSED:
        Serial.println("Door is closed!");
        sendMsg("status", "closed");
        break;
    }
    doorStatus = b;
    delay(20);
    Serial.println("Done!");
  }
}

void loop() {

  // If WiFi connection is lost - try again every 5 seconds ...
  if(WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0,0,0,0)) {
    if( !g_tmo )
      g_tmo = millis();
    if ((millis() - g_tmo) > TIMEOUT_DLY) {
      // Not connected ... try again ...
      WiFi.reconnect();
      g_tmo = 0; // Connected ... ?
    }
  }

  // If we have WiFi connection - do the other network stuff ...
  if(WiFi.status() == WL_CONNECTED ) {
    ArduinoOTA.handle();
  
    if (!client.connected()) {
      if ((millis() - g_now) > TIMEOUT_DLY) {
        reconnect();
        g_now = millis();
      }
    }
  
    if (client.connected()) {
      client.loop();
    }
  }

#ifndef USE_INTERRUPT
  rfidloop();
#endif

  switch(mode) {
  case PROGRAM:
    // Program mode to add a new ID card
    programModeOn();
    break;
  case DELETE:
    // Delete mode to delete an added ID card
    deleteModeOn();
    break;
  }

  if (beatRunning && ((millis() - beatStart) >= PULSE_TIME)) {
    // New heartbeat ...
    if( (heartBeat % (10*60)) == 0 ) {
      // Check the door every 10 minutes ...
      doorStatus = UNKNOWN;
      checkDoor();
    }
    // Update beat counter ...
    beatStart += PULSE_TIME; // this prevents drift in the delays
    heartBeat++;
  }
}

// Opens door and turns on the green LED for setDelay seconds
void openDoor( int setDelay )
{
  Serial.print("Open door!\n");
  sendKey("key", readCard);
  setDelay *= 1000; // Sets delay in seconds
  digitalWrite(failPin, LOW); // Turn off red LED
  digitalWrite(passPin, HIGH); // Turn on green LED
  digitalWrite(relayPin, HIGH); // Unlock door!

  delay(setDelay); // Hold door lock open for some seconds

  digitalWrite(relayPin, LOW); // Relock door

  delay(setDelay); // Hold green LED on for some more seconds

  digitalWrite(passPin, LOW); // Turn off green LED
}

// Flashes Red LED if failed login
void failed()
{
  flashLed("R0", 10, 50);
  digitalWrite(passPin, LOW); // Make sure green LED is off
  // Blink red fail LED to indicate failed key
  for(int k=0; k<5; k++) {
    digitalWrite(failPin, HIGH); // Turn on red LED
    delay(100);
    digitalWrite(failPin, LOW); // Turn on red LED
    delay(50);
  }
}

// Controls LED's for Normal mode, Blue on, all others off
void normalModeOn()
{
  digitalWrite(passPin,  LOW); // Make sure Green LED is off
  digitalWrite(failPin,  LOW); // Make sure Red LED is off
  digitalWrite(relayPin, LOW); // Make sure Door is Locked
  mode = IDLE;
  bNewMode = false;
}

// Controls LED's for program mode, cycles through RGB
void programModeOn()
{
  static int led = 0;
  long now = millis();
  mode = PROGRAM;
  if (now - lastMsg < BLINK_DLY)
    return;

  lastMsg = now;

#ifdef RX_DEBUG
  if( !bNewMode )
    Serial.println("Program mode on!");
#endif
  bNewMode = true;

  digitalWrite(failPin, led & 1 ? HIGH : LOW); // Toggle red led ...
  digitalWrite(passPin, led & 1 ? LOW : HIGH); // Toggle green led ...

  led++;
  if (lastMsg - tmOut > TIMEOUT_DLY)
    normalModeOn();
}

// Controls LED's for delete mode, cycles through RB
void deleteModeOn()
{
  static int led = 0;
  long now = millis();
  mode = DELETE;
  if (now - lastMsg < BLINK_DLY)
    return;

  lastMsg = now;

#ifdef RX_DEBUG
  if( !bNewMode )
    Serial.println("Delete mode on!");
#endif
  bNewMode = true;

  digitalWrite(passPin, LOW); // Green led off ...
  digitalWrite(failPin, led & 1 ? HIGH : LOW); // Toggle red led ...

  led++;
  if (lastMsg - tmOut > TIMEOUT_DLY)
    normalModeOn();
}

void flashLed(char *ledPgm, int loop, int dly)
{
  char *led;
  while( loop-- ) {
    led = ledPgm;
    while(*led) {
      switch(*led++) {
        case '0':
          Serial.print("0");
          digitalWrite(failPin, LOW); // Make sure red LED is off
          digitalWrite(passPin, LOW); // Make sure green LED is off
          break;
        case 'R':
          Serial.print("R");
          digitalWrite(failPin, HIGH); // Make sure red LED is on
          digitalWrite(passPin, LOW); // Make sure green LED is off
          break;
        case 'G':
          Serial.print("G");
          digitalWrite(failPin, LOW); // Make sure red LED is off
          digitalWrite(passPin, HIGH); // Make sure green LED is on
          break;
        case '*':
          Serial.print("*");
          digitalWrite(failPin, HIGH); // Make sure red LED is on
          digitalWrite(passPin, HIGH); // Make sure green LED is on
          break;
      }
      delay(dly);
    }
  }
  Serial.println();
}
// Flashes the green LED 3 times to indicate a successful write to EEPROM
void successWrite()
{
#ifdef RX_DEBUG
  Serial.println("Write OK!");
#endif
  flashLed("G0", 3, BLINK_DLY);
}

// Flashes the red LED 3 times to indicate a failed write to EEPROM
void failedWrite()
{
#ifdef RX_DEBUG
  Serial.println("Failed write!");
#endif
  flashLed("R0", 3, BLINK_DLY);
}

// Flashes the red + green LED 3 times to indicate a success delete to EEPROM
void successDelete()
{
#ifdef RX_DEBUG
  Serial.println("Delete ok!");
#endif
  flashLed("*0", 3, BLINK_DLY);
}

// Controls LED's for wipe mode, cycles through BG
void wipeModeOn()
{
  mode = WIPE;
#ifdef RX_DEBUG
  Serial.println("Wipe mode on!");
#endif
  flashLed("G0R0", 3, BLINK_DLY/2);
}

void WipeMemory()
{
  wipeModeOn();
  Serial.println("Wipe!");
  alarm = 0;
  sendMsg("wipe");
  eraseEEPROM();
  normalModeOn(); // Normal mode, blue Power LED is on, all others are off
}

void rfidloop()
{
  if (reader.isAvailable())
  {
    readCard = reader.getTag(); // if true, then receives a tag object
    Serial.print("Got a card: ");
    printKey(readCard);

    switch (mode)
    {
    case PROGRAM:
      // Program mode to add a new ID card
      Serial.println("Program mode!");
      // Check to see if it is the master programing card or delete card
      if (whichCard(readCard) == OTHER) {
        if( writeID(readCard) ) { // If not, write the card to the EEPROM storage
          sendKey("add", readCard);
          successWrite();
        } else
          failedWrite();
        normalModeOn();
      }
      break;
    case DELETE:
      // Delete mode to delete an added ID card
      Serial.println("Delete mode!");
      // Check to see if it is the master programing card or the Delete Card
      if (whichCard(readCard) == OTHER) {
        if( deleteID(readCard ) ) { // If not, delete the card from the EEPROM sotrage
          sendKey("del", readCard);
          successWrite();
        } else
          failedWrite();
        normalModeOn();
      }
      break;
    case WIPE:
      // Wipe mode to wipe out the EEPROM
      Serial.println("Wipe mode!?");
      break;
    case IDLE:
      // Normal Operation...
      Serial.println("Handle card ...");
      switch (whichCard(readCard))
      {
      case MASTER_CARD:
        Serial.println("Master!");
        tmOut = millis();
        alarm = 0;
        programModeOn(); // Program Mode cycles through RGB waiting to read a new card
        break;
      case DELETE_CARD:
        Serial.println("Delete!");
        tmOut = millis();
        alarm = 0;
        deleteModeOn(); // Delete Mode cycles through RB waiting to read a new card
        break;
      case WIPE_CARD:
        Serial.println("Wipe!");
        WipeMemory();
        break;
      case OTHER:
        if (findID(readCard) >= 0) // If not, see if the card is in the EEPROM
        {
          Serial.println("Valid card - press button!");
          openDoor(1); // If it is, open the door lock
          alarm = 0;
        }
        else
        {
          Serial.println("Unknown card!");
          sendKey("fail", readCard);
          failed(); // If not, show that the ID was not valid
          alarm++;
          delay(1000);
        }
      }
    }
  }
}
