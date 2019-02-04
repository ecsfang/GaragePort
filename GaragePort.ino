#define   RFID

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

unsigned int  heartBeat = 0;
unsigned long beatStart = 0;        // the time the delay started
bool          beatRunning = false;  // true if still waiting for delay to finish

void checkDoor();
void openDoor( int setDelay );
int listID(void);

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


void setup() {
  pinMode(passPin,   OUTPUT); // Connected to Green on tri-color LED to indicate user is valid
  pinMode(failPin,   OUTPUT); // Connected to Red on tri-color LED to indicate user is NOT valid or read failed
  pinMode(relayPin,  OUTPUT); // Connected to relay to activate the door lock
  pinMode(sensorPin, INPUT);  // Connected to magnetic switch on the door

  alarm = 0;

#ifdef RX_DEBUG
  Serial.begin(115200);
  Serial.println("\n\nBooting ...");
  Serial.print("Looking for '");
  Serial.print(ssid);
  Serial.println("' ...");
#endif

//  rfid.begin(); // Connect to the rfid reader

  g_tmo = millis();
  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(failPin, HIGH); // Blink with red/internal LED
    delay(150);
    digitalWrite(failPin, LOW);
    delay(150);
    if ((millis() - g_tmo) > 10000) {
#ifdef RX_DEBUG
      Serial.println("Connection Failed! Rebooting...");
#endif
      digitalWrite(failPin, HIGH); // Turn on red LED
      delay(5000);
      ESP.restart();
    }
  }

  EEPROM.begin(EEROM_SIZE);

  ArduinoOTA.setHostname("garageDoor");

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

void rfidloop();

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
    if ((millis() - g_tmo) > 5000) {
      // Not connected ... try again ...
      WiFi.reconnect();
      g_tmo = 0; // Connected ... ?
    }
  }

  // If we have WiFi connection - do the other network stuff ...
  if(WiFi.status() == WL_CONNECTED ) {
    ArduinoOTA.handle();
  
    if (!client.connected()) {
      long now = millis();
      if ((now - g_now) > 5000) {
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
    // Every 10 minutes ...
    doorStatus = 99;
    checkDoor();
  }

  if (beatRunning && ((millis() - beatStart) >= PULSE_TIME)) {
    beatStart += PULSE_TIME; // this prevents drift in the delays
    heartBeat++;
  }
}

// Ascii -> Hex conversion
byte Asc2Hex(byte val)
{
  byte hex = 0;
  if ( (val >= '0' ) && ( val <= '9' ) )
    hex = val - '0';
  else if ( ( val >= 'a' ) && ( val <= 'f' ) )
    hex = 10 + val - 'a';
  else if ( ( val >= 'A' ) && ( val <= 'F' ) )
    hex = 10 + val - 'A';
  return hex;
}

void sendKey (const char *m, RFIDtag card)
{
  int n = 0;
  n += snprintf (msg+n, MSG_LEN-n, "%s %s", m, card.getTag());
  sendMsg(msg);
}

void printCard(char *lbl, int pos, RFIDtag card)
{
  Serial.print(lbl);
  Serial.print(" [");
  Serial.print(pos);
  Serial.print("] ");
  printKey(card);
}

// Read an ID from EEPROM and save it to the storedCard variable
void readID( int number ) // Number = position in EEPROM to get the card bytes from
{
  int start = (number * _tagLength ) + 1; // Figure out starting position
#ifdef DEBUG
  snprintf (msg, MSG_LEN, "Start: %d\n\n", start);
  Serial.print(msg);
#endif//DEBUG
  char card[_tagLength+1];
  EEPROM.get(start, card);
  card[_tagLength] = 0;
  storedCard.setTag(card);
  printCard("Read", start, storedCard);
}

// Write an array to the EEPROM in the next available slot
void writeID( RFIDtag a )
{
  if ( !findID( a ) ) // Before we write to the EEPROM, check to see if we have seen this card before!
  {
    int num = EEPROM.read(0); // Get the numer of used spaces, position 0 stores the number of ID cards
#ifdef DEBUG
    snprintf (msg, MSG_LEN, "Num: %d\n", num);
    Serial.print(msg);
#endif//DEBUG
    int start = ( num * _tagLength ) + 1; // Figure out where the next slot starts
    char card[_tagLength];
    strncpy(card, a.getTag(), _tagLength);
    EEPROM.put(start, card);
    num++; // Increment the counter by one
    EEPROM.write( 0, num ); // Write the new count to the counter
    printCard("Write", start, a);
    EEPROM.commit();
    successWrite();
  }
  else
  {
    failedWrite();
  }
}

// Delete an array stored in EEPROM from the designated slot
void deleteID( RFIDtag a )
{
  if ( !findID( a ) ) // Before we delete from the EEPROM, check to see if we have this card!
  {
    failedWrite(); // If not
  }
  else
  {
    int num = EEPROM.read(0); // Get the numer of used spaces, position 0 stores the number of ID cards
    int slot; // Figure out the slot number of the card
    int start;// Figure out where the next slot starts
    int looping; // The number of times the loop repeats
    int j;

    int count = EEPROM.read(0); // Read the first Byte of EEPROM that
#ifdef DEBUG
    snprintf (msg, MSG_LEN, "Count: %d\n\n", count); // stores the number of ID's in EEPROM
    Serial.print(msg);
#endif//DEBUG
    slot = findIDSLOT( a ); //Figure out the slot number of the card to delete
    start = ((slot-1) * _tagLength) + 1;
    looping = ((num - slot) * _tagLength);
    num--; // Decrement the counter by one
    EEPROM.write( 0, num ); // Write the new count to the counter

    for ( j = 0; j < looping; j++ ) // Loop the card shift times
    {
      // Shift the array values to 5 places earlier in the EEPROM
      EEPROM.write( start + j, EEPROM.read(start + _tagLength + j));
    }
    for ( int k = 0; k < _tagLength; k++ ) //Shifting loop
    {
      EEPROM.write( start + j + k, 0);
    }
    EEPROM.commit();
    successDelete();
  }
}

// Find the slot number of the id to be deleted
int listID()
{
  int count = EEPROM.read(0); // Read the first Byte of EEPROM that (number of stored ID's)
#ifdef DEBUG
  snprintf (msg, MSG_LEN, "Count: %d\n\n", count);
  Serial.print(msg);
#endif//DEBUG
  for ( int i = 0; i < count; i++ ) // Loop once for each EEPROM entry
  {
    readID(i); // Read an ID from EEPROM, it is stored in storedCard[6]
#ifdef DEBUG
    Serial.print("Card ");
    Serial.print(i+1);
    Serial.print(": ");
#endif
    printKey(storedCard);
  }
}

// Find the slot number of the id to be deleted
int findIDSLOT( RFIDtag find )
{
  int count = EEPROM.read(0); // Read the first Byte of EEPROM that
#ifdef DEBUG
  snprintf (msg, MSG_LEN, "Count: %d\n\n", count); // stores the number of ID's in EEPROM
  Serial.print(msg);
#endif//DEBUG
  for ( int i = 0; i < count; i++ ) // Loop once for each EEPROM entry
  {
    readID(i); // Read an ID from EEPROM, it is stored in storedCard[6]
    if ( find == storedCard ) // Check to see if the storedCard read from EEPROM
    { // is the same as the find[] ID card passed
      Serial.print("FindIDSLOT: We have a matched card!!! \n");
      return i+1; // The slot number of the card
    }
  }
}

void printKey( RFIDtag tag )
{
  Serial.print("[");
  Serial.print(tag.getTag());   // get TAG in ascii format
  Serial.print("] (");
  Serial.print(tag.getCardNumber());  // get cardNumber in long format
  Serial.println(")");
}

void printKeys ( RFIDtag a, RFIDtag b )
{
#ifdef DEBUG
  Serial.print("[");
  Serial.print(a.getTag());
  Serial.print("] -- [");
  Serial.print(b.getTag());
  Serial.println("]");
#endif
}

// Looks in the EEPROM to try to match any of the EEPROM ID's with the passed ID
boolean findID( RFIDtag find )
{
  int count = EEPROM.read(0); // Read the first Byte of EEPROM that
#ifdef DEBUG
  snprintf (msg, MSG_LEN, "Count: %d\n\n", count); // stores the number of ID's in EEPROM
  Serial.print(msg);
#endif//DEBUG
  for ( int i = 0; i < count; i++ ) // Loop once for each EEPROM entry
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
//  rfid.flush();
}

// Flashes Red LED if failed login
void failed()
{
  digitalWrite(passPin, LOW); // Make sure green LED is off
  // Blink red fail LED to indicate failed key
  for(int k=0; k<5; k++) {
    digitalWrite(failPin, HIGH); // Turn on red LED
    delay(100);
    digitalWrite(failPin, LOW); // Turn on red LED
    delay(50);
  }
//  rfid.flush();
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
  static int x = 0;

  long now = millis();
  if (now - lastMsg < 200)
    return;

  lastMsg = now;

#ifdef RX_DEBUG
  if( programMode==1 )
    Serial.println("Program mode on!");
#endif

  switch(x++) {
    case 0:
      digitalWrite(failPin, LOW); // Make sure red LED is off
      digitalWrite(passPin, HIGH); // Make sure green LED is on
      break;
    case 1:
      digitalWrite(failPin, HIGH); // Make sure red LED is on
      digitalWrite(passPin, LOW); // Make sure green LED is off
      break;
    //case 2:
    //  digitalWrite(failPin, LOW); // Make sure red LED is off
    //  digitalWrite(passPin, LOW); // Make sure green LED is off
    default:
      x = 0;
  }
  programMode++;
  if (lastMsg - tmOut > 5000)
    normalModeOn();
}

// Controls LED's for delete mode, cycles through RB
void deleteModeOn()
{
  static int x = 0;

  long now = millis();
  if (now - lastMsg < 200)
    return;

  lastMsg = now;

#ifdef RX_DEBUG
  if( deleteMode==1 )
    Serial.println("Delete mode on!");
#endif

  switch(x++) {
    case 0:
      digitalWrite(failPin, HIGH); // Make sure red LED is on
      digitalWrite(passPin, LOW); // Make sure green LED is off
      break;
    case 1:
      digitalWrite(failPin, LOW); // Make sure red LED is off
      digitalWrite(passPin, LOW); // Make sure green LED is off
    default:
      x=0;
  }
  deleteMode++;
  if (lastMsg - tmOut > 5000)
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
  flashLed("G0", 3, 200);
//  rfid.flush();
}

// Flashes the red LED 3 times to indicate a failed write to EEPROM
void failedWrite()
{
#ifdef RX_DEBUG
  Serial.println("Failed write!");
#endif
  flashLed("R0", 3, 200);
}

// Flashes the red + green LED 3 times to indicate a success delete to EEPROM
void successDelete()
{
#ifdef RX_DEBUG
  Serial.println("Delete ok!");
#endif
  flashLed("*0", 3, 200);
}

// Controls LED's for wipe mode, cycles through BG
void wipeModeOn()
{
#ifdef RX_DEBUG
  Serial.println("Wipe mode on!");
#endif
  flashLed("G0R0", 3, 100);
}

void WipeMemory()
{
  Serial.println("Wipe!");
  wipeMode = true; // If so, enable deletion mode
  alarm = 0;
  wipeModeOn();
  sendMsg("wipe");
  for (int i = 0 ; i < EEPROM.length() ; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
  wipeMode = false;
  normalModeOn(); // Normal mode, blue Power LED is on, all others are off
}

void rfidloop ()
{
  byte val = 0; // Temp variable to hold the current byte
  readCard = reader.getTag();  // if true, then receives a tag object
  Serial.print("Got a card: ");
  printKey(readCard);
  if ( programMode) // Program mode to add a new ID card
  {
    Serial.println("Program mode!");
    // Check to see if it is the master programing card or delete card
    if ( isMaster(readCard) || isDelete(readCard) || isWipe(readCard))
    {
      return; // Ignore! failedWrite();
    }
    else
    {
      writeID(readCard); // If not, write the card to the EEPROM storage
      sendKey("add", readCard);
    }
    programMode = 0;
    normalModeOn(); // Normal mode, blue Power LED is on, all others are off
//    rfid.flush();
  }
  else if ( deleteMode ) // Delete mode to delete an added ID card
  {
    Serial.println("Delete mode!");
    if ( isMaster(readCard) || isDelete(readCard) || isWipe(readCard) ) // Check to see if it is the master programing card or the Delete Card
    {
      return; // Ignore! failedWrite();
    }
    else //if ( !isMaster(readCard) && !isDelete(readCard) )
    {
      deleteID(readCard); // If not, delete the card from the EEPROM sotrage
      sendKey("del", readCard);
    }
    deleteMode = 0;
    normalModeOn(); // Normal mode, blue Power LED is on, all others are off
//    rfid.flush();
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
      if ( findID(readCard) ) // If not, see if the card is in the EEPROM
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
//    rfid.flush();
  }
}
