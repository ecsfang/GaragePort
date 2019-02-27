#include <EEPROM.h> // Needed to write to EEPROM storage
#include <RFIDRdm630.h>
#include "idcard.h"

RFIDtag storedCard;  // Stores an ID read from EEPROM
RFIDtag readCard;    // Sotres an ID read from the RFID reader

// Get address in EEPROM given slot number (0-n)
#define GET_ROM_ADDRESS(n)  (((n)*sizeof(RFIDtag)) + sizeof(EHead_t))

#define HASH_TAG  0x5117BEEF
#define EEROM_SIZE  512

RFIDtag masterCard("03001303F5E6");
RFIDtag wipeCard("0300749148AE");
RFIDtag deleteCard("03001303E2F1");

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

uint8_t whichCard( RFIDtag test )
{
  if( isMaster(test) ) return MASTER_CARD;
  if( isDelete(test) ) return DELETE_CARD;
  if( isWipe(test) ) return WIPE_CARD;
  return OTHER;
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
  printTag(a);
  Serial.print(" -- ");
  printTag(b);
  Serial.println();
}

void printCard(char *lbl, int pos, RFIDtag card)
{
  Serial.print(lbl);
  Serial.print(" [");
  Serial.print(pos);
#ifdef RX_DEBUG
  Serial.print("@");
  Serial.print(GET_ROM_ADDRESS(pos));
#endif
  Serial.print("] ");
  printKey(card);
}

bool initEEPROM(void)
{
  EEPROM.begin(EEROM_SIZE);

  EHead_t eHead;
  EEPROM.get(0, eHead);

#ifdef RX_DEBUG
  Serial.println("\n\nEEPROM");
  Serial.print("Head: ");
  Serial.println(eHead.hash, HEX);
  Serial.print("Count: ");
  Serial.println(eHead.num);
#endif

  if( eHead.hash != HASH_TAG ) {
    // EEPROM in bad shape ... reinitialize ... !
#ifdef RX_DEBUG
    Serial.println("Reinitialize EEPROM ...");
#endif
    eraseEEPROM();
    return false;
  }
  return true;
}

void eraseEEPROM(void)
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


// Read an ID given slot number from EEPROM and save it to the storedCard variable
void readID( int number )
{
  int start = GET_ROM_ADDRESS(number); // Figure out starting position
  EEPROM.get(start, storedCard);
  printCard("Read", number, storedCard);
}

// List all cards found in the EEPROM ...
int listID(void)
{
  EHead_t eHead;
  EEPROM.get(0, eHead); // Read the first Byte of EEPROM that
#ifdef RX_DEBUG
  if( eHead.num )
    snprintf (msg, MSG_LEN, "Found %d cards in EEPROM\n\n", eHead.num, eHead.num > 1 ? "cards" : "card");
  else
    snprintf (msg, MSG_LEN, "No cards found in EEPROM!!\n\n");
  Serial.print(msg);
#endif//RX_DEBUG
  for ( int i = 0; i < eHead.num; i++ ) // Loop once for each EEPROM entry
  {
    readID(i); // Read an ID from EEPROM, it is stored in storedCard[6]
#ifdef RX_DEBUG
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
#ifdef RX_DEBUG
  snprintf (msg, MSG_LEN, "Count: %d\n\n", eHead.num); // stores the number of ID's in EEPROM
  Serial.print(msg);
#endif//RX_DEBUG
  for ( int i = 1; i <= eHead.num; i++ ) // Loop once for each EEPROM entry
  {
    readID(i); // Read an ID from EEPROM, it is stored in storedCard[6]
    if ( find == storedCard ) // Check to see if the storedCard read from EEPROM
    { // is the same as the find[] ID card passed
      Serial.print("FindID: We have a matched card!!! \n");
      return i; // The slot number of the card
    }
  }
  // Card not found ...
  return -1;
}

// Write a new ID to the EEPROM in the next available slot
bool writeID( RFIDtag newCard )
{
  if ( findID( newCard ) >= 0 ) // Before we write to the EEPROM, check to see if we have seen this card before!
    // ID already exists in EEPROM ...
    return false;

  EHead_t eHead;
  EEPROM.get(0, eHead); // Get the number of used slots ...
  eHead.num++; // Increment the counter by one
  int start = GET_ROM_ADDRESS(eHead.num); // Figure out where the next slot starts
  EEPROM.put(start, newCard);
  EEPROM.put( 0, eHead ); // Write the new count to the counter
  printCard("Write", eHead.num, newCard);
  EEPROM.commit();
  return true;
}

// Delete a card stored in EEPROM from the designated slot
bool deleteID( RFIDtag a )
{
  int slot = findID( a ); // Figure out the slot number of the card to delete

  if ( slot < 0 ) // Before we delete from the EEPROM, check to see if we have this card!
  {
#ifdef RX_DEBUG
    snprintf (msg, MSG_LEN, "Can't find '%s'!\n\n", a.getTag()); // stores the number of ID's in EEPROM
    Serial.print(msg);
#endif//RX_DEBUG
    return false; // ID not found in EEPROM ...
  }
  else
  {
    EHead_t eHead;
    EEPROM.get(0, eHead); // Get the numer of used spaces, position 0 stores the number of ID cards

#ifdef RX_DEBUG
    snprintf (msg, MSG_LEN, "Count: %d\n\n", eHead.num); // stores the number of ID's in EEPROM
    Serial.print(msg);
#endif//RX_DEBUG

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
  }
  return true;
}
