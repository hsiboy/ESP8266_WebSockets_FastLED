#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WiFiMulti.h>
#include <WebSocketsServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Hash.h>
#include <FS.h>

#include "FastLED.h"
FASTLED_USING_NAMESPACE
 
#if defined(FASTLED_VERSION) && (FASTLED_VERSION < 3001000)
#warning "Requires FastLED 3.1 or later; check github for latest code."
#endif
 
#define DATA_PIN      13     // for Huzzah: Pins w/o special function:  #4, #5, #12, #13, #14; // #16 does not work :(
#define LED_TYPE      WS2811
#define COLOR_ORDER   GRB
#define NUM_LEDS      60    
CRGB leds[NUM_LEDS];
 
uint8_t BRIGHTNESS =           128;   // this is half brightness
uint8_t new_BRIGHTNESS =       128;   // shall be initially the same as brightness
 
#define FRAMES_PER_SECOND  120    // here you can control the speed. With the Access Point / Web Server the
                                  // animations run a bit slower.
uint8_t gHue = 0; // rotating "base color" used by many of the patterns
uint8_t ledMode = 2;                  // this is the starting animation

bool blackout = false;               // used for "on/off" switch
 
// The SSID to connect to
const char* ssid     = "GUEST_WIFI";
// the password of above SSID
const char* password = "";
 
#define USE_SERIAL Serial

// the mDNS host id to use
const char* mDNSid   = "Websockets";

ESP8266WiFiMulti WiFiMulti;

// serve the app from here
ESP8266WebServer server(80);

//holds the current upload
File fsUploadFile;
WebSocketsServer webSocket = WebSocketsServer(81);

//format bytes
String formatBytes(size_t bytes){
  if (bytes < 1024){
    return String(bytes)+"B";
  } else if(bytes < (1024 * 1024)){
    return String(bytes/1024.0)+"KB";
  } else if(bytes < (1024 * 1024 * 1024)){
    return String(bytes/1024.0/1024.0)+"MB";
  } else {
    return String(bytes/1024.0/1024.0/1024.0)+"GB";
  }
}

String getContentType(String filename){
  if(server.hasArg("download")) return "application/octet-stream";
  else if(filename.endsWith(".htm")) return "text/html";
  else if(filename.endsWith(".html")) return "text/html";
  else if(filename.endsWith(".css")) return "text/css";
  else if(filename.endsWith(".js")) return "application/javascript";
  else if(filename.endsWith(".png")) return "image/png";
  else if(filename.endsWith(".gif")) return "image/gif";
  else if(filename.endsWith(".jpg")) return "image/jpeg";
  else if(filename.endsWith(".ico")) return "image/x-icon";
  else if(filename.endsWith(".xml")) return "text/xml";
  else if(filename.endsWith(".pdf")) return "application/x-pdf";
  else if(filename.endsWith(".zip")) return "application/x-zip";
  else if(filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

bool handleFileRead(String path){
  Serial.println("handleFileRead: " + path);

  if(path.endsWith("/")) path += "index.htm";
  String contentType = getContentType(path);
  String pathWithGz = path + ".gz";
  if(SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)){
    if(SPIFFS.exists(pathWithGz))
      path += ".gz";
    File file = SPIFFS.open(path, "r");
    if (!file) {
    Serial.println("oof! file open failed");
    }
    size_t sent = server.streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}

void handleFileUpload(){
  if(server.uri() != "/edit") return;
  HTTPUpload& upload = server.upload();
  if(upload.status == UPLOAD_FILE_START){
    String filename = upload.filename;
    if(!filename.startsWith("/")) filename = "/"+filename;

    Serial.print("handleFileUpload Name: "); Serial.println(filename);
    fsUploadFile = SPIFFS.open(filename, "w");
    filename = String();
  } else if(upload.status == UPLOAD_FILE_WRITE){
    //Serial.print("handleFileUpload Data: "); Serial.println(upload.currentSize);

    if(fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize);
  } else if(upload.status == UPLOAD_FILE_END){
    if(fsUploadFile)
      fsUploadFile.close();

    Serial.print("handleFileUpload Size: "); Serial.println(upload.totalSize);
  }
}

void handleFileDelete(){
  if(server.args() == 0) return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);

  Serial.println("handleFileDelete: " + path);

  if(path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if(!SPIFFS.exists(path))
    return server.send(404, "text/plain", "FileNotFound");
  SPIFFS.remove(path);
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileCreate(){
  if(server.args() == 0)
    return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);

  Serial.println("handleFileCreate: " + path);

  if(path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if(SPIFFS.exists(path))
    return server.send(500, "text/plain", "FILE EXISTS");
  File file = SPIFFS.open(path, "w");
  if(file)
    file.close();
  else
    return server.send(500, "text/plain", "CREATE FAILED");
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileList() {
  if(!server.hasArg("dir")) {server.send(500, "text/plain", "BAD ARGS"); return;}
  
  String path = server.arg("dir");

  Serial.println("handleFileList: " + path);

  Dir dir = SPIFFS.openDir(path);
  path = String();

  String output = "[";
  while(dir.next()){
    File entry = dir.openFile("r");
    if (output != "[") output += ',';
    bool isDir = false;
    output += "{\"type\":\"";
    output += (isDir)?"dir":"file";
    output += "\",\"name\":\"";
    output += String(entry.name()).substring(1);
    output += "\"}";
    entry.close();
  }
  
  output += "]";
  server.send(200, "text/json", output);
}


void setColor(CRGB colour, int pixel) {

  for (int i=0; i<NUM_LEDS; i++) {
    leds[i] = CRGB::Black;
  }
   leds[pixel] = colour;
  FastLED.show();
}


void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t lenght) {

    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[%u] Disconnected!\n", num);
            break;
        case WStype_CONNECTED: {
            IPAddress ip = webSocket.remoteIP(num);
            Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);

            // send message to client
            webSocket.sendTXT(num, "Connected");
        }
            break;
        case WStype_TEXT:
           

            Serial.printf("[%u] get Text: %s\n", num, payload);

            if(payload[0] == '#') {
                // we get RGB data
                // decode rgb data
                uint32_t rgb = (uint32_t) strtol((const char *) &payload[1], NULL, 16);

                // NeoPixels
                for (int i=0; i<NUM_LEDS; i++) {
                  leds[i] = ((rgb >> 16) & 0xFF), ((rgb >> 8) & 0xFF),((rgb >> 0) & 0xFF);
                }
                FastLED.show();
            }

            if(payload[0] == '*') {
                // we get Pixel number
                uint32_t PixelNumber = (uint32_t) strtol((const char *) &payload[1], NULL, 16);
                // NeoPixels
                for (int i=0; i<PixelNumber; i++) {
                  leds[i] = CRGB::Black;
                }
                
                FastLED.show();
            }

if(payload[0] == '$') {
                // we get switch.
                
            }

            break;
    }

}

void setup() {
 delay(2000);          // sanity delay for LEDs
  FastLED.addLeds<LED_TYPE,DATA_PIN,COLOR_ORDER>(leds, NUM_LEDS).setCorrection(DirectSunlight);           // for WS2812 (Neopixel)
  //FastLED.addLeds<LED_TYPE,DATA_PIN,CLK_PIN,COLOR_ORDER>(leds, NUM_LEDS).setCorrection(DirectSunlight); // for APA102 (Dotstar)
  FastLED.setBrightness(BRIGHTNESS);

    
    Serial.begin(115200);
    Serial.println("sync");
    Serial.println("sync");
    Serial.println("sync");

    for(uint8_t t = 4; t > 0; t--) {
        Serial.printf("[SETUP] BOOT WAIT %d...\n", t);
        Serial.flush();
        delay(1000);
    }

  SPIFFS.begin();
  {
    Dir dir = SPIFFS.openDir("/");
    Serial.println("Directory Listing:");
    while (dir.next()) {    
      String fileName = dir.fileName();
      size_t fileSize = dir.fileSize();
      Serial.printf("FS File: %s, size: %s\n", fileName.c_str(), formatBytes(fileSize).c_str());
    }
    Serial.printf("\n");
  }

Serial.print("Setting up WLAN");

  WiFiMulti.addAP(ssid, password);
  while(WiFiMulti.run() != WL_CONNECTED) {
    delay(100);
    Serial.print(".");
  }

  Serial.println("");
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());

  // start webSocket server
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  
  // Set up mDNS responder:
  if (!MDNS.begin(mDNSid)) {
    Serial.println("Error setting up MDNS responder!");
    while(1) { 
      delay(1000);
    }
  }
    Serial.println("mDNS responder started");

  Serial.print("Open http://");
  Serial.print(mDNSid);
  Serial.println(".local/edit to see the file browser");

  //SERVER INIT
  //list directory
  server.on("/list", HTTP_GET, handleFileList);
  //load editor
  server.on("/edit", HTTP_GET, [](){
    if(!handleFileRead("/edit.htm")) server.send(404, "text/plain", "FileNotFound");
  });
  //create file
  server.on("/edit", HTTP_PUT, handleFileCreate);
  //delete file
  server.on("/edit", HTTP_DELETE, handleFileDelete);
  //first callback is called after the request has ended with all parsed arguments
  //second callback handles file uploads at that location
  server.on("/edit", HTTP_POST, [](){ server.send(200, "text/plain", ""); }, handleFileUpload);

  //called when the url is not defined here
  //use it to load content from SPIFFS
  server.onNotFound([](){
    if(!handleFileRead(server.uri()))
      server.send(404, "text/plain", "FileNotFound");
  });

  server.begin();
  Serial.println("HTTP server started");

  // Add service to MDNS
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("ws", "tcp", 81);

FastLED.show();
}

// LED animations
void all_off() {

FastLED.clear();
FastLED.show();
FastLED.delay(1000/FRAMES_PER_SECOND);
}
 
void rainbow()
{
  // FastLED's built-in rainbow generator
  EVERY_N_MILLISECONDS( 20 ) { gHue++; } // slowly cycle the "base color" through the rainbow
  fill_rainbow( leds, NUM_LEDS, gHue, 7);
  show_at_max_brightness_for_power();
  delay_at_max_brightness_for_power(1000/FRAMES_PER_SECOND);
//  FastLED.show();  
//  FastLED.delay(1000/FRAMES_PER_SECOND);
}
 
void rainbowWithGlitter()
{
  // built-in FastLED rainbow, plus some random sparkly glitter
  rainbow();
  addGlitter(80);
}
 
void addGlitter( fract8 chanceOfGlitter)
{
  if( random8() < chanceOfGlitter) {
    leds[ random16(NUM_LEDS) ] += CRGB::White;
  }
}
 
void confetti()
{
  // random colored speckles that blink in and fade smoothly
  fadeToBlackBy( leds, NUM_LEDS, 10);
  int pos = random16(NUM_LEDS);
  leds[pos] += CHSV( gHue + random8(64), 200, 255);
  show_at_max_brightness_for_power();
  delay_at_max_brightness_for_power(1000/FRAMES_PER_SECOND);
//  FastLED.show();  
//  FastLED.delay(1000/FRAMES_PER_SECOND);
}
 
void sinelon()
{
  // a colored dot sweeping back and forth, with fading trails
  fadeToBlackBy( leds, NUM_LEDS, 20);
  int pos = beatsin16(13,0,NUM_LEDS);
  leds[pos] += CHSV( gHue, 255, 192);
  show_at_max_brightness_for_power();
  delay_at_max_brightness_for_power(1000/FRAMES_PER_SECOND);
//  FastLED.show();  
//  FastLED.delay(1000/FRAMES_PER_SECOND);
}
 
void bpm()
{
  // colored stripes pulsing at a defined Beats-Per-Minute (BPM)
  uint8_t BeatsPerMinute = 62;
  CRGBPalette16 palette = PartyColors_p;
  uint8_t beat = beatsin8( BeatsPerMinute, 64, 255);
  for( int i = 0; i < NUM_LEDS; i++) { //9948
    leds[i] = ColorFromPalette(palette, gHue+(i*2), beat-gHue+(i*10));
  }
  show_at_max_brightness_for_power();
  delay_at_max_brightness_for_power(1000/FRAMES_PER_SECOND);
//  FastLED.show();  
//  FastLED.delay(1000/FRAMES_PER_SECOND);
}
 
void juggle() {
  // eight colored dots, weaving in and out of sync with each other
  fadeToBlackBy( leds, NUM_LEDS, 20);
  byte dothue = 0;
  for( int i = 0; i < 8; i++) {
    leds[beatsin16(i+7,0,NUM_LEDS)] |= CHSV(dothue, 200, 255);
    dothue += 32;
  }
  show_at_max_brightness_for_power();
  delay_at_max_brightness_for_power(1000/FRAMES_PER_SECOND);
//  FastLED.show();  
//  FastLED.delay(1000/FRAMES_PER_SECOND);
}

void loop() {
    if (ledMode != 999) {
 
     switch (ledMode) {
      case  1: all_off(); break;
      case  2: rainbow(); break;
      case  3: rainbowWithGlitter(); break;
      case  4: confetti(); break;
      case  5: sinelon(); break;
      case  6: juggle(); break;
      case  7: bpm(); break;
      }
      }
       EVERY_N_MILLISECONDS( 20 ) { gHue++; } // slowly cycle the "base color" through the rainbow
 
    
    webSocket.loop();
    server.handleClient();
}

