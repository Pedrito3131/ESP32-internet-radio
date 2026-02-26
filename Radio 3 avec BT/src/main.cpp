//#include "Arduino.h"
#include "WiFi.h"
#include "SPI.h"
#include "vs1053_ext.h"
#include <LiquidCrystal_I2C.h>
#include "AiEsp32RotaryEncoder.h"

#include <SPI.h>
//#include <VS1053.h>
#include "BluetoothA2DPSink.h"
#include <cbuf.h>

unsigned char bt_wav_header[44] = {
    0x52, 0x49, 0x46, 0x46, // RIFF
    0xFF, 0xFF, 0xFF, 0xFF, // size
    0x57, 0x41, 0x56, 0x45, // WAVE
    0x66, 0x6d, 0x74, 0x20, // fmt
    0x10, 0x00, 0x00, 0x00, // subchunk1size
    0x01, 0x00,             // audio format - pcm
    0x02, 0x00,             // numof channels
    0x44, 0xac, 0x00, 0x00, //, samplerate 44k1: 0x44, 0xac, 0x00, 0x00       48k: 48000: 0x80, 0xbb, 0x00, 0x00,
    0x10, 0xb1, 0x02, 0x00, //byterate
    0x04, 0x00,             // blockalign
    0x10, 0x00,             // bits per sample - 16
    0x64, 0x61, 0x74, 0x61, // subchunk3id -"data"
    0xFF, 0xFF, 0xFF, 0xFF  // subchunk3size (endless)
};

uint8_t station_ind = 0;
#define STATION_NB 8
const char * sta_url [STATION_NB] = {
    "icecast.radiofrance.fr/franceinter-hifi.aac",
    "radioclassique.ice.infomaniak.ch/radioclassique-high.mp3",
    "icecast.radiofrance.fr/fip-hifi.aac",
    "icecast.radiofrance.fr/franceculture-hifi.aac",
    "icecast.radiofrance.fr/francemusique-hifi.aac",
    "ouifm.ice.infomaniak.ch/ouifm-high.mp3",
    "radionova.ice.infomaniak.ch/radionova-256.aac",
    "streaming.radio.rtl2.fr/rtl2-1-44-128?listen=webCwsBCggNCQgLDQUGBAcGBg"
};
const char * sta_name [STATION_NB] = {
    "  France Inter",
    "Radio Classique",
    "      FIP",
    " France Culture",
    " France Musique",
    "     Oui FM",
    "   Radio Nova",
    "      RTL2"
};

#define BLUETOOTH_NAME "BT-Lavigne2"

// Digital I/O used for VS1053
#define VS1053_MOSI   23
#define VS1053_MISO   19
#define VS1053_SCK    18
#define VS1053_CS      5
#define VS1053_DCS    16
#define VS1053_DREQ    4

// Digital I/O used for LCD
#define LCD_ADDR 0x27
#define LCD_COLS 16
#define LCD_LN 2

// Digital I/O used for rotary encoder
#define ROTARY_ENCODER_ST_A_PIN 32
#define ROTARY_ENCODER_ST_B_PIN 33
#define ROTARY_ENCODER_VOL_A_PIN 26
#define ROTARY_ENCODER_VOL_B_PIN 27
#define ROTARY_ENCODER_VOL_BUTTON_PIN 25
#define ROTARY_ENCODER_STEPS 4

AiEsp32RotaryEncoder rotaryEncoder_vol = AiEsp32RotaryEncoder(ROTARY_ENCODER_VOL_A_PIN, ROTARY_ENCODER_VOL_B_PIN, ROTARY_ENCODER_VOL_BUTTON_PIN, -1, ROTARY_ENCODER_STEPS);
AiEsp32RotaryEncoder rotaryEncoder_st = AiEsp32RotaryEncoder(ROTARY_ENCODER_ST_A_PIN, ROTARY_ENCODER_ST_B_PIN, -1, -1, ROTARY_ENCODER_STEPS);

String ssid =     "Livebox-E330";
String password = "KdF2DMXQo4gFRjPiG6";

bool bluetoothmode = false;

#define VOLUME_INIT 8
#define VOLUME_MAX 20
int volume = VOLUME_INIT, lastvolume = 0;
String station;

VS1053 mp3(VS1053_CS, VS1053_DCS, VS1053_DREQ, VSPI, VS1053_MOSI, VS1053_MISO, VS1053_SCK);
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_LN);

//VS1053 player(VS1053_CS, VS1053_DCS, VS1053_DREQ);
BluetoothA2DPSink* a2dp_sink = NULL;
#include "bluetoothsink.h"

// LCD display management
String ln1, ln2, lastln1, lastln2;
uint8_t ln2pos = 0, ln2len = 0;
bool ln2scroll = false, doscroll = false;
String BLANK ("                                ");
#define LN2HOLDCYCLES 15 // nb of cycles where beg of line 2 is hold
uint8_t wait1st = 0;

void IRAM_ATTR readEncoderISR_st()
{
	rotaryEncoder_st.readEncoder_ISR();
}

void IRAM_ATTR readEncoderISR_vol()
{
	rotaryEncoder_vol.readEncoder_ISR();
}

// LCD display update
void updatelcd()
{
    if (ln2scroll && doscroll)  // we must scroll line 2
    {
      lcd.setCursor(0,1);
      lcd.print(ln2.substring(ln2pos,ln2pos+LCD_COLS));
      doscroll = false;
    }
    if (ln1!=lastln1)           // we must update line 1
    {
        lcd.setCursor(0,0);
        if (ln1.length()>LCD_COLS)
        {
            lcd.print(ln1.substring(0,LCD_COLS));
        }
        else
        {
            lcd.print(ln1);
            lcd.print(BLANK.substring(0,LCD_COLS-ln1.length()));
        }
        lastln1=ln1;
    }
    if (ln2!=lastln2)           // we must update line 2
    {
        Serial.println("PT4");
        lcd.setCursor(0,1);
        ln2len = ln2.length();
        if (ln2len>LCD_COLS)    // we will have to scroll
        {
            ln2pos = 0;
            ln2scroll = true;
            lcd.print(ln2.substring(0,LCD_COLS));
        }
        else                    // no scroll needed
        {
            ln2scroll = false;
            lcd.print(ln2);
            lcd.print(BLANK.substring(0,LCD_COLS-ln2len));
        }
        lastln2=ln2;
    }
}

// slow timer: update of line 1
hw_timer_t * timer_1sec = NULL;
void IRAM_ATTR timer_1sec_fn()
{
    if (bluetoothmode)
        ln1 = "-- Bluetooth --";
    else
        if (volume == 0)
            ln1 = "   -- Mute --  ";
        else
            ln1 = station;
}

// fast timer: update of line 2 (scroll)
hw_timer_t * timer_02sec = NULL;
void IRAM_ATTR timer_02sec_fn()
{
  if (ln2scroll)
  {
    if (ln2pos > 0 || wait1st >= LN2HOLDCYCLES)
    {
        doscroll = true;            // display must be updated
        if (ln2pos+LCD_COLS < ln2len)
            ln2pos++;
        else
        ln2pos = 0;
        wait1st = 0;
    }
    else
        if (wait1st < LN2HOLDCYCLES)
            wait1st ++;
  }
}

// new stream opening
void open_station()
{
    mp3.connecttohost(sta_url[station_ind]);
    station = sta_name[station_ind];
    ln2 = "";
}

void initbluetooth ()
{
  Serial.println("Opening Bluetooth...");
  WiFi.disconnect();
  //SPI.begin();
  a2dp_sink = new BluetoothA2DPSink;
  circBuffer.flush();
  a2dp_sink->set_stream_reader(read_data_stream, false);
  a2dp_sink->set_avrc_metadata_callback(avrc_metadata_callback);
  a2dp_sink->start(BLUETOOTH_NAME);
  delay(100); 
  circBuffer.write((char *)bt_wav_header, 44);
  delay(100);
}

void closebluetooth ()
{
    Serial.println("Closing Bluetooth...");
    delete a2dp_sink;
    a2dp_sink = NULL;
}

// general setup
void setup() {
    Wire.begin();

	// initialize the LCD
	lcd.begin(16,2);
	lcd.backlight();

    Serial.begin(115200);

    SPI.begin(VS1053_SCK, VS1053_MISO, VS1053_MOSI);
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    while (WiFi.status() != WL_CONNECTED)
    {
      delay(1500);
      Serial.println("(connecting...)");
      ln1="(connecting...)";
    }
    Serial.println("Wifi connected");
    ln1="Wifi connected";

    mp3.begin();
    mp3.setVolume(volume); 
/*
    initbluetooth();
    bluetoothmode = true;
*/
    // Timer 1 initialization (slow)
    timer_1sec = timerBegin(0, 80, true);
    timerAttachInterrupt(timer_1sec, &timer_1sec_fn, true);
    timerAlarmWrite(timer_1sec, 2000000, true);
    timerAlarmEnable(timer_1sec);
        
    // Timer2 initialization (fast)
    timer_02sec = timerBegin(1, 80, true);
    timerAttachInterrupt(timer_02sec, &timer_02sec_fn, true);
    timerAlarmWrite(timer_02sec, 300000, true);
    timerAlarmEnable(timer_02sec);

	// Volume rotary encoder initialisation
    pinMode(ROTARY_ENCODER_VOL_A_PIN, INPUT_PULLUP);
    pinMode(ROTARY_ENCODER_VOL_B_PIN, INPUT_PULLUP);
    rotaryEncoder_vol.areEncoderPinsPulldownforEsp32=false;
    rotaryEncoder_vol.begin();
	rotaryEncoder_vol.setup(readEncoderISR_vol);
	rotaryEncoder_vol.setBoundaries(-(VOLUME_MAX/2), VOLUME_MAX/2, false); //minValue, maxValue, circleValues true|false (when max go to min and vice versa)
	rotaryEncoder_vol.setAcceleration(0); //or set the value - larger number = more accelearation; 0 or 1 means disabled acceleration

	// Stations rotary encoder initialisation
    pinMode(ROTARY_ENCODER_ST_A_PIN, INPUT_PULLUP);
    pinMode(ROTARY_ENCODER_ST_B_PIN, INPUT_PULLUP);
    rotaryEncoder_st.areEncoderPinsPulldownforEsp32=false;
    rotaryEncoder_st.begin();
	rotaryEncoder_st.setup(readEncoderISR_st);
	rotaryEncoder_st.setBoundaries(0, STATION_NB-1, true); //minValue, maxValue, circleValues true|false (when max go to min and vice versa)
	rotaryEncoder_st.setAcceleration(0); //or set the value - larger number = more accelearation; 0 or 1 means disabled acceleration

    open_station();
}


// The loop function is called in an endless loop
void loop()
{
  // We see if a command is input on serial interface
  if (Serial.available() > 0)
  {
      int cmd = Serial.read();
      if (cmd=='+')
      {
          if (volume<VOLUME_MAX - 2)
              volume += 2;
          mp3.setVolume(volume);
          Serial.println("Vol up");
          ln1=String("   Volume: ")+volume;
      }
      if (cmd=='-')
      {
          if (volume>0)
              volume -= 2;
          mp3.setVolume(volume);
          Serial.println("Vol down");
          ln1=String("   Volume: ")+volume;
      }
      if (cmd=='o')
      {
          Serial.println("Enter stream url: ");
          String newurl = Serial.readString();
          Serial.println(String("Opening: ")+newurl);
          bool status = mp3.connecttohost(newurl);
          station = newurl;
          Serial.println(String("'connecttohost' returned: ")+status);
      }
      if (cmd=='p')
      {
        if (station_ind < STATION_NB-1)
            station_ind++;
        else
            station_ind=0;
        open_station();
      }
      if (cmd=='m')
      {
        if (station_ind > 0)
            station_ind--;
        else
            station_ind=STATION_NB-1;
        open_station();
      }
      if (cmd=='b')
      {
        bluetoothmode = ! bluetoothmode;
		if (bluetoothmode)
	      initbluetooth();
        else
          closebluetooth();
      }
  }
  if (bluetoothmode)
	handle_stream();
  else  
    mp3.loop();
  updatelcd();

  if (rotaryEncoder_st.encoderChanged())
  {
      Serial.println(String("encoder changed: ")+rotaryEncoder_st.readEncoder());
      station_ind = rotaryEncoder_st.readEncoder();
      open_station();
  }


  if (rotaryEncoder_vol.encoderChanged())
  {
      Serial.println(String("encoder changed: ")+rotaryEncoder_vol.readEncoder());
      volume = (VOLUME_MAX/2)+rotaryEncoder_vol.readEncoder();
      ln1=String("   Volume: ")+volume;
      mp3.setVolume(volume);
  }
  if (rotaryEncoder_vol.isEncoderButtonClicked())
  {
      Serial.println("button clicked");
      if (volume > 0)
      {
        lastvolume = volume;
        volume = 0;
      }
      else
      {
        volume = lastvolume;
      }
      mp3.setVolume(volume);
  }


}

// next code is optional:
void vs1053_info(const char *info) {                // called from vs1053
    Serial.print("DEBUG:        ");
    Serial.println(info);                           // debug infos
}
void vs1053_showstation(const char *info){          // called from vs1053
    Serial.print("STATION:      ");
    Serial.println(info);                           // Show station name
}
void vs1053_showstreamtitle(const char *info){      // called from vs1053
    Serial.print("STREAMTITLE:  ");
    Serial.println(info);                           // Show title
    ln2=info;
}
void vs1053_showstreaminfo(const char *info){       // called from vs1053
    Serial.print("STREAMINFO:   ");
    Serial.println(info);                           // Show streaminfo
}
void vs1053_eof_mp3(const char *info){              // called from vs1053
    Serial.print("vs1053_eof:   ");
    Serial.print(info);                             // end of mp3 file (filename)
}
void vs1053_bitrate(const char *br){                // called from vs1053
    Serial.print("BITRATE:      ");
    Serial.println(String(br)+"kBit/s");            // bitrate of current stream
}
void vs1053_commercial(const char *info){           // called from vs1053
    Serial.print("ADVERTISING:  ");
    Serial.println(String(info)+"sec");             // info is the duration of advertising
}
void vs1053_icyurl(const char *info){               // called from vs1053
    Serial.print("Homepage:     ");
    Serial.println(info);                           // info contains the URL
}
void vs1053_eof_speech(const char *info){           // called from vs1053
    Serial.print("end of speech:");
    Serial.println(info);
}
void vs1053_lasthost(const char *info){             // really connected URL
    Serial.print("lastURL:      ");
    Serial.println(info);
}
