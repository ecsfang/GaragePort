#include <Stepper.h>

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <RFIDRdm630.h>
#include <Ticker.h>
#include "idcard.h"
#include "mySSID.h"
#include "config.h"

const char* door_alias = DOOR1_ALIAS;
const char* mqtt_door_action_topic = MQTT_DOOR1_ACTION_TOPIC;
const char* mqtt_door_status_topic = MQTT_DOOR1_STATUS_TOPIC;
const int door_openPin = DOOR1_OPEN_PIN;
const int door_closePin = DOOR1_CLOSE_PIN;
const int door_statusPin = DOOR1_STATUS_PIN;
const char* door_statusSwitchLogic = DOOR1_STATUS_SWITCH_LOGIC;

enum Mode_enum {IDLE, PROGRAM, DELETE, WIPE};
enum Door_enum {UNKNOWN, DOOR_OPEN, DOOR_CLOSED};

uint8_t mode = IDLE;
bool bNewMode = false;

int alarm = 0; // Extra Security

int oldDoorStatus = UNKNOWN;
int newDoorStatus = UNKNOWN;

Ticker flipper;

void checkDoor();
void doCheckDoor();
void updateDoor();
void openDoor(int doorPin, int setDelay );
void flashLed(char *ledPgm, int loop, int dly);
void rfidloop();

String availabilityBase = mqtt_client;
String availabilitySuffix = "/availability";
String availabilityTopicStr = availabilityBase + availabilitySuffix;
const char* availabilityTopic = availabilityTopicStr.c_str();
const char* birthMessage = "online";
const char* lwtMessage = "offline";

RFIDRdm630 reader = RFIDRdm630(rfidPinRx,rfidPinTx);    // the reader object.

WiFiClient espClient;
PubSubClient client(espClient);

long lastMsg = 0;
long tmOut = 0;

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
  flashLed("G0R0", 5, BLINK_DLY/2);
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

  listID();
  g_now = millis();
  g_tmo = 0;  // WiFi timeout ...

  // Start keeping an eye on the door-switch ...
  attachInterrupt(digitalPinToInterrupt(sensorPin), checkDoor, CHANGE);
#ifdef USE_INTERRUPT
  // ... and the RFID-reader ...
  attachInterrupt(digitalPinToInterrupt(cardInt), rfidloop, FALLING);
#endif
  // fCheck the door every 5 minutes
  flipper.attach(5*60, doCheckDoor);
  // Once connected and done, publish an announcement...
  sendMsg("ready");
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  
  Serial.println();

  String topicToProcess = topic;
  payload[length] = '\0';
  String payloadToProcess = (char*)payload;
  triggerDoorAction(topicToProcess, payloadToProcess);
}
/*
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
  doCheckDoor();
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
***/

// Function called by callback() when a message is received 
// Passes the message topic as the "requestedDoor" parameter and the message payload as the "requestedAction" parameter

void triggerDoorAction(String requestedDoor, String requestedAction) {
  if (requestedDoor == mqtt_door_action_topic && requestedAction == "OPEN") {
    Serial.print("Triggering ");
    Serial.print(door_alias);
    Serial.println(" OPEN relay!");
    openDoor(door_openPin, 1);
  }
  else if (requestedDoor == mqtt_door_action_topic && requestedAction == "CLOSE") {
    Serial.print("Triggering ");
    Serial.print(door_alias);
    Serial.println(" CLOSE relay!");
    openDoor(door_closePin, 1);
  }
  else if (requestedDoor == mqtt_door_action_topic && requestedAction == "STATE") {
    Serial.print("Publishing on-demand status update for ");
    Serial.print(door_alias);
    Serial.println("!");
    publish_birth_message();
    publish_door_status();
  }
  else { Serial.println("Unrecognized action payload... taking no action!");
  }
}

// Functions that check door status and publish an update when called

void publish_door_status() {
  if (digitalRead(door_statusPin) == LOW) {
    if (door_statusSwitchLogic == "NO") {
      Serial.print(door_alias);
      Serial.print(" closed! Publishing to ");
      Serial.print(mqtt_door_status_topic);
      Serial.println("...");
      client.publish(mqtt_door_status_topic, "closed", true);
    }
    else if (door_statusSwitchLogic == "NC") {
      Serial.print(door_alias);
      Serial.print(" open! Publishing to ");
      Serial.print(mqtt_door_status_topic);
      Serial.println("...");
      client.publish(mqtt_door_status_topic, "open", true);      
    }
    else {
      Serial.println("Error! Specify only either NO or NC for DOOR_STATUS_SWITCH_LOGIC! Not publishing...");
    }
  }
  else {
    if (door_statusSwitchLogic == "NO") {
      Serial.print(door_alias);
      Serial.print(" open! Publishing to ");
      Serial.print(mqtt_door_status_topic);
      Serial.println("...");
      client.publish(mqtt_door_status_topic, "open", true);
    }
    else if (door_statusSwitchLogic == "NC") {
      Serial.print(door_alias);
      Serial.print(" closed! Publishing to ");
      Serial.print(mqtt_door_status_topic);
      Serial.println("...");
      client.publish(mqtt_door_status_topic, "closed", true);      
    }
    else {
      Serial.println("Error! Specify only either NO or NC for DOOR_STATUS_SWITCH_LOGIC! Not publishing...");
    }
  }
}

// Function that publishes birthMessage

void publish_birth_message() {
  // Publish the birthMessage
  Serial.print("Publishing birth message \"");
  Serial.print(birthMessage);
  Serial.print("\" to ");
  Serial.print(availabilityTopic);
  Serial.println("...");
  client.publish(availabilityTopic, birthMessage, true);
}

void reconnect() {
  // Loop until we're reconnected
  if(!client.connected()) {
#ifdef RX_DEBUG
    Serial.print("Attempting another MQTT connection...");
#endif
    // Attempt to connect
    if (client.connect(mqtt_client, mqtt_user, mqtt_pass, availabilityTopic, 0, true, lwtMessage)) {
      digitalWrite(passPin, HIGH); // Turn on green LED
      Serial.println("connected");

      // Publish the birth message on connect/reconnect
      publish_birth_message();

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
  #define MSG_LEN 32
  char msg[MSG_LEN];
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
  sendMsg(m, card.getTag());
}

void updateDoor() {
  if( newDoorStatus != oldDoorStatus ) {
    switch(newDoorStatus) {
      case DOOR_OPEN:
        Serial.println("Door is open!");
        sendMsg("status", "open");
        break;
      case DOOR_CLOSED:
        Serial.println("Door is closed!");
        sendMsg("status", "closed");
        break;
    }
    oldDoorStatus = newDoorStatus;
    delay(20);
  }
}

// Check the door status
void checkDoor() {
  newDoorStatus = digitalRead(sensorPin) ? DOOR_OPEN : DOOR_CLOSED;
}

// Force a check and update on the door status
void doCheckDoor() {
  oldDoorStatus = UNKNOWN;
  checkDoor();
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

  updateDoor();
}

// Opens door and turns on the green LED for setDelay seconds
void openDoor(int doorPin,  int setDelay )
{
  Serial.print("Open door!\n");
  sendKey("key", readCard);
  setDelay *= 1000; // Sets delay in seconds
  digitalWrite(failPin, LOW); // Turn off red LED
  digitalWrite(passPin, HIGH); // Turn on green LED
  digitalWrite(doorPin, HIGH); // Unlock door!

  delay(setDelay); // Hold door lock open for some seconds

  digitalWrite(doorPin, LOW); // Relock door

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

#define XOR(a,b)  ((!a) != (!b))
void flashLed(char *ledPgm, int loop, int dly)
{
  char *led;
  while( loop-- ) {
    led = ledPgm;
    while(*led) {
      // Note! This bit fiddling trick only works for ascii 0, R, G or *
      digitalWrite(failPin, XOR(*led&0x10, *led&0x20) ? HIGH : LOW); // Set red LED
      digitalWrite(passPin, (*led&0x09) ? HIGH : LOW); // Setgreen LED
      delay(dly);
      led++;
    }
  }
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
          openDoor(relayPin, 1); // If it is, open the door lock
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
