#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <EEPROM.h> // Needed to write to EEPROM storage
#include <RFIDRdm630.h>
#include "mySSID.h"

#define RX_DEBUG 1

#define failPin   14 // Red LED
#define passPin   12 // Green LED
#define relayPin   4 // Relay
#define rfidPinRx  5 // RDM630 Reader
#define rfidPinTx  0 // RDM630 Reader
#define sensorPin 13 // Door switch

#define EEROM_SIZE  512
#define CARD_LEN    5

int programMode = 0; // Initialize program mode to false
int deleteMode = 0; // Initialize delete mode to false
int wipeMode = 0; // Initialize wipe mode to false

int alarm = 0; // Extra Security

int doorStatus = 99;

#define PULSE_TIME 1000 // Heartbeat - 1 Hz
#define BLINK_DLY 200
#define TIMEOUT_DLY 5000


unsigned int  heartBeat = 0;
unsigned long beatStart = 0;        // the time the delay started
bool          beatRunning = false;  // true if still waiting for delay to finish

void checkDoor();
void openDoor( int setDelay );
int listID(void);
void flashLed(char *ledPgm, int loop, int dly);
void rfidloop();
void initEEPROM();

RFIDRdm630 reader = RFIDRdm630(rfidPinRx,rfidPinTx);    // the reader object.
RFIDtag storedCard;  // Stores an ID read from EEPROM
RFIDtag readCard;    // Sotres an ID read from the RFID reader

WiFiClient espClient;
PubSubClient client(espClient);

long lastMsg = 0;
long tmOut = 0;

#define MSG_LEN 50
char msg[MSG_LEN];

long g_now = 0;
long g_tmo = 0;

#define HASH_TAG  0x5117BEEF
typedef struct {
  uint32_t  hash;
  uint8_t   num;
} EHead_t;

// Get address in EEPROM given slot number (0-n)
#define GET_ROM_ADDRESS(n)  (((n)*sizeof(RFIDtag)) + sizeof(EHead_t))

void setup() {
  pinMode(passPin,   OUTPUT); // Connected to Green on tri-color LED to indicate user is valid
  pinMode(failPin,   OUTPUT); // Connected to Red on tri-color LED to indicate user is NOT valid or read failed
  pinMode(relayPin,  OUTPUT); // Connected to relay to activate the door lock
  pinMode(sensorPin, INPUT);  // Connected to magnetic switch on the door

  digitalWrite(failPin,  LOW);
  digitalWrite(passPin,  LOW);
  digitalWrite(relayPin, LOW);

  alarm = 0;

#ifdef RX_DEBUG
  Serial.begin(115200);
  Serial.println("\n\nBooting ...");
  Serial.print("Looking for '");
  Serial.print(ssid);
  Serial.println("' ...");
#endif

  g_tmo = millis();
  while (WiFi.status() != WL_CONNECTED) {
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

  EEPROM.begin(EEROM_SIZE);

  EHead_t eHead;
  EEPROM.get(0, eHead);
  if( eHead.hash != HASH_TAG ) {
    // EEPROM in bad shape ... reinitialize ... !
#ifdef RX_DEBUG
    Serial.println("Reinitialize EEPROM ...");
#endif
    digitalWrite(failPin, HIGH); // Blink with red/internal LED
    initEEPROM();
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
  doorStatus = 99;
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

void checkDoor() {
  int b = digitalRead(sensorPin);
  if( b != doorStatus ) {
    switch(b) {
      case HIGH:
        Serial.println("Door is open!");
        sendMsg("status", "open");
        break;
      case LOW:
        Serial.println("Door is closed!");
        sendMsg("status", "closed");
        break;
    }
    doorStatus = b;
    delay(20);
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

  // If the rfid-reader is available, check it out ...
  if( reader.isAvailable() )
    rfidloop();

  if(programMode) // Program mode to add a new ID card
  {
    programModeOn();
  }
  else if(deleteMode) // Delete mode to delete an added ID card
  {
    deleteModeOn();
  }

  if( (heartBeat % (10*60)) == 0 ) {
    // Check the door every 10 minutes ...
    doorStatus = 99;
    checkDoor();
  }

  if (beatRunning && ((millis() - beatStart) >= PULSE_TIME)) {
    beatStart += PULSE_TIME; // this prevents drift in the delays
    heartBeat++;
  }
}

void sendKey (const char *m, RFIDtag card)
{
  int n = 0;
  n += snprintf (msg+n, MSG_LEN-n, "%s %s", m, card.getTag());
  sendMsg(msg);
}

void printTag( RFIDtag tag )
{
  Serial.print("[");
  Serial.print(tag.getTag());   // get TAG in ascii format
  Serial.print("]");
}

void printKey( RFIDtag tag )
{
  printTag(tag);
  Serial.print(" (");
  Serial.print(tag.getCardNumber());  // get cardNumber in long format
  Serial.println(")");
}

void printKeys ( RFIDtag a, RFIDtag b )
{
#ifdef DEBUG
  printTag(a);
  Serial.print(" -- ");
  printTag(b);
  Serial.println();
#endif
}

void printCard(char *lbl, int pos, RFIDtag card)
{
  Serial.print(lbl);
  Serial.print(" [");
  Serial.print(pos);
  Serial.print("@");
  Serial.print(GET_ROM_ADDRESS(pos));
  Serial.print("] ");
  printKey(card);
}

// Read an ID given slot number from EEPROM and save it to the storedCard variable
void readID( int number )
{
  int start = GET_ROM_ADDRESS(number); // Figure out starting position
  EEPROM.get(start, storedCard);
  printCard("Read", number, storedCard);
}

// Write a new ID to the EEPROM in the next available slot
void writeID( RFIDtag newCard )
{
  if ( findID( newCard ) >= 0 ) // Before we write to the EEPROM, check to see if we have seen this card before!
  {
    // ID already exists in EEPROM ...
    failedWrite();
    return;
  }

  EHead_t eHead;
  EEPROM.get(0, eHead); // Get the number of used slots ...
  eHead.num++; // Increment the counter by one
  int start = GET_ROM_ADDRESS(eHead.num); // Figure out where the next slot starts
  EEPROM.put(start, newCard);
  EEPROM.put( 0, eHead ); // Write the new count to the counter
  printCard("Write", eHead.num, newCard);
  EEPROM.commit();
  successWrite();
}

// Delete a card stored in EEPROM from the designated slot
void deleteID( RFIDtag a )
{
  int slot = findID( a ); // Figure out the slot number of the card to delete

  if ( slot < 0 ) // Before we delete from the EEPROM, check to see if we have this card!
  {
#ifdef DEBUG
    snprintf (msg, MSG_LEN, "Can't find '%s'!\n\n", a.getTag()); // stores the number of ID's in EEPROM
    Serial.print(msg);
#endif//DEBUG
    failedWrite(); // ID not found in EEPROM ...
  }
  else
  {
    EHead_t eHead;
    EEPROM.get(0, eHead); // Get the numer of used spaces, position 0 stores the number of ID cards

#ifdef DEBUG
    snprintf (msg, MSG_LEN, "Count: %d\n\n", eHead.num); // stores the number of ID's in EEPROM
    Serial.print(msg);
#endif//DEBUG

    // Shift down rest of the entries ...
    for ( ; slot < (eHead.num-1); slot++ ) // Loop the card shift times
    {
      // Shift the array values down one step in the EEPROM
      EEPROM.get(GET_ROM_ADDRESS(slot+1), storedCard);
      EEPROM.put(GET_ROM_ADDRESS(slot), storedCard);
    }
    eHead.num--; // Decrement the counter by one
    EEPROM.put( 0, eHead ); // Write the new count to the counter
    EEPROM.commit();
    successDelete();
  }
}

// List all cards found in the EEPROM ...
int listID()
{
  EHead_t eHead;
  EEPROM.get(0, eHead); // Read the first Byte of EEPROM that
#ifdef DEBUG
  if( eHead.num )
    snprintf (msg, MSG_LEN, "Found %d cards in EEPROM\n\n", eHead.num, eHead.num > 1 ? "cards" : "card");
  else
    snprintf (msg, MSG_LEN, "No cards found in EEPROM!!\n\n");
  Serial.print(msg);
#endif//DEBUG
  for ( int i = 0; i < eHead.num; i++ ) // Loop once for each EEPROM entry
  {
    readID(i); // Read an ID from EEPROM, it is stored in storedCard[6]
#ifdef DEBUG
    printCard("Card", i, storedCard);
#else
    printKey(storedCard);
#endif
  }
  return eHead.num;
}

// Find the slot number of the id to be deleted
int findID( RFIDtag find )
{
  EHead_t eHead;
  EEPROM.get(0, eHead); // Read the first Byte of EEPROM that
#ifdef DEBUG
  snprintf (msg, MSG_LEN, "Count: %d\n\n", eHead.num); // stores the number of ID's in EEPROM
  Serial.print(msg);
#endif//DEBUG
  for ( int i = 0; i < eHead.num; i++ ) // Loop once for each EEPROM entry
  {
    readID(i); // Read an ID from EEPROM, it is stored in storedCard[6]
    if ( find == storedCard ) // Check to see if the storedCard read from EEPROM
    { // is the same as the find[] ID card passed
      Serial.print("FindID: We have a matched card!!! \n");
      return i; // The slot number of the card
    }
  }
  return -1;
}

#if 0
// Looks in the EEPROM to try to match any of the EEPROM ID's with the passed ID
boolean findID( RFIDtag find )
{
  EHead_t eHead;
  EEPROM.get(0, eHead); // Read the header of the EEPROM
#ifdef DEBUG
  snprintf (msg, MSG_LEN, "Count: %d\n\n", eHead.num);
  Serial.print(msg);
#endif//DEBUG
  for ( int i = 0; i < eHead.num; i++ ) // Loop once for each EEPROM entry
  {
    readID(i); // Read an ID from EEPROM, it is stored in storedCard[6]
    if ( find == storedCard ) // Check to see if the storedCard read from EEPROM
    { // is the same as the find[] ID card passed
      Serial.print("FindID: We have a matched card!!! \n");
      return true;
    }
  }
  return false;
}
#endif

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

RFIDtag masterCard("03001303F500");
RFIDtag wipeCard("030074914800");
RFIDtag deleteCard("03001303E200");

// Check to see if the ID passed is the master programing card
boolean isMaster( RFIDtag test )
{
  return test == masterCard;
}

// Check to see if the ID passed is the wipe memory card
boolean isWipe( RFIDtag test )
{
  return test == wipeCard;
}

// Check to see if the ID passed is the deletion card
boolean isDelete( RFIDtag test )
{
  return test == deleteCard;
}

// Controls LED's for Normal mode, Blue on, all others off
void normalModeOn()
{
  digitalWrite(passPin,  LOW); // Make sure Green LED is off
  digitalWrite(failPin,  LOW); // Make sure Red LED is off
  digitalWrite(relayPin, LOW); // Make sure Door is Locked
  programMode = deleteMode = 0;
}

// Controls LED's for program mode, cycles through RGB
void programModeOn()
{
  long now = millis();
  if (now - lastMsg < BLINK_DLY)
    return;

  lastMsg = now;

#ifdef RX_DEBUG
  if( programMode==1 )
    Serial.println("Program mode on!");
#endif

  digitalWrite(failPin, programMode & 1 ? HIGH : LOW); // Toggle red led ...
  digitalWrite(passPin, programMode & 1 ? LOW : HIGH); // Toggle green led ...

  programMode++;
  if (lastMsg - tmOut > TIMEOUT_DLY)
    normalModeOn();
}

// Controls LED's for delete mode, cycles through RB
void deleteModeOn()
{
  long now = millis();
  if (now - lastMsg < BLINK_DLY)
    return;

  lastMsg = now;

#ifdef RX_DEBUG
  if( deleteMode==1 )
    Serial.println("Delete mode on!");
#endif

  digitalWrite(passPin, LOW); // Green led off ...
  digitalWrite(failPin, deleteMode & 1 ? HIGH : LOW); // Toggle red led ...

  deleteMode++;
  if (lastMsg - tmOut > TIMEOUT_DLY)
    normalModeOn();
}

void flashLed(char *ledPgm, int loop, int dly)
{
  char *led;
  while( loop-- ) {
    led = ledPgm;
    while(led) {
      switch(*led++) {
        case '0':
          digitalWrite(failPin, LOW); // Make sure red LED is off
          digitalWrite(passPin, LOW); // Make sure green LED is off
          break;
        case 'R':
          digitalWrite(failPin, HIGH); // Make sure red LED is on
          digitalWrite(passPin, LOW); // Make sure green LED is off
          break;
        case 'G':
          digitalWrite(failPin, LOW); // Make sure red LED is off
          digitalWrite(passPin, HIGH); // Make sure green LED is on
          break;
        case '*':
          digitalWrite(failPin, HIGH); // Make sure red LED is on
          digitalWrite(passPin, HIGH); // Make sure green LED is on
          break;
      }
      delay(dly);
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
#ifdef RX_DEBUG
  Serial.println("Wipe mode on!");
#endif
  flashLed("G0R0", 3, BLINK_DLY/2);
}

void initEEPROM()
{
  EHead_t eHead;
#ifdef RX_DEBUG
  Serial.println("Initialize EEPROM ...");
#endif
  for (int i = 0 ; i < EEPROM.length() ; i++) {
    EEPROM.write(i, 0);
  }
  eHead.hash = HASH_TAG;
  eHead.num = 0;
  EEPROM.put(0, eHead);
  EEPROM.commit();
}

void WipeMemory()
{
  Serial.println("Wipe!");
  wipeMode = true; // If so, enable deletion mode
  alarm = 0;
  wipeModeOn();
  sendMsg("wipe");
  initEEPROM();
  wipeMode = false;
  normalModeOn(); // Normal mode, blue Power LED is on, all others are off
}

void rfidloop ()
{
  readCard = reader.getTag();  // if true, then receives a tag object
  Serial.print("Got a card: ");
  printKey(readCard);

  if ( programMode) // Program mode to add a new ID card
  {
    Serial.println("Program mode!");
    // Check to see if it is the master programing card or delete card
    if ( isMaster(readCard) || isDelete(readCard) || isWipe(readCard))
    {
      return; // Ignore!
    }
    else
    {
      writeID(readCard); // If not, write the card to the EEPROM storage
      sendKey("add", readCard);
    }
    normalModeOn();
  }
  else if ( deleteMode ) // Delete mode to delete an added ID card
  {
    Serial.println("Delete mode!");
    // Check to see if it is the master programing card or the Delete Card
    if ( isMaster(readCard) || isDelete(readCard) || isWipe(readCard) )
    {
      return; // Ignore!
    }
    else
    {
      deleteID(readCard); // If not, delete the card from the EEPROM sotrage
      sendKey("del", readCard);
    }
    normalModeOn();
  }
  else if ( wipeMode ) // Wipe mode to wipe out the EEPROM
  {
    Serial.println("Wipe mode!?");
  }
  // Normal Operation...
  else
  {
    Serial.println("Handle card ...");
    if ( isMaster( readCard ) ) // Check to see if the card is the master programing card
    {
      Serial.println("Master!");
      programMode = 1; // If so, enable programing mode
      tmOut = millis();
      alarm = 0;
      programModeOn(); // Program Mode cycles through RGB waiting to read a new card
    }
    else if ( isDelete( readCard ) ) // Check to see if the card is the deletion card
    {
      Serial.println("Delete!");
      deleteMode = 1; // If so, enable deletion mode
      tmOut = millis();
      alarm = 0;
      deleteModeOn(); // Delete Mode cycles through RB waiting to read a new card
    }
    else if ( isWipe( readCard ) ) // Check to see if the card is the deletion card
    {
      Serial.println("Wipe!");
      WipeMemory();
    }
    else
    {
      if ( findID(readCard) >= 0 ) // If not, see if the card is in the EEPROM
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
