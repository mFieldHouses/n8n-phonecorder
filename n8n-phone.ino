/*************************************************** 
  This is an example for the Adafruit VS1053 Codec Breakout

  Designed specifically to work with the Adafruit VS1053 Codec Breakout 
  ----> https://www.adafruit.com/products/1381

  Adafruit invests time and resources providing this open source code, 
  please support Adafruit and open-source hardware by purchasing 
  products from Adafruit!

  Written by Limor Fried/Ladyada for Adafruit Industries.  
  BSD license, all text above must be included in any redistribution
 ****************************************************/

#define USE_LOG_COPYING // When this is defined, log copying will be used. Comment out this line to disable log copying
#define MAX_LOG_LINES 150 // The maximum amount of lines to remember when using log copying

// Log copying will remember MAX_LOG_LINES messages at most, always including all messages until the SETUP DONE message. 
// When the line count hits MAX_LOG_LINES, the oldest message which is not part of the setup header will be removed.

#define TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3" // POSIX timezone
#define PHONE_HOSTNAME "n8n-pink-phone"

#define UPLOAD_REATTEMPT_TIMEOUT 2000 // in ms. How long the phone will wait before reattempting to upload a file to the webhook after failing to upload

#define RECORDING_PROFILE_PATH "/recording_profile.img" // Path to the .img file that will be used to encode .ogg files when recording

#define PICKUP_SOUND_PATH "/sounds/pickup.mp3" // Path to the .mp3 file that will be played when the phone horn is picked up
#define RESTART_SOUND_PATH "/sounds/restart.mp3" // Path to the .mp3 file that will be played when the phone restarts itself
#define WIFI_ERROR_SOUND_PATH "/sounds/wifi_error.mp3" // Path to the .mp3 file that will be played when the phone is unable to connect to a WiFi network
#define WEBHOOK_ERROR_SOUND_PATH "/sounds/webhook_error.mp3" // Path to the .mp3 file that will be played when the phone is unable to retrieve a webhook URL from the SD card
#define TIME_ERROR_SOUND_PATH "/sounds/time_error.mp3" // Path to the .mp3 file that will be played when the phone is unable to properly synchronize its internal time and date
#define RECORDING_PLUGIN_ERROR_SOUND_PATH "/sounds/recording_error.mp3"

#define DEFAULT_VOLUME 20 // The lower this number, the higher the volume
#define NOTIFY_VOLUME 1 // Same for this

// Pin definitions. Do not change unless you really know what you're doing
#define SHIELD_RESET 6
#define SHIELD_CS 19
#define SHIELD_DCS 20
#define CARDCS 7
#define DREQ 5
#define HORN_SWITCH 16

// Required libraries
// Most of these are all standard libraries and should be installed already alongside ESP32 boards, except for Adafruit_VS1053.h
#include <SPI.h>
#include <Adafruit_VS1053.h>
#include <SD.h>
#include <WiFi.h>
#include <string.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <vector>

// State of the phone
// Is used for the statemachine-ish logic in loop() and to display the status on the status API
enum PhoneState {IDLE, UPLOADING, PLAYING, RECORDING};
PhoneState phone_state = PhoneState::IDLE;

enum PhoneMode {PLAYBACK, RECORD};
PhoneMode phone_mode = PhoneMode::PLAYBACK;

// Whether the phone horn is picked up or not
// Updated at the start of loop().
bool horn_picked_up = false;

// Notification types. Used for bleep error notifications
enum NotificationType {
  SD_CARD_ERROR = 1,
  RECORDING_PLUGIN_ERROR = 2,
  WIFI_ERROR = 3, 
  FILE_ERROR = 4,
  WEBHOOK_ERROR = 5,
  TIME_ERROR = 6,
};

// Upload reattempt timeout
// Will be set to UPLOAD_REATTEMPT_TIMEOUT when uploading fails, and uploading will only be attempted when this timer is < 0
int reattempt_upload_timeout = -1;

// Error flags for display on the API
// Some error flags are excluded because if those errors were raised, the API would not be available, like WiFi errors and "SD card not found" error
bool vs1053_not_found = false;
bool webhook_not_responding = false;// Pin definitions. Do not change unless you really know what you're doing
#define SHIELD_RESET 6
#define SHIELD_CS 19
#define SHIELD_DCS 20
#define CARDCS 7
#define DREQ 5
#define HORN_SWITCH 16
bool webhook_file_not_found = false;
bool recording_plugin_not_found = false;
bool pickup_mp3_missing = false;

// Interface to the VS1053 shield
Adafruit_VS1053_FilePlayer musicPlayer = Adafruit_VS1053_FilePlayer(SHIELD_RESET, SHIELD_CS, SHIELD_DCS, DREQ, CARDCS);

// Web server and client
WebServer server(80);
HTTPClient http;

// Webhook URL buffer
// Will be set in setup() using retrieveWebhookURL()
String webhook_url;

// Copy of the serial communication history for display on /output
#ifdef USE_LOG_COPYING
std::vector<String> log_copy;
int log_line_count = 0;
int setup_header_end = -1;
#endif

// File that the microphone stream will be encoded to when recording
File recording;
#define RECBUFFSIZE 128
uint8_t recording_buffer[RECBUFFSIZE];

// Hacky macros for log copying. Could probably just turn these into actual functions
#ifdef USE_LOG_COPYING
  #define SPRINTLN(x) Serial.println(x);addLogLine(String(x) + "\n")
  #define SPRINT(x) Serial.print(x);addLogLine(String(x))
#else
  #define SPRINTLN(x) Serial.println(x);
  #define SPRINT(x) Serial.print(x);
#endif



// Restarts the ESP and plays the sound at RESTART_SOUND_PATH
void restart() {
  if (phone_mode != PhoneMode::PLAYBACK) {
    enablePlaybackMode();
  }
  
  musicPlayer.setVolume(NOTIFY_VOLUME, NOTIFY_VOLUME);
  musicPlayer.playFullFile(RESTART_SOUND_PATH);

  ESP.restart();
}



void setup() {
  Serial.begin(9600);
  SPRINTLN("\n====== n8n Pink Phone ======");

  #ifndef USE_LOG_COPYING
    log_copy.push_back("Output log copying is disabled. You can re-enable it by uncommenting '#define USE_LOG_COPYING' in the first few lines of the program and reflashing the ESP32.");

  #endif

  // Horn switch setup. Uses a pullup on the input and connects to ground when the horn is down. So 1 is horn up, 0 is horn down
  pinMode(HORN_SWITCH, INPUT_PULLUP);

  if (!musicPlayer.begin()) {  // initialise the music player
    SPRINTLN("Couldn't find VS1053");
    vs1053_not_found = true;
  }
  else {
    SPRINTLN("VS1053 found");
    musicPlayer.setVolume(DEFAULT_VOLUME, DEFAULT_VOLUME);
  }

  SPRINTLN();

  // Initialise SD card
  if (!SD.begin(CARDCS)) {
    SPRINTLN("SD failed, or not present. Restarting");
    speakerNotify(NotificationType::SD_CARD_ERROR);
    restart();
    while (1);
  }

  SPRINTLN("SD Card found");

  SPRINT(getSDCardSpaceUsedPercentage()); SPRINTLN("% of card used");

  SPRINTLN();

  // Buffers for WiFi credentials, to be passed to retrieveWifiCredentials() in 3 lines
  String ssid = "unchanged";
  String password = "unchanged";

  retrieveWifiCredentials(ssid, password);

  SPRINTLN("Retrieved WiFi credentials: ");
  SPRINT("SSID: "); SPRINTLN(ssid);
  SPRINT("Password: "); SPRINTLN(password);
  
  SPRINT("\nattempting to connect to WiFi");

  // Setup WiFi
  WiFi.setHostname(PHONE_HOSTNAME);
  WiFi.begin(ssid, password);

  // Max amounts of connection attempts the ESP can make before restarting
  // An attempt is made every 100ms, so 100 tries would take 10 seconds
  int max_tries = 100;

  while (WiFi.status() != WL_CONNECTED) {
    SPRINT(".");

    if (WiFi.status() == WL_CONNECT_FAILED) {
      SPRINTLN("Could not connect to WiFi. Restarting");
      speakerNotify(NotificationType::WIFI_ERROR);
      restart();
    }

    max_tries--;

    if (max_tries < 0) {
      SPRINTLN("Unable to find WiFi, restarting");
      speakerNotify(NotificationType::WIFI_ERROR);
      restart();
    }

    delay(100);
  }

  SPRINTLN("\nConnected to WiFi");
  SPRINT("Live on IP: "); SPRINTLN(WiFi.localIP());

  // Set up time for file naming
  configTzTime(TIMEZONE, "pool.ntp.org");
  SPRINTLN("\nTime set up");
  
  struct tm timeinfo;
  getLocalTime(&timeinfo);

  Serial.print("The time is ");
  Serial.println(&timeinfo, "%H:%M:%S");

  if (timeinfo.tm_year + 1900 == 1970) { // Common condition when time synchronization did not succeed
    SPRINTLN("Time synchronization failed, restarting");
    speakerNotify(NotificationType::TIME_ERROR);
    restart();
  }
  
  server.on("/", HTTP_GET, handleStatusRequest);
  server.on("/output", HTTP_GET, handleLogRequest);
  server.on("/api/status", HTTP_GET, handleStatusRequest);

  // Set up webserver
  server.begin();

  SPRINTLN("\nWebserver set up");

  // Put webhook URL into buffer to be used later
  bool webhook_found = retrieveWebhookURL(webhook_url);

  if (!webhook_found) {
    speakerNotify(NotificationType::WEBHOOK_ERROR);
    restart();
  }

  SPRINT("\nRetrieved webhook url: "); SPRINTLN(webhook_url);

  SPRINTLN("\nSETUP DONE\n\n========================================\n");

  markSetupHeaderEndLine();
}



void loop() {
  // Update whether the horn is down or not
  horn_picked_up = digitalRead(HORN_SWITCH);

  // Handle incoming HTTP requests
  server.handleClient();

  switch (phone_state) {

    case PhoneState::IDLE: {

      if (horn_picked_up) { // Phone is idle and horn has been picked up
        SPRINTLN("Start playing track");

        enablePlaybackMode();

        musicPlayer.setPlaySpeed(20); // TODO remove this
        musicPlayer.startPlayingFile(PICKUP_SOUND_PATH);

        phone_state = PhoneState::PLAYING;
      }
      else { // Phone is idle and horn is down

        // Try uploading feedback recordings that haven't been uploaded yet

        phone_state = PhoneState::UPLOADING;

        // Check if we are allowed to reattempt upload according to the timeout
        if (reattempt_upload_timeout > 0) {
          reattempt_upload_timeout -= 1;
          delay(1);
          phone_state = PhoneState::IDLE;
          break;
        }

        File dir = SD.open("/recorded/");
        File entry;
        
        while (true) { // Search through all files
          entry = dir.openNextFile();

          if (entry.isDirectory()) {
            continue;
          }

          if (String(entry.path()).indexOf("/uploaded/") == -1 || !entry) { // If this file is not in /recorded/uploaded/ or there is no file
            break;
          }
        }

        if (!entry) { // If there is no file, just abort
          phone_state = PhoneState::IDLE;
          entry.close();
          break;
        }

        SPRINTLN(String("Starting upload of ") + String(entry.name()));
        
        // Whether the response to the HTTP request was 200
        bool successful = uploadFileToWebhook(entry);

        if (successful) {
          SPRINTLN(String("Uploading ") + String(entry.name()) + String(" was successful"));
          markFileAsUploaded(entry);
          phone_state = PhoneState::IDLE;
        }
        else {
          SPRINTLN(String("Uploading ") + String(entry.name()) + String(" was unsuccessful. Will retry in at least 2 seconds"));
          reattempt_upload_timeout = UPLOAD_REATTEMPT_TIMEOUT;
          phone_state = PhoneState::IDLE;
        }
      }
    }
    break;


    case PhoneState::PLAYING: {

      musicPlayer.feedBuffer(); // Manually push MP3 buffer to speaker. Using interrupts for this, as is recommended by the VS1053 library, will cause very frequent crashing of the ESP32
      // SPRINTLN("Push buffer");

      if (horn_picked_up) { // If music is playing and the phone horn is still picked up

        if (!musicPlayer.playingMusic) { // If the track is done playing

          musicPlayer.setPlaySpeed(1); // TODO remove

          if (!enableRecordingMode()) {
            break;
          }

          // Compose filename according to current time and date
          String filename = String("/recorded/") + getTimeStampString() + String(".OGG");

          SPRINT("Recording to file: "); SPRINTLN(filename);
          recording = SD.open(filename, FILE_WRITE);

          if (!recording) {
            SPRINTLN("Couldn't open file to record");
          }
          else {
            SPRINTLN("Start recording");
            musicPlayer.startRecordOgg(true);
            phone_state = PhoneState::RECORDING;
          }
        }
      }
      else { // If track is playing and the phone horn is down
        SPRINTLN("Stop playing track");
        musicPlayer.stopPlaying();
        phone_state = PhoneState::IDLE;
      }
    }
    break;


    case PhoneState::RECORDING: {

      // SPRINTLN("recording");

      if (horn_picked_up) { // If we are currently recording feedback and the phone horn is still picked up
        saveRecordedData(true);
      }
      else { // If we are currently recording feedback and the phone horn has been put down
        SPRINTLN("Stop recording");

        saveRecordedData(true);
        musicPlayer.stopRecordOgg();
        saveRecordedData(false);
        
        recording.close();
        phone_state = PhoneState::IDLE;
        delay(500);
      }
    }
    break;
  }
}



#ifdef USE_LOG_COPYING

// Adds a line to the log copy
void addLogLine(String line) {
  log_copy.push_back(line);

  log_line_count++;

  if (log_line_count > MAX_LOG_LINES) {
    log_copy.erase(log_copy.begin() + setup_header_end + 1); // remove first line after the setup header
    log_line_count = MAX_LOG_LINES;
  }
}



// Marks the index of the end of the setup header for use in addLogLine()
void markSetupHeaderEndLine() {
  setup_header_end = log_line_count - 1;
}

#endif



// Returns a list of errors to be used in handleStatusRequest()
std::vector<String> getErrors() {

  // Macro to make adding new errors to this function just a tiny bit easier
  #define ADDERR(x) result.push_back(x) 

  std::vector<String> result;

  if (vs1053_not_found) {
    ADDERR("VS1053 was not found");
  }
  if (webhook_not_responding) {
    ADDERR("Webhook not responding with OK code");
  }
  if (webhook_file_not_found) {
    ADDERR("Webhook URL file was not found");
  }
  if (recording_plugin_not_found) {
    ADDERR(String("Recording plugin image could not be found at ") + String(RECORDING_PROFILE_PATH));
  }
  if (pickup_mp3_missing) {
    ADDERR("/sounds/pickup.mp3 could not be found");
  }

  return result;
  #undef ADDERR
}



// Called when a request is made to <hostname> or <hostname>/api/status
void handleStatusRequest() {

  // Actual body to respond with
  String result;

  std::vector<String> errors = getErrors();

  result += "{\n";

  // status
  result += String("  \"status\": \"") + getStatusString() + String("\"\n");
  
  if (phone_state == PhoneState::IDLE || phone_state == PhoneState::UPLOADING) {
    // errors
    result += "  \"errors\": [\n";
    for (const String str : errors) {
      result += "    \"" + str + "\",\n";
    }
    result += "  ]\n";

    // space_used_percentage
    result += "  \"space_used_percentage\": ";
    result += String(getSDCardSpaceUsedPercentage(), 2);
    result += ",\n";
    
    // total_recordings_count
    result += "  \"total_recordings_count\": ";
    result += String(countFilesInDirectory("/recorded/"));
    result += ",\n";

    // uploaded_recordings_count
    result += "  \"uploaded_recordings_count\": ";
    result += String(countFilesInDirectory("/recorded/uploaded/"));
    result += ",\n";
  }
  else {
    result += "  \"errors\": [\n    \"Phone playing or recording sound, will not retrieve full status\"\n  ]\n";

    result += "  \"space_used_percentage\": -1.0,\n";
    result += "  \"total_recordings_count\": -1,\n";
    result += "  \"uploaded_recordings_count\": -1,\n";
  }

  result += "}";

  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "text/javascript; charset=utf-8", result);
}



// Called when a request is made to <hostname>/output 
void handleLogRequest() {

  // Actual body to respond with
  String result;

  for (const String line : log_copy) {
    result += line;
  }

  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "text/plain; charset=utf-8", result);
}



/// File listing helper
void printDirectory(File dir, int numTabs) {
  while (true) {

    File entry = dir.openNextFile();
    if (!entry) {
      // no more files
      //Serial.println("**nomorefiles**");
      break;
    }
    for (uint8_t i = 0; i < numTabs; i++) {
      SPRINT('\t');
    }
    Serial.print(entry.name());
    if (entry.isDirectory()) {
      SPRINTLN("/");
      printDirectory(entry, numTabs + 1);
    } else {
      // files have sizes, directories do not
      Serial.print("\t\t");
      SPRINTLN(String(entry.size(), DEC));
    }
    entry.close();
  }
}



// Plays a number of bleeps on the phone speaker based on the error type
void speakerNotify(NotificationType type) {
  musicPlayer.setVolume(NOTIFY_VOLUME, NOTIFY_VOLUME);

  switch (type) {
    case NotificationType::WIFI_ERROR:
      musicPlayer.playFullFile(WIFI_ERROR_SOUND_PATH);
      break;
    
    case NotificationType::WEBHOOK_ERROR:
      musicPlayer.playFullFile(WEBHOOK_ERROR_SOUND_PATH);
      break;
    
    case NotificationType::TIME_ERROR:
      musicPlayer.playFullFile(TIME_ERROR_SOUND_PATH);
      break;
    
    case NotificationType::RECORDING_PLUGIN_ERROR:
      musicPlayer.playFullFile(RECORDING_PLUGIN_ERROR_SOUND_PATH);
      break;
    
    case NotificationType::SD_CARD_ERROR:
      speakerPulses(1);
      delay(100);
      break;
  }

  musicPlayer.setVolume(DEFAULT_VOLUME, DEFAULT_VOLUME);
  return;
}



// Plays a number of bleeps on the phone speaker
void speakerPulses(int pulse_count) {
  for (int i = 0; i < pulse_count; i++) {
    musicPlayer.sineTest(10, 20);
    delay(40);
  }
}



// Retrieves the WiFi credentials from wifi.txt and puts them into the specified buffers
// This function assumes the SD is operational since it's only ever called after SD card initialization
void retrieveWifiCredentials(String &ssid_str, String &password_str) {
  if (!SD.exists("/wifi.txt")) {
    speakerNotify(NotificationType::WIFI_ERROR);
    return;
  }
  
  File creds_file = SD.open("/wifi.txt");

  String ssid_line = creds_file.readStringUntil((char) 10); // 10 is the ASCII newline character
  String password_line = creds_file.readStringUntil((char) 10);

  ssid_str = ssid_line.substring(5); // substring starting after "ssid="
  password_str = password_line.substring(9); // substring starting after "password="
}



// Retrieves the webhook URL from webhook.txt and puts it into the specified buffer
bool retrieveWebhookURL(String &webhook_url_buf) {
  if (!SD.exists("/webhook.txt")) {
    speakerNotify(NotificationType::WEBHOOK_ERROR);
    webhook_file_not_found = true;
    return false;
  }

  File webhook_url_file = SD.open("/webhook.txt");

  webhook_url_buf = webhook_url_file.readStringUntil((char) 10);
  
  if (webhook_url_buf.isEmpty()) {
    return false;
  }
  
  return true; 
}



// Uploads a file to the webhook specified in webhook.txt
// Returns whether the upload was successful
bool uploadFileToWebhook(File &file) {
  if (!http.begin(webhook_url)) {
    SPRINTLN("http error");
    return -1;
  }
  
  String filename = String(file.name());

  http.addHeader("Content-Disposition", String("attachment; filename = \"") + filename + String("\""));
  http.addHeader("Content-Type", "audio/ogg");

  int result = http.sendRequest("POST", &file, file.size());

  webhook_not_responding = result != 200;

  http.end();

  return result == 200;
}



// Forward declaration for use in markFileAsUploaded()
void moveFile(File&, String, bool);

// Moves file into /recorded/uploaded/
void markFileAsUploaded(File &file) {
  String destination_path = String("/recorded/uploaded/") + String(file.name());

  if (SD.exists(destination_path)) {
    SPRINTLN(String("File ") + String(file.name()) + String(" was already marked as uploaded"));
    SD.remove(file.path());
    return;
  }

  moveFile(file, destination_path, true);
}



// Moves file to the specified path
void moveFile(File &file, String destination_path, bool remove_original = false) {
  File destination = SD.open(destination_path, FILE_WRITE);

  uint8_t buffer[512];

  file.seek(0);

  while (file.available()) {
    size_t n = file.read(buffer, sizeof(buffer));
    if (n == 0) break;

    destination.write(buffer, n);
  }

  destination.close();

  if (remove_original) {
    SD.remove(file.path());
  }
}



// Encodes incoming microphone data into OGG file
// Taken from record_ogg example of the VS1053 library
uint16_t saveRecordedData(boolean isrecord) {
  uint16_t written = 0;
  
  // read how many words are waiting for us
  uint16_t wordswaiting = musicPlayer.recordedWordsWaiting();
  
  // try to process 256 words (512 bytes) at a time, for best speed
  while (wordswaiting > 256) {
    //Serial.print("Waiting: "); Serial.println(wordswaiting);
    // for example 128 bytes x 4 loops = 512 bytes
    for (int x=0; x < 512/RECBUFFSIZE; x++) {
      // fill the buffer!
      for (uint16_t addr=0; addr < RECBUFFSIZE; addr+=2) {
        uint16_t t = musicPlayer.recordedReadWord();
        //Serial.println(t, HEX);
        recording_buffer[addr] = t >> 8; 
        recording_buffer[addr+1] = t;
      }
      if (! recording.write(recording_buffer, RECBUFFSIZE)) {
            Serial.print("Couldn't write "); Serial.println(RECBUFFSIZE); 
            while (1);
      }
    }
    // flush 512 bytes at a time
    recording.flush();
    written += 256;
    wordswaiting -= 256;
  }
  
  wordswaiting = musicPlayer.recordedWordsWaiting();
  if (!isrecord) {
    Serial.print(wordswaiting); Serial.println(" remaining");
    // wrapping up the recording!
    uint16_t addr = 0;
    for (int x=0; x < wordswaiting-1; x++) {
      // fill the buffer!
      uint16_t t = musicPlayer.recordedReadWord();
      recording_buffer[addr] = t >> 8; 
      recording_buffer[addr+1] = t;
      if (addr > RECBUFFSIZE) {
          if (! recording.write(recording_buffer, RECBUFFSIZE)) {
                Serial.println("Couldn't write!");
                while (1);
          }
          recording.flush();
          addr = 0;
      }
    }
    if (addr != 0) {
      if (!recording.write(recording_buffer, addr)) {
        Serial.println("Couldn't write!"); while (1);
      }
      written += addr;
    }
    musicPlayer.sciRead(VS1053_SCI_AICTRL3);
    if (! (musicPlayer.sciRead(VS1053_SCI_AICTRL3) & (1 << 2))) {
       recording.write(musicPlayer.recordedReadWord() & 0xFF);
       written++;
    }
    recording.flush();
  }

  return written;
}



// Pads strings with 0s at their starts
String padZero(String input) {
  if (input.length() == 1) {
    return "0" + input;
  }

  return input;
}



// Returns time stamp as a specifically formatted string for file names
String getTimeStampString() {
  struct tm timeinfo;
  getLocalTime(&timeinfo);

  //return padZero(String(timeinfo.tm_hour)) + padZero(String(timeinfo.tm_min)) + padZero(String(timeinfo.tm_sec));
  return String(timeinfo.tm_year + 1900) + "-" + padZero(String(timeinfo.tm_mon + 1)) + "-" + padZero(String(timeinfo.tm_mday)) + "_" + padZero(String(timeinfo.tm_hour)) + "-" + padZero(String(timeinfo.tm_min)) + "-" + padZero(String(timeinfo.tm_sec));
}



// Returns the current status as a string to be used in handleStatusRequest
String getStatusString() {
  switch (phone_state) {
    case PhoneState::PLAYING:
    case PhoneState::RECORDING:
      return String("in_use");
    
    case PhoneState::UPLOADING:
      return String("uploading");

    default:
      return String("idle");
  }
}



// Returns how many recordings have been made in total, including uploaded files
int getRecordedFilesCount() {
  Serial.println("get recorded files count"); // TODO fix this

  File dir = SD.open("/recorded/");
  
  int count = 0;

  while (1) { // Search through all files
    // if (!dir.isDirectory()) {
    //   count++;
    // }
    count++;
    dir = dir.openNextFile();

    if (!dir.available() && !dir.isDirectory()) {
      break;
    }
  }

  return count;
}



// Returns the amount of files in the specified directory, recursively counted
int countFilesInDirectory(String dir_path) {
  if (!SD.exists(dir_path)) {
    return 0;
  }

  int count = 0;

  File dir = SD.open(dir_path);
  File entry;
        
  while (true) { // Search through all files
    entry = dir.openNextFile();

    if (entry.isDirectory()) {
      continue;
    }

    if (!entry) { // If this file is not in /recorded/uploaded/ or there is no file
      break;
    }

    count++;
  }

  return count;
}



// "Enables" playback mode by just soft resetting
void enablePlaybackMode() {
  if (phone_mode != PhoneMode::PLAYBACK) {
    musicPlayer.softReset();
  }

  phone_mode = PhoneMode::PLAYBACK;
}



// Enables recording mode by loading the plugin
// Some internal mode is changed when this is done, making playback impossible after prepareRecordOgg() has been called
// Returns whether the plugin has been loaded in
bool enableRecordingMode() {
  if (phone_mode != PhoneMode::RECORD) {
    if (!musicPlayer.prepareRecordOgg(RECORDING_PROFILE_PATH)) {
      SPRINTLN("Couldn't load OGG recording plugin");
      recording_plugin_not_found = true;
      speakerNotify(NotificationType::RECORDING_PLUGIN_ERROR);
      restart();
      return false;
    }
  }

  phone_mode = PhoneMode::RECORD;

  return true;
}



// Calculates and returns the percentage of space that is used on the SD card
float getSDCardSpaceUsedPercentage() {
  return ((float) SD.usedBytes()) / ((float) SD.totalBytes()) * 100.0;
}