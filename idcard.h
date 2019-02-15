#ifndef __IDCARD_H__
#define __IDCARD_H__

typedef struct {
  uint32_t  hash;
  uint8_t   num;
} EHead_t;

enum Card_enum {OTHER, MASTER_CARD, DELETE_CARD, WIPE_CARD};

extern RFIDtag storedCard;  // Stores an ID read from EEPROM
extern RFIDtag readCard;    // Sotres an ID read from the RFID reader

uint8_t whichCard( RFIDtag test );

void printTag( RFIDtag tag );
void printKey( RFIDtag tag );
void printKeys ( RFIDtag a, RFIDtag b );
void printCard(char *lbl, int pos, RFIDtag card);

bool initEEPROM(void);
void eraseEEPROM(void);

// Read an ID given slot number from EEPROM and save it to the storedCard variable
void readID( int number );
// List all cards found in the EEPROM ...
int listID(void);
// Find the slot number of the id to be deleted
int findID( RFIDtag find );

// Write a new ID to the EEPROM in the next available slot
bool writeID( RFIDtag newCard );
// Delete a card stored in EEPROM from the designated slot
bool deleteID( RFIDtag a );

#endif//__IDCARD_H__
