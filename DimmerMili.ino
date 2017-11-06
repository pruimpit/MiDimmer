// Revision 1.00  
// Arduino 1.6.13

#include "Arduino.h"
#include "PinChangeInt.h"           // from: http://code.google.com/p/arduino-pinchangeint/
#include <avr/sleep.h>
#include "LowPower.h"		            // from: https://github.com/rocketscream/Low-Power
#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>                   // from: https://github.com/TMRh20/RF24
#include "PL1167_nRF24.h"           // from:  https://github.com/henryk/openmili        
#include "MiLightRadio.h"
#include <printf.h>

/*
 Brown-out must be disabled in bootloader.
 Change in boards.txt:  
 change: pro.menu.cpu.8MHzatmega328.bootloader.extended_fuses=0x05
 in: pro.menu.cpu.8MHzatmega328.bootloader.extended_fuses=0xFF
 reprogram bootloader. You can use an UNO board for reprogramming the Mini Pro

 Program Examples=> ArduinoISP in the UNO
 Connect pins:
 
 UNO    Mini PRO
 ===============
 5V     VCC
 GND    GND
 10     RESET
 11     11
 12     12
 13     13

Tools => Burn Bootloader
 
*/


/* Milight protocol (RGBW)
B0 F2 EA 6D B0 02 f0"
|        |  |  |  |
|        |  |  |  sequence number
|        |  |  Button_of_Remote
|        |  brightness
|        color
ID_of_Remote (3 byte)

Buttons:
-----------
1 All On
2 All Off
3 Group 1 On
4 Group 1 Off
5 Group 2 On
6 Group 2 Off
7 Group 3 On
8 Group 3 Off
9 Group 4 On
A Group 4 Off
B Speed + 
C Speed -
D Multicolor (Disco)
E Brightness
F Color


https://hackaday.io/project/5888-reverse-engineering-the-milight-on-air-protocol

Dimming: 

Group
1:      91...A9
2:      92...AA
3:      93...AB
4:      94...AC



*/

#define GROUP   3

#define DEBUG 0
#define WAIT_TIME_TRANSMITTER   5
#define WAIT_TIME_RFLINK        2
#define WAIT_TIME_NEXT          2
#define LOOP_TIME               10
#define LONGPRESSTIME           100000   // in loops. appr 10 usec
#define REPEAT_DELAY            4    // ms

#define ENCODER_PIN_A           2
#define ENCODER_PIN_B           5
#define ENCODER_PRESS_PIN       3
#define TX_POWER_PIN            8
#define CE_PIN                  9
#define CSN_PIN                 10
#define TX_RETRANSMIT           2
#define LED_PIN                 13

#define MESSAGE_ON_REPEATS       5
#define MESSAGE_REPEATS         12

#if  (GROUP == 1)
  #define ON_COMMAND           0x03
  #define OFF_COMMAND          0x04
  #define WHITE_ON_COMMAND     0x13
#elif (GROUP == 2)
  #define ON_COMMAND           0x05
  #define OFF_COMMAND          0x06
  #define WHITE_ON_COMMAND     0x15
#elif (GROUP == 3)
  #define ON_COMMAND           0x07
  #define OFF_COMMAND          0x08
  #define WHITE_ON_COMMAND     0x17
#elif (GROUP == 4)
  #define ON_COMMAND           0x09
  #define OFF_COMMAND          0x0A
  #define WHITE_ON_COMMAND     0x19
#endif

#define BRIGHTNESS_COMMAND     0x0E
#define COLOR_COMMAND          0x0F
#define ZERO_COMMAND           0x00

  

/* Brighness  Goes from 0x90 (min) at the leftmost end, down(!) to 0x00 in the middle, 
to 0xA8 (max) on the rightmost end, in increments of 0x08 */
#define MIN_BRIGHTNESS           0
#define MAX_BRIGHTNESS           28

#define MODUS_BRIGHTNESS         0
#define MODUS_COLOR              1


volatile int  EncoderTicksLeft=0;
volatile int  EncoderTicksRight=0;
volatile int  EncoderPressed=0;
volatile int  EncoderLongPressed=0;
uint8_t Brightness = 0;
uint8_t OldBrightness = 0;
int repeatcounter = WAIT_TIME_TRANSMITTER;
static uint8_t outgoingPacket[7];
uint8_t packetcounter = 0;
uint8_t MiliBrightness = 0x00;       // MiLight brightness
uint8_t Color = 0;                   // MiLight color
uint8_t OldColor = 0;                // MiLight color
uint8_t Modus = MODUS_BRIGHTNESS;    



RF24 radio(CE_PIN, CSN_PIN);
PL1167_nRF24 prf(radio);
MiLightRadio mlr(prf);


void setup() {
  if (DEBUG == 1) {
    Serial.begin(9600);
    printf_begin();
    Serial.write("Starting serial debug\n");
    unsigned int resetvalue = MCUSR;
    MCUSR = 0;
    if (resetvalue & 0x01)
      Serial.write("Power-on Reset\n");
    if (resetvalue & 0x02)
      Serial.write("External Reset\n");
    if (resetvalue & 0x04)
      Serial.write("Brownout Reset\n");
    if (resetvalue & 0x08)
      Serial.write("Watchdog Reset\n");
  }
  
  mlr.begin();
  
  // Fill in your remote control address 
  outgoingPacket[0] = 0xB0;
  outgoingPacket[1] = 0x3E;
  outgoingPacket[2] = 0xAC;
  

  pinMode(ENCODER_PIN_A, INPUT_PULLUP);        // Sets Encoder pin A as input
  pinMode(ENCODER_PIN_B, INPUT_PULLUP);        // Sets Encoder pin  B as input
  pinMode(ENCODER_PRESS_PIN, INPUT_PULLUP);    // Sets Encoder press pin as input
  pinMode(TX_POWER_PIN, OUTPUT);               // Sets Transmitter power pin as output
  pinMode(LED_PIN, OUTPUT);                    // Sets LED pin as output
  digitalWrite(TX_POWER_PIN, LOW);             // Do not power Transmitter
  digitalWrite(LED_PIN, LOW);                  // LED Off
  attachInterrupt(digitalPinToInterrupt(ENCODER_PIN_A), ExtInterrupt, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_PRESS_PIN), PressInterrupt, FALLING);
  PCintPort::attachInterrupt(ENCODER_PIN_B, &PinChangeInterrupt, CHANGE);
  
}


void loop() {
  delay(LOOP_TIME);
  if (DEBUG == 1) {
    Serial.print(".");
  }
  check_pressed();
  check_encoder();  
  goto_sleep();
}




/*************************************/
/*     Check encoder pressed         */
/*************************************/
void check_pressed(){
  if (EncoderPressed > 0) {
    if (DEBUG) {
       Serial.println("Sending light off");
    }
    digitalWrite(TX_POWER_PIN, HIGH);        // Power up transmitter
    digitalWrite(LED_PIN, HIGH);             // LED On
    delay(WAIT_TIME_TRANSMITTER);
    send_off(MESSAGE_REPEATS);
    Brightness = MIN_BRIGHTNESS;
    digitalWrite(TX_POWER_PIN, LOW);          // Power off transmitter
    digitalWrite(LED_PIN, LOW);               // LED Off
    EncoderPressed = 0;
    delay(WAIT_TIME_NEXT);
    EncoderTicksRight = 0;
    EncoderTicksLeft = 0;
    Modus = MODUS_BRIGHTNESS;
  }
  else if (EncoderLongPressed > 0) {
    digitalWrite(TX_POWER_PIN, HIGH);        // Power up transmitter
    digitalWrite(LED_PIN, HIGH);             // LED On
    delay(WAIT_TIME_TRANSMITTER);
    
    if (Modus == MODUS_BRIGHTNESS) {
      Modus = MODUS_COLOR;
      if (DEBUG) {
        Serial.println("Modus: Color");
      }
      Brightness = MAX_BRIGHTNESS;
      convert_brightness();
      send_color(MESSAGE_REPEATS);
      send_brightness(MESSAGE_REPEATS);
    }
    else {
      Modus = MODUS_BRIGHTNESS;
      if (DEBUG) {
        Serial.println("Modus: Brightness");
      }
      convert_brightness();
      send_white_on(MESSAGE_REPEATS);
      send_brightness(MESSAGE_REPEATS);
    }
    digitalWrite(TX_POWER_PIN, LOW);          // Power off transmitter
    digitalWrite(LED_PIN, LOW);               // LED Off
    EncoderLongPressed = 0;
    delay(WAIT_TIME_NEXT);
    EncoderTicksRight = 0;
    EncoderTicksLeft = 0;
  }
}


/*************************************/
/*     Check encoder rotation        */
/*************************************/
void check_encoder() { 
    // increase light
  while (EncoderTicksRight > 0) {
    if (Modus == MODUS_BRIGHTNESS){
      if (Brightness < MAX_BRIGHTNESS) {
        Brightness += 1;
      }
      else {
        OldBrightness -=1;                    // send command again
      }
    }
    else {
      Color++;
    }
    EncoderTicksRight -= 1;
  }
  while (EncoderTicksLeft > 0) {
    if (Modus == MODUS_BRIGHTNESS){
      if (Brightness > MIN_BRIGHTNESS) {
         Brightness -= 1;
      }
      else {
        OldBrightness +=1;                   // send command again
      }
    }
    else {
      Color--;
    }
    EncoderTicksLeft -= 1;
  }
  
  if (Brightness != OldBrightness) {
    if (OldBrightness == MIN_BRIGHTNESS) {
      digitalWrite(TX_POWER_PIN, HIGH);       // Power up transmitter
      digitalWrite(LED_PIN, HIGH);            // LED On
      delay(WAIT_TIME_TRANSMITTER);
      Modus = MODUS_BRIGHTNESS;
      send_on(MESSAGE_REPEATS);
      send_zero();
      send_white_on(MESSAGE_REPEATS);
      send_zero();
      send_brightness(MESSAGE_REPEATS);
      if (DEBUG == 1) {
        Serial.print("Sending White and Dim: ");
        Serial.print(Brightness);
        Serial.print("\n");
        printf("Mili: %02x", MiliBrightness);
      }
      send_zero();
    }
    else if (Brightness > MIN_BRIGHTNESS ) {
      digitalWrite(TX_POWER_PIN, HIGH);       // Power up transmitter
      digitalWrite(LED_PIN, HIGH);            // LED On
      delay(WAIT_TIME_TRANSMITTER);
      if (Brightness == MAX_BRIGHTNESS) {
        send_on(MESSAGE_REPEATS);
        send_zero();  
      }
      send_brightness(MESSAGE_REPEATS);
      if (DEBUG == 1) {
        Serial.print("Sending Dim: ");
        Serial.print(Brightness);
        Serial.print("\n");
        printf("Mili: %02x", MiliBrightness);
      }
      send_zero();
    }
    else {
      if (DEBUG == 1) {
        Serial.println("Sending light off");
      }
      digitalWrite(TX_POWER_PIN, HIGH);        // Power up transmitter
      digitalWrite(LED_PIN, HIGH);             // LED On
      delay(WAIT_TIME_TRANSMITTER);
      convert_brightness();
      send_off(MESSAGE_REPEATS);
      send_zero();
    }
    OldBrightness = Brightness;
    digitalWrite(TX_POWER_PIN, LOW);          // Power off transmitter
    digitalWrite(LED_PIN, LOW);               // LED Off
    delay(WAIT_TIME_NEXT);                    // wait before sending new value.
  }
  else if (Color != OldColor){
    digitalWrite(TX_POWER_PIN, HIGH);       // Power up transmitter
    digitalWrite(LED_PIN, HIGH);            // LED On
    delay(WAIT_TIME_TRANSMITTER);  
    send_color(MESSAGE_REPEATS);
    OldColor = Color;
    digitalWrite(TX_POWER_PIN, LOW);          // Power off transmitter
    digitalWrite(LED_PIN, LOW);               // LED Off
    delay(WAIT_TIME_NEXT);                    // wait before sending new value.
    if (DEBUG == 1) {
      Serial.println("Sending Color");
    }
    send_zero();
  }
}




/******************************************/
/* Go to sleep when nothing is happening  */
/******************************************/
void goto_sleep() { 
 if ( (OldBrightness == Brightness) &&
      (EncoderPressed == 0)&&
      (EncoderTicksRight == 0)&&
      (EncoderTicksLeft == 0) ) {
    digitalWrite(TX_POWER_PIN, LOW);             // Power off transmitter
    digitalWrite(LED_PIN, LOW);       
    radio.powerDown();
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    cli();
    sleep_enable();
    sei();
    
    LowPower.powerDown(SLEEP_FOREVER, ADC_OFF, BOD_OFF);
     sleep_cpu();

    // ******  SLEEP *************
    
    /* wake up here */
    sleep_disable();
    radio.powerUp();
  }
}




void send_on(uint8_t repeats){
  int i;
  convert_brightness();
  outgoingPacket[3] = Color;
  outgoingPacket[4] = MiliBrightness;
  outgoingPacket[5] = ON_COMMAND;
  for (i=1 ; i <= repeats ; i++) {
    outgoingPacket[6] = ++packetcounter;
    mlr.write(outgoingPacket, sizeof(outgoingPacket));
    if (i != repeats) {
      delay(REPEAT_DELAY);
    }
  }
}


void send_white_on(uint8_t repeats){
  int i;
  convert_brightness();
  outgoingPacket[3] = Color;
  outgoingPacket[4] = MiliBrightness;
  outgoingPacket[5] = WHITE_ON_COMMAND;
  for (i=1 ; i <= repeats ; i++) {
    outgoingPacket[6] = ++packetcounter;
    mlr.write(outgoingPacket, sizeof(outgoingPacket));
    if (i != repeats) {
      delay(REPEAT_DELAY);
      if (DEBUG == 1) {
        printf("Packet: %02X %02X %02X %02X %02X %02X %02X\n", outgoingPacket[0], outgoingPacket[1], outgoingPacket[2], outgoingPacket[3],  outgoingPacket[4],  outgoingPacket[5],  outgoingPacket[6]);  
      }
    }
  }
}

void send_off(uint8_t repeats){
  int i;
  convert_brightness();
  outgoingPacket[3] = Color;
  outgoingPacket[4] = MiliBrightness;
  outgoingPacket[5] = OFF_COMMAND;
  for (i=1 ; i <= repeats ; i++) {
    outgoingPacket[6] = ++packetcounter;
    mlr.write(outgoingPacket, sizeof(outgoingPacket));
    if (i != repeats) {
      delay(REPEAT_DELAY);
    }
  }
}


void send_color(uint8_t repeats){
  int i;
  convert_brightness();
  outgoingPacket[3] = Color;
  outgoingPacket[4] = MiliBrightness;
  outgoingPacket[5] = COLOR_COMMAND;
  for (i=1 ; i <= repeats ; i++) {
    outgoingPacket[6] = ++packetcounter;
    mlr.write(outgoingPacket, sizeof(outgoingPacket));
    if (i != repeats) {
      delay(REPEAT_DELAY);
    }
  }
}

void send_brightness(uint8_t repeats){
  int i;
  convert_brightness();
  outgoingPacket[3] = Color;
  outgoingPacket[4] = MiliBrightness;
  outgoingPacket[5] = BRIGHTNESS_COMMAND;
  
  for (i=1 ; i <= repeats ; i++) {
    outgoingPacket[6] = ++packetcounter;
    mlr.write(outgoingPacket, sizeof(outgoingPacket));
    if (i != repeats) {
      delay(REPEAT_DELAY);
      if (DEBUG == 1) {
        printf("Packet: %02X %02X %02X %02X %02X %02X %02X\n", outgoingPacket[0], outgoingPacket[1], outgoingPacket[2], outgoingPacket[3],  outgoingPacket[4],  outgoingPacket[5],  outgoingPacket[6]);  
      }
    }
  }
  delay(REPEAT_DELAY);
 }


 void send_zero(void){
  int i;
  int repeats = 1;
  outgoingPacket[3] = Color;
  outgoingPacket[4] = MiliBrightness;
  outgoingPacket[5] = ZERO_COMMAND;
  for (i=1 ; i <= repeats ; i++) {
    outgoingPacket[6] = ++packetcounter;
    mlr.write(outgoingPacket, sizeof(outgoingPacket));
    if (i != repeats) {
      delay(REPEAT_DELAY);
      if (DEBUG == 1) {
        printf("Packet: %02X %02X %02X %02X %02X %02X %02X\n", outgoingPacket[0], outgoingPacket[1], outgoingPacket[2], outgoingPacket[3],  outgoingPacket[4],  outgoingPacket[5],  outgoingPacket[6]);  
      }
    }
  }
 }

/* Convert percentage to Milight brightness
 Goes from 0x90 (min) at the leftmost end, down(!) to 0x00 in the middle, 
to 0xA8 (max) on the rightmost end, in increments of 0x08 */
uint8_t convert_brightness(){
   MiliBrightness  = 0x98 + GROUP - (Brightness*8);
   return MiliBrightness;
}


/*************************************/
/*    Encoder interrupt routines     */
/*************************************/
void ExtInterrupt() {
 int state = digitalRead(ENCODER_PIN_B) & 0x01;
  state += (digitalRead(ENCODER_PIN_A) << 1) & 0x02;
  if (state == 3)
    EncoderTicksRight += 1;
}


void PinChangeInterrupt() {
  int state = digitalRead(ENCODER_PIN_B) & 0x01;
  state += (digitalRead(ENCODER_PIN_A) << 1) & 0x02;
  if (state == 3)
    EncoderTicksLeft += 1;
}


void PressInterrupt(){
  delay(100);
  uint32_t PressCounter = 0;
  if (digitalRead(ENCODER_PRESS_PIN) == 0) {
    while ( (digitalRead(ENCODER_PRESS_PIN) == 0) && \
             (PressCounter < LONGPRESSTIME) )
    {
      PressCounter++;
    }
    if (PressCounter < LONGPRESSTIME){
        EncoderPressed += 1;
        if (DEBUG == 1) {
          Serial.println("*P: ");
          Serial.println(PressCounter);
          Serial.println("\n");
          
        }
    }
    else {
      EncoderLongPressed += 1;  
      if (DEBUG == 1) {
          Serial.println("*P++: ");
          Serial.println(PressCounter);
          Serial.println("\n");
      }
    }
    
  }
  
}







