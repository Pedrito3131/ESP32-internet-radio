#include "Arduino.h"
#include "WiFi.h"
#include "Audio.h"
//#include <driver/dac.h>

const static char *SSID = "Livebox-E330";
const static char *PASSWORD = "KdF2DMXQo4gFRjPiG6";

const uint8_t kPinI2S_BCLK = 12; // red (PCM5102A board: BCK)
const uint8_t kPinI2S_LRCK = 14; // yellow (PCM5102A board: LRCK)
const uint8_t kPinI2S_SD = 13; // orange (PCM5102A board: DIN)

// Own host name announced to the WiFi network
const char* kDeviceName = "ESP32-Webradio";

/** Maximum audio volume that can be set in the 'esp32-audioI2S' library */
const uint8_t kVolumeMax = 21;

/** Width of the stream title sprite in pixels */
const int16_t kTitleSpriteWidth = 1000;

/** Web radio stream URLs */
const String kStationURLs[] = {
    "http://icecast.radiofrance.fr/franceinter-midfi.mp3", // "icecast.radiofrance.fr/franceinter-hifi.aac",
    "http://radioclassique.ice.infomaniak.ch/radioclassique-high.mp3",
    "http://icecast.radiofrance.fr/fip-midfi.mp3", // "icecast.radiofrance.fr/fip-hifi.aac",
    "http://icecast.radiofrance.fr/franceculture-midfi.mp3", // "icecast.radiofrance.fr/franceculture-hifi.aac",
    "http://icecast.radiofrance.fr/francemusique-midfi.mp3", // "icecast.radiofrance.fr/francemusique-hifi.aac",
    "http://ouifm.ice.infomaniak.ch/ouifm-high.mp3",
    "http://novazz.ice.infomaniak.ch/novazz-128.mp3",
    "http://streaming.radio.rtl2.fr/rtl2-1-44-128?listen=webCwsBCggNCQgLDQUGBAcGBg"
};

/** Number of stations */
const uint8_t kNumStations = sizeof(kStationURLs) / sizeof(kStationURLs[0]);
Audio audio_ = Audio(false); // Use external DAC

// Content in audio buffer (provided by esp32-audioI2S library)
uint32_t audioBufferFilled_ = 0;

// Size of audio buffer (provided by esp32-audioI2S library)
uint32_t audioBufferSize_ = 0;

// Current station index
uint8_t stationIndex_ = 0;

// Flag to indicate the user wants to changed the station
bool stationChanged_ = true;

// Flag to indicate that audio is muted after tuning to a new station
bool stationChangedMute_ = true;

// Name of the current station as provided by the stream header data
String stationStr_ = "";

// Flag indicating the station name has changed
bool stationUpdatedFlag_ = false;

// Flag indicating that the connection to a host could not be established
bool connectionError_ = false;
// Title of the current song as provided by the stream meta data
String titleStr_ = "";

// Flag indicating the song title has changed
bool titleUpdatedFlag_ = false;


// Audio volume to be set by the audio task
uint8_t volumeCurrent_ = 0;

// Volume as float value for fading
float_t volumeCurrentF_ = 0.0f;

// Flag indicating the volume needs to be set by the audio task
bool volumeCurrentChangedFlag_ = true;

// Audio volume that is set during normal operation
uint8_t volumeNormal_ = kVolumeMax;

// Time in milliseconds at which the connection to the chosen stream has been established
uint64_t timeConnect_ = 0;

/**
 * Enable or disable the shutdown circuit of the amplifier.
 * Amplifier: M5Stack SPK hat with PAM8303.
 * - b = true  --> GPIO_0 = 0 : Shutdown enabled
 * - b = false --> GPIO_0 = 1 : Shutdown disabled
 */
void setAudioShutdown(bool b) {
    /*
    if (b) {
        gpio_set_level(GPIO_NUM_0, 0); // Enable shutdown circuit
    }
    else {
        gpio_set_level(GPIO_NUM_0, 1); // Disable shutdown circuit
    }
    */
}

/**
 * Function to be executed by the audio processing task.
 */
void audioProcessing(void *p) {
    while (true) {
        // Process requested change of audio volume
        if (volumeCurrentChangedFlag_) {
            audio_.setVolume(volumeCurrent_);
            volumeCurrentChangedFlag_ = false; // Clear flag
        }
        
        // Proces requested station change
        if (stationChanged_) {
            audio_.stopSong();
            setAudioShutdown(true); // Turn off amplifier
            stationChangedMute_ = true; // Mute audio until stream becomes stable

            // Establish HTTP connection to requested stream URL
            const char *streamUrl = kStationURLs[stationIndex_].c_str();

            Serial.printf("Connecting to host: %s\n",streamUrl);

            bool success = audio_.connecttohost( streamUrl );

            if (success) {
                Serial.println("Connection OK");
                stationChanged_ = false; // Clear flag
                connectionError_ = false; // Clear in case a connection error occured before

                timeConnect_ = millis(); // Store time in order to detect stream errors after connecting
            }
            else {
                Serial.println("Connection failed");
                stationChanged_ = false; // Clear flag
                connectionError_ = true; // Raise connection error flag
            }

            // Update buffer state variables
            audioBufferFilled_ = audio_.inBufferFilled(); // 0 after connecting
            audioBufferSize_ = audio_.inBufferFree() + audioBufferFilled_;
        }

        // After the buffer has been filled up sufficiently enable audio output
        if (stationChangedMute_) {
            if ( audioBufferFilled_ > 0.9f * audioBufferSize_) {
                setAudioShutdown(false);
                stationChangedMute_ = false;
                connectionError_ = false;
            }
            else {
                // If the stream does not build up within a few seconds something is wrong with the connection
                if ( millis() - timeConnect_ > 3000 ) {
                    if (!connectionError_) {
                        Serial.printf("Audio buffer low: %u of %u bytes.\n", audioBufferFilled_, audioBufferSize_);
                        connectionError_ = true; // Raise connection error flag
                    }
                }
            }
        }

        // Let 'esp32-audioI2S' library process the web radio stream data
        audio_.loop();

        audioBufferFilled_ = audio_.inBufferFilled(); // Update used buffer capacity
        
        vTaskDelay(1 / portTICK_PERIOD_MS); // Let other tasks execute
    }
}


void setup() {
 	Serial.begin(115200);
    while(!Serial);
    Serial.println("Setup...");
    
    pinMode(LED_BUILTIN, OUTPUT);
	digitalWrite(LED_BUILTIN, HIGH);
	delay(200);
	digitalWrite(LED_BUILTIN, LOW);
	delay(200);
	digitalWrite(LED_BUILTIN, HIGH);
	delay(200);
	digitalWrite(LED_BUILTIN, LOW);
	delay(200);
    digitalWrite(LED_BUILTIN, HIGH);
	delay(200);
	digitalWrite(LED_BUILTIN, LOW);

    Serial.println(" Connecting to WiFi...");
    Serial.printf(" SSID: %s\n", SSID); // WiFi network name

    // Initialize WiFi and connect to network
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(kDeviceName);
    WiFi.begin(SSID, PASSWORD);

    while (!WiFi.isConnected()) {
        delay(100);
    }

    // Display own IP address after connecting
    Serial.println(" Connected to WiFi");
    Serial.printf(" IP: %s\n", WiFi.localIP().toString().c_str());

    // Setup audio
    audio_.setVolume(0); // 0...21
    audio_.setPinout(kPinI2S_BCLK, kPinI2S_LRCK, kPinI2S_SD);

    // Start the audio processing task
    xTaskCreate(audioProcessing, "Audio processing task", 4096, nullptr, configMAX_PRIORITIES - 1, nullptr);

    // Wait some time before wiping out the startup screen
  //  vTaskDelay(2000 / portTICK_PERIOD_MS);
}

void loop() {
//	digitalWrite(LED_BUILTIN, LOW);
//	delay(500);
//	digitalWrite(LED_BUILTIN, HIGH);
//	delay(500);


    // Button A: Switch to next station
    if (false) {
        
        // Turn down volume
        volumeCurrent_ = 0;
        volumeCurrentF_ = 0.0f;
        volumeCurrentChangedFlag_ = true; // Raise flag for the audio task

        // Advance station index to next station
        stationIndex_ = (stationIndex_ + 1) % kNumStations;
        stationChanged_ = true; // Raise flag for the audio task

        // Erase station name
        stationStr_ = "";
        stationUpdatedFlag_ = true; // Raise flag for display update routine

        // Erase stream info
        titleStr_ = "";
        titleUpdatedFlag_ = true; // Raise flag for display update routine
    }
    else {
        // Increase volume gradually after station change
        if (!stationChangedMute_ && volumeCurrent_ < volumeNormal_) {
            volumeCurrentF_ += 0.25;
            volumeCurrent_ = (uint8_t) volumeCurrentF_;
            volumeCurrentChangedFlag_ = true; // Raise flag for the audio task
            Serial.printf("Vol: %2u\n", volumeCurrent_);
        }
    }

    if (connectionError_) {
        Serial.println("connectionError");
        vTaskDelay(200 / portTICK_PERIOD_MS); // Wait until next cycle
    }
    else {
        // Update the station name if flag is raised
        if (stationUpdatedFlag_) {
			Serial.println("station changed");
            stationUpdatedFlag_ = false; // Clear update flag
        }

        // Update the song title if flag is raised
        if (titleUpdatedFlag_) {

            Serial.println(titleStr_);

            titleUpdatedFlag_ = false; // Clear update flag


        }
        else {
        }

        vTaskDelay(20 / portTICK_PERIOD_MS); // Wait until next cycle
    }


}

