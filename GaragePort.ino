#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <EEPROM.h> // Needed to write to EEPROM storage
#include <rdm630.h>
#include "mySSID.h"

#define RX_DEBUG 1

#define failPin   14 // Red LED
#define passPin   12 // Green LED
#define doorPin    4 // Relay
#define rfidPinRx  5 // RDM630 Reader
#define rfidPinTx  0 // RDM630 Reader
#define switchPin 13 // Door switch

#define EEROM_SIZE  512

int programMode = 0; // Initialize program mode to false
int deleteMode = 0; // Initialize delete mode to false
int wipeMode = 0; // Initialize wipe mode to false
//boolean match = false; // initialize card match to false

byte storedCard[6]; // Stores an ID read from EEPROM
byte readCard[6]; // Sotres an ID read from the RFID reader

int alarm = 0; // Extra Security

int doorStatus = 99;

void checkDoor();
void openDoor( int setDelay );
int listID(void);

rdm630 rfid(rfidPinRx, rfidPinTx);  //TX-pin of RDM630 connected to ESP RX

WiFiClient espClient;
PubSubClient client(espClient);

long lastMsg = 0;
long tmOut = 0;

#define MSG_LEN 50
char msg[MSG_LEN];

long g_now = 0;

void setup() {
  pinMode(passPin, OUTPUT); // Connected to Green on tri-color LED to indicate user is valid
  pinMode(failPin, OUTPUT); // Connected to Red on tri-color LED to indicate user is NOT valid or read failed
  pinMode(doorPin, OUTPUT); // Connected to relay to activate the door lock
  pinMode(switchPin, INPUT); // Connected to magnetic switch on the door

  alarm = 0;

#ifdef RX_DEBUG
  Serial.begin(115200);
  Serial.println("\n\nBooting ...");
#endif

  rfid.begin(); // Connect to the rfid reader

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
#ifdef RX_DEBUG
    Serial.println("Connection Failed! Rebooting...");
#endif
    delay(5000);
    ESP.restart();
  }

  EEPROM.begin(EEROM_SIZE);

  ArduinoOTA.setHostname("garageDoor");

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

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
  Serial.println("GaragePort v2 -- Ready!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
#endif
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  // Once connected, publish an announcement...
  //client.publish("fromGarage", "ready");
  listID();
  g_now = millis();
}

void callback(char* topic, byte* payload, unsigned int length) {
 String strTopic = String((char*)topic);

 if(strcmp(topic, "toGarage/door/status") == 0)
 {
  doorStatus = 99;
  checkDoor();
 }

 if(strcmp(topic, "toGarage/door/open") == 0)
 {
  openDoor(1);
 }
}
/*
void callback(char* topic, byte* payload, unsigned int length) {
#ifdef RX_DEBUG
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
#endif

  readCard[0]=0xDE;
  readCard[1]=0xAD;
  readCard[2]=0xBE;
  readCard[3]=0xEF;
  readCard[4]=0x00;

  switch((char)payload[0]) {
    case '0':
    case '1':
      openDoor(1);
      break;
    default:
      ;
  }
}
*/

void reconnect() {
  // Loop until we're reconnected
  if(!client.connected()) {
#ifdef RX_DEBUG
    Serial.print("Attempting another MQTT connection...");
#endif
    // Attempt to connect
    if (client.connect("aESP8266Client")) {
      digitalWrite(passPin, HIGH); // Turn on green LED
      Serial.println("connected");
      // Once connected, publish an announcement...
      client.publish("fromGarage", "ready");
      // ... and resubscribe
      client.subscribe("toGarage/#");
      delay(250);
      digitalWrite(passPin, LOW); // Turn off green LED
    } else {
#ifdef RX_DEBUG
      digitalWrite(failPin, HIGH); // Turn on red LED
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(500);
      digitalWrite(failPin, LOW); // Turn off red LED
#endif
    }
  }
}

void rfidloop();

void sendMsg(const char *m)
{
  if (client.connected()) {
#ifdef RX_DEBUG
    Serial.print("Publish message: ");
    Serial.println(m);
#endif
    client.publish("fromGarage", m);
  }
}

void sendMsg(const char *topic, const char *m)
{
  if (client.connected()) {
#ifdef RX_DEBUG
    Serial.print("Publish message: ");
    Serial.println(m);
#endif
    snprintf (msg, MSG_LEN, "fromGarage/%s", topic);
    client.publish(msg, m);
  }
}

void checkDoor() {
  int b = digitalRead(switchPin);
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
  ArduinoOTA.handle();

  if( rfid.available() )
    rfidloop();

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

  if(programMode) // Program mode to add a new ID card
  {
    programModeOn();
  }
  else if(deleteMode) // Delete mode to delete an added ID card
  {
    deleteModeOn();
  }

  checkDoor();

}

// Ascii -> Hex conversion
byte Asc2Hex(byte val)
{
  byte hex = 0;
  if ( (val >= '0' ) && ( val <= '9' ) )
  {
    hex = val - '0';
  }
  else if ( ( val >= 'a' ) && ( val <= 'f' ) )
  {
    hex = 10 + val - 'a';
  }
  else if ( ( val >= 'A' ) && ( val <= 'F' ) )
  {
    hex = 10 + val - 'A';
  }
  return hex;
}

void sendKey (const char *m)
{
  int n = 0;
  n += snprintf (msg+n, MSG_LEN-n, "%s ", m);
  for(int k=0; k<5; k++) {
    n += snprintf (msg+n, MSG_LEN-n, "%02X", readCard[k]);
  }
  sendMsg(msg);
}

// If the serial port is ready and we received the STX BYTE (2) then this function is called
// to get the 4 BYTE ID + 1 BYTE checksum. The ID+checksum is stored in readCard[6]
// Bytes 0-4 are the 5 ID bytes, byte 5 is the checksum
void getID(byte *card)
{
  byte length;
  rfid.getData(card,length);
}

// Read an ID from EEPROM and save it to the storedCard[6] array
void readID( int number ) // Number = position in EEPROM to get the 5 Bytes from
{
  int start = (number * 5 ) - 4; // Figure out starting position
#ifdef DEBUG
  snprintf (msg, MSG_LEN, "Start: %d\n\n", start);
  Serial.print(msg);
#endif//DEBUG
  for ( int i = 0; i < 5; i++ ) // Loop 5 times to get the 5 Bytes
  {
    storedCard[i] = EEPROM.read(start + i); // Assign values read from EEPROM to array
    /*
      Serial.print("Read [");
      Serial.print(start+i);
      Serial.print("] [");
      Serial.print(storedCard[i], HEX);
      Serial.print("] \n");
    */
  }
}

// Write an array to the EEPROM in the next available slot
void writeID( byte a[] )
{
  if ( !findID( a ) ) // Before we write to the EEPROM, check to see if we have seen this card before!
  {
    int num = EEPROM.read(0); // Get the numer of used spaces, position 0 stores the number of ID cards
#ifdef DEBUG
    snprintf (msg, MSG_LEN, "Num: %d\n", num);
    Serial.print(msg);
#endif//DEBUG
    int start = ( num * 5 ) + 1; // Figure out where the next slot starts
    num++; // Increment the counter by one
    EEPROM.write( 0, num ); // Write the new count to the counter
    for ( int j = 0; j < 5; j++ ) // Loop 5 times
    {
      EEPROM.write( start + j, a[j] ); // Write the array values to EEPROM in the right position
      /*
        Serial.print("W[");
        Serial.print(start+j);
        Serial.print("] Value [");
        Serial.print(a[j], HEX);
        Serial.print("] \n");
      */
    }
    EEPROM.commit();
    successWrite();
  }
  else
  {
    failedWrite();
  }
}

// Delete an array stored in EEPROM from the designated slot
void deleteID( byte a[] )
{
  if ( !findID( a ) ) // Before we delete from the EEPROM, check to see if we have this card!
  {
    failedWrite(); // If not
  }
  else
  {
    int num = EEPROM.read(0); // Get the numer of used spaces, position 0 stores the number of ID cards
    int slot; // Figure out the slot number of the card
    int start;// = ( num * 5 ) + 1; // Figure out where the next slot starts
    int looping; // The number of times the loop repeats
    int j;

    int count = EEPROM.read(0); // Read the first Byte of EEPROM that
#ifdef DEBUG
    snprintf (msg, MSG_LEN, "Count: %d\n\n", count); // stores the number of ID's in EEPROM
    Serial.print(msg);
#endif//DEBUG
    slot = findIDSLOT( a ); //Figure out the slot number of the card to delete
    start = (slot * 5) - 4;
    looping = ((num - slot) * 5);
    num--; // Decrement the counter by one
    EEPROM.write( 0, num ); // Write the new count to the counter

    for ( j = 0; j < looping; j++ ) // Loop the card shift times
    {
      EEPROM.write( start + j, EEPROM.read(start + 5 + j)); // Shift the array values to 5 places earlier in the EEPROM
      /*
        Serial.print("W[");
        Serial.print(start+j);
        Serial.print("] Value [");
        Serial.print(a[j], HEX);
        Serial.print("] \n");
      */
    }
    for ( int k = 0; k < 5; k++ ) //Shifting loop
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
  int count = EEPROM.read(0); // Read the first Byte of EEPROM that
#ifdef DEBUG
  snprintf (msg, MSG_LEN, "Count: %d\n\n", count); // stores the number of ID's in EEPROM
  Serial.print(msg);
#endif//DEBUG
  for ( int i = 1; i <= count; i++ ) // Loop once for each EEPROM entry
  {
    readID(i); // Read an ID from EEPROM, it is stored in storedCard[6]
#ifdef DEBUG
    Serial.print("Card ");
    Serial.print(i);
    Serial.print(": ");
#endif
    printKey(storedCard);
  }
}

// Find the slot number of the id to be deleted
int findIDSLOT( byte find[] )
{
  int count = EEPROM.read(0); // Read the first Byte of EEPROM that
#ifdef DEBUG
  snprintf (msg, MSG_LEN, "Count: %d\n\n", count); // stores the number of ID's in EEPROM
  Serial.print(msg);
#endif//DEBUG
  for ( int i = 1; i <= count; i++ ) // Loop once for each EEPROM entry
  {
    readID(i); // Read an ID from EEPROM, it is stored in storedCard[6]
    if ( checkTwo( find, storedCard ) ) // Check to see if the storedCard read from EEPROM
    { // is the same as the find[] ID card passed
      Serial.print("FindIDSLOT: We have a matched card!!! \n");
      return i; // The slot number of the card
      break; // Stop looking we found it
    }
  }
}

void printKey ( byte a[] )
{
  Serial.print("[");
  for ( int k = 0; k < 5; k++ ) { // Loop 5 times
    snprintf(msg, 20, "%02X", a[k]);
    Serial.print(msg);
  }
  Serial.println("]");
}

void printKeys ( byte a[], byte b[] )
{
#ifdef DEBUG
  Serial.print("[");
  for ( int k = 0; k < 5; k++ ) // Loop 5 times
    Serial.print(a[k], HEX);
  Serial.print("] -- [");
  for ( int k = 0; k < 5; k++ ) // Loop 5 times
    Serial.print(b[k], HEX);
  Serial.println("]");
#endif
}

// Check two arrays of bytes to see if they are exact matches
boolean checkTwo ( byte a[], byte b[] )
{
  if ( a[0] == 0 ) // Make sure there is something in the array first
    return false;

  printKeys(a, b);

  boolean match = true; // Assume they match at first
  for ( int k = 0; match && k < 5; k++ ) // Loop 5 times
  {
    if ( a[k] != b[k] ) // IF a != b then set match = false, one fails, all fail
      match = false;
  }
  return match;
}

// Looks in the EEPROM to try to match any of the EEPROM ID's with the passed ID
boolean findID( byte find[] )
{
  int count = EEPROM.read(0); // Read the first Byte of EEPROM that
#ifdef DEBUG
  snprintf (msg, MSG_LEN, "Count: %d\n\n", count); // stores the number of ID's in EEPROM
  Serial.print(msg);
#endif//DEBUG
  for ( int i = 1; i <= count; i++ ) // Loop once for each EEPROM entry
  {
    readID(i); // Read an ID from EEPROM, it is stored in storedCard[6]
    if ( checkTwo( find, storedCard ) ) // Check to see if the storedCard read from EEPROM
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
  sendKey("key");
  setDelay *= 1000; // Sets delay in seconds
  digitalWrite(failPin, LOW); // Turn off red LED
  digitalWrite(passPin, HIGH); // Turn on green LED
  digitalWrite(doorPin, HIGH); // Unlock door!

  delay(setDelay); // Hold door lock open for some seconds

  digitalWrite(doorPin, LOW); // Relock door

  delay(setDelay); // Hold green LED on for some more seconds

  digitalWrite(passPin, LOW); // Turn off green LED
  rfid.flush();
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
  rfid.flush();
}

// Check to see if the ID passed is the master programing card
boolean isMaster( byte test[] )
{
  byte val[6] = {0x03, 0x00, 0x13, 0x03, 0xF5, 0x00 };
  return checkTwo(test, val);
}

// Check to see if the ID passed is the wipe memory card
boolean isWipe( byte test[] )
{
  byte val[6] = {0x03, 0x00, 0x74, 0x91, 0x48, 0x00 };
  return checkTwo(test, val);
}

// Check to see if the ID passed is the deletion card
boolean isDelete( byte test[] )
{
  byte val[6] = {0x03, 0x00, 0x13, 0x03, 0xE2, 0x00 };
  return checkTwo(test, val);
}

// Controls LED's for Normal mode, Blue on, all others off
void normalModeOn()
{
  digitalWrite(passPin, LOW); // Make sure Green LED is off
  digitalWrite(failPin, LOW); // Make sure Red LED is off
  digitalWrite(doorPin, LOW); // Make sure Door is Locked
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

// Flashes the green LED 3 times to indicate a successful write to EEPROM
void successWrite()
{
#ifdef RX_DEBUG
  Serial.println("Write OK!");
#endif
  digitalWrite(failPin, LOW); // Make sure red LED is off
  digitalWrite(passPin, LOW); // Make sure green LED is on
  delay(200);
  digitalWrite(passPin, HIGH); // Make sure green LED is on
  delay(200);
  digitalWrite(passPin, LOW); // Make sure green LED is off
  delay(200);
  digitalWrite(passPin, HIGH); // Make sure green LED is on
  delay(200);
  digitalWrite(passPin, LOW); // Make sure green LED is off
  delay(200);
  digitalWrite(passPin, HIGH); // Make sure green LED is on
  delay(200);
  rfid.flush();
}

// Flashes the red LED 3 times to indicate a failed write to EEPROM
void failedWrite()
{
#ifdef RX_DEBUG
  Serial.println("Failed write!");
#endif
  digitalWrite(failPin, LOW); // Make sure red LED is on
  digitalWrite(passPin, LOW); // Make sure green LED is off
  delay(200);
  digitalWrite(failPin, HIGH); // Make sure red LED is on
  delay(200);
  digitalWrite(failPin, LOW); // Make sure red LED is off
  delay(200);
  digitalWrite(failPin, HIGH); // Make sure red LED is on
  delay(200);
  digitalWrite(failPin, LOW); // Make sure red LED is off
  delay(200);
  digitalWrite(failPin, HIGH); // Make sure red LED is on
  delay(200);
  rfid.flush();
}

// Flashes the red + green LED 3 times to indicate a success delete to EEPROM
void successDelete()
{
#ifdef RX_DEBUG
  Serial.println("Delete ok!");
#endif
  digitalWrite(failPin, LOW); // Make sure red LED is off
  digitalWrite(passPin, LOW); // Make sure green LED is on
  delay(200);
  digitalWrite(failPin, HIGH); // Make sure red LED is on
  delay(200);
  digitalWrite(failPin, LOW); // Make sure red LED is off
  delay(200);
  digitalWrite(failPin, HIGH); // Make sure red LED is on
  delay(200);
  digitalWrite(failPin, LOW); // Make sure red LED is off
  delay(200);
  digitalWrite(failPin, HIGH); // Make sure red LED is on
  delay(200);
  digitalWrite(failPin, LOW); // Make sure red LED is off
  rfid.flush();
}

// Controls LED's for wipe mode, cycles through BG
void wipeModeOn()
{
#ifdef RX_DEBUG
  Serial.println("Wipe mode on!");
#endif
  digitalWrite(failPin, LOW); // Make sure red LED is off
  digitalWrite(passPin, LOW); // Make sure green LED is off
  delay(50);
  digitalWrite(passPin, LOW); // Make sure green LED is off
  delay(200);
  digitalWrite(passPin, HIGH); // Make sure green LED is on
  delay(200);
  digitalWrite(passPin, LOW); // Make sure green LED is off
  delay(200);
  digitalWrite(passPin, HIGH); // Make sure green LED is on
  delay(200);
  digitalWrite(passPin, LOW); // Make sure green LED is on
  delay(200);
  digitalWrite(failPin, HIGH); // Make sure green LED is on
}


void rfidloop ()
{
  byte val = 0; // Temp variable to hold the current byte
  getID(readCard); // Get the ID, sets readCard = to the read ID
  Serial.print("Got a card: ");
  printKey(readCard);
  if ( programMode) // Program mode to add a new ID card
  {
    // Check to see if it is the master programing card or delete card
    if ( isMaster(readCard) || isDelete(readCard) || isWipe(readCard))
    {
      return; // Ignore! failedWrite();
    }
    else
    {
      writeID(readCard); // If not, write the card to the EEPROM storage
      sendKey("add");
    }
    programMode = 0;
    normalModeOn(); // Normal mode, blue Power LED is on, all others are off
    rfid.flush();
  }
  else if ( deleteMode ) // Delete mode to delete an added ID card
  {
    if ( isMaster(readCard) || isDelete(readCard) || isWipe(readCard) ) // Check to see if it is the master programing card or the Delete Card
    {
      return; // Ignore! failedWrite();
    }
    else //if ( !isMaster(readCard) && !isDelete(readCard) )
    {
      deleteID(readCard); // If not, delete the card from the EEPROM sotrage
      sendKey("del");
    }
    deleteMode = 0;
    normalModeOn(); // Normal mode, blue Power LED is on, all others are off
    rfid.flush();
  }
  else if ( wipeMode ) // Wipe mode to wipe out the EEPROM
  {
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
      wipeMode = true; // If so, enable deletion mode
      alarm = 0;
      rfid.flush();
      wipeModeOn();
      sendMsg("wipe");
      for (int i = 0; i < EEROM_SIZE; i++) // Loop repeats equal to the number of array in EEPROM
      {
        EEPROM.write(i, 0);
      }
      EEPROM.commit();
      wipeMode = false;
      normalModeOn(); // Normal mode, blue Power LED is on, all others are off
      rfid.flush();
    }
    else
    {
      if ( findID(readCard) ) // If not, see if the card is in the EEPROM
      {
        Serial.println("Valid card!");
        openDoor(1); // If it is, open the door lock
        alarm = 0;
      }
      else
      {
        Serial.println("Unknown card!");
        sendKey("fail");
        failed(); // If not, show that the ID was not valid
        alarm++;
        delay(1000);
      }
    }
    rfid.flush();
  }
}
