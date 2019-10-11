
#define RX_DEBUG 1
//#define USE_INTERRUPT

#define DOOR1_ALIAS                 "North door"
#define MQTT_DOOR1_ACTION_TOPIC     "garage/door/1/action"
#define MQTT_DOOR1_STATUS_TOPIC     "garage/door/1/status"
#define DOOR1_OPEN_PIN              D2
#define DOOR1_CLOSE_PIN             D2
#define DOOR1_STATUS_PIN            D7
// The type of reed/magnetic switch used for the Door.
// Must be placed within quotation marks.
// Set to "NO for normally-open. Set to "NC" for normally-closed. (Default: NO)
#define DOOR1_STATUS_SWITCH_LOGIC   "NO"

#define failPin                     D5 // 14 // Red LED
#define passPin                     D6 // 12 // Green LED
//#define relayPin                    D2 //  4 // Relay
#define rfidPinRx                   D1 //  5 // RDM630 Reader
#define rfidPinTx                   D3 //  0 // RDM630 Reader
//#define sensorPin                   D7 // 13 // Door switch
#ifdef USE_INTERRUPT
#define cardInt                     D8 // 15 // RDM630 interrupt
#endif

#define CARD_LEN                    5

#define BLINK_DLY                   200
#define TIMEOUT_DLY                 5000
