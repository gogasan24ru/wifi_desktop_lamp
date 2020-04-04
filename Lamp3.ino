#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <Kelvin2RGB.h>
#include <ESP8266Ping.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
//#include <WebSocketsServer.h>
#include <Hash.h>
#include <WiFiUdp.h>
#include "ws2812_i2s.h"
#include "FS.h"
#include <EEPROM.h>

#define DBG_OUTPUT_PORT Serial

// Set to the number of LEDs in your LED strip
#define NUM_LEDS 144
// Maximum number of packets to hold in the buffer. Don't change this.
#define BUFFER_LEN 1024
// Toggles FPS output (1 = print FPS over serial, 0 = disable output)
#define PRINT_FPS 0

// Wifi and socket settings
const char* host = "Lamp1";
const char* ssid = "YOUR_NETWORK_SSID";
const char* password = "YOUR_NETWORK_PASSWORD";

int pingFailures = 0;


ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;
bool rise = true; //used for offline brightness control

byte hb_counter = 0;
bool rising = true;
const int ledPin =  14;
const int hwBtn =  0;
int ledState = LOW;
unsigned long previousMillis = 0;
long interval = 5000;
int udpKeepAlive = 0;
int udpTimeout = 10 * 1000;


#define sysVersion "006"
struct stateStruct {
  char ver[4];
  byte r;
  byte g;
  byte b;
  bool operational;
  byte brightness;
  int temperature;
  bool udpon;
  int udpport;
  Pixel_t pixels[NUM_LEDS];
  uint8_t pingMaxFailures;
  uint8_t udpTimeout;

  char wwwu[32];
  char wwwp[32];

  bool udpParseModeNew;
} state =
{
  sysVersion,   		    //ver
  100,          		    //r
  100,          		    //g
  100,          		    //b
  true,         		    //operational
  100,          		    //brightness
  0,            		    //temperature
  false,        		    //udpon
  7777,         		    //udpport
  {},           		    //pixels
  5,        	        	//pingMaxFailures
  10 * 1000,  			    //udpTimeout
  "your_endpoints_login",   //wwwu
  "your_endpoints_password",//wwwp

  false,
};



unsigned int localPort = 7777; //UDP SERVER PORT for audio visualisations
char packetBuffer[BUFFER_LEN];

// LED strip
static WS2812 ledstrip;
//static Pixel_t pixels[NUM_LEDS];
WiFiUDP port;


int status = WL_IDLE_STATUS;     // the Wifi radio's status

template <class T> int EEPROM_writeAnything(int ee, const T& value) {
  const byte* p = (const byte*)(const void*)&value;
  unsigned int i;
  for (i = 0; i < sizeof(value); i++)
    EEPROM.write(ee++, *p++);
  return i;
}

template <class T> int EEPROM_readAnything(int ee, T& value) {
  byte* p = (byte*)(void*)&value;
  unsigned int i;
  for (i = 0; i < sizeof(value); i++)
    *p++ = EEPROM.read(ee++);
  return i;
}



File fsUploadFile;
String getContentType(String filename) {
  if (httpServer.hasArg("download")) {
    return "application/octet-stream";
  } else if (filename.endsWith(".htm")) {
    return "text/html";
  } else if (filename.endsWith(".html")) {
    return "text/html";
  } else if (filename.endsWith(".css")) {
    return "text/css";
  } else if (filename.endsWith(".js")) {
    return "application/javascript";
  } else if (filename.endsWith(".png")) {
    return "image/png";
  } else if (filename.endsWith(".gif")) {
    return "image/gif";
  } else if (filename.endsWith(".jpg")) {
    return "image/jpeg";
  } else if (filename.endsWith(".ico")) {
    return "image/x-icon";
  } else if (filename.endsWith(".xml")) {
    return "text/xml";
  } else if (filename.endsWith(".pdf")) {
    return "application/x-pdf";
  } else if (filename.endsWith(".zip")) {
    return "application/x-zip";
  } else if (filename.endsWith(".gz")) {
    return "application/x-gzip";
  }
  return "text/plain";
}

void handlePropertyUpdate() {
  if (httpServer.hasArg("udpParseModeNew"))
  {
    state.udpParseModeNew = httpServer.arg("udpParseModeNew") == "on";
    httpServer.sendHeader("Location", String("/"), true);
    httpServer.send(302, "text/plain", String(state.udpParseModeNew));
    return;
  }

  httpServer.send(500, "text/plain", "BAD ARGS - no match");
}

void handleFileList() {
  if (!httpServer.hasArg("dir")) {
    httpServer.send(500, "text/plain", "BAD ARGS");
    return;
  }

  String path = httpServer.arg("dir");
  DBG_OUTPUT_PORT.println("handleFileList: " + path);
  Dir dir = SPIFFS.openDir(path);
  path = String();

  String output = "[";
  while (dir.next()) {
    File entry = dir.openFile("r");
    if (output != "[") {
      output += ',';
    }
    bool isDir = false;
    output += "{\"type\":\"";
    output += (isDir) ? "dir" : "file";
    output += "\",\"name\":\"";
    output += String(entry.name()).substring(1);
    output += "\"}";
    entry.close();
  }

  output += "]";
  httpServer.send(200, "text/json", output);
}
bool handleFileRead(String path) {
  DBG_OUTPUT_PORT.println("handleFileRead: " + path);
  if (path.endsWith("/")) {
    path += "index.html";
  }
  String contentType = getContentType(path);
  String pathWithGz = path + ".gz";
  if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) {
    if (SPIFFS.exists(pathWithGz)) {
      path += ".gz";
    }
    File file = SPIFFS.open(path, "r");
    httpServer.sendHeader("Cache-Control", "public, max-age=31536000");
    httpServer.streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}
void handleFileDelete() {
  if (httpServer.args() == 0) {
    return httpServer.send(500, "text/plain", "BAD ARGS");
  }
  String path = httpServer.arg(0);
  DBG_OUTPUT_PORT.println("handleFileDelete: " + path);
  if (path == "/") {
    return httpServer.send(500, "text/plain", "BAD PATH");
  }
  if (!SPIFFS.exists(path)) {
    return httpServer.send(404, "text/plain", "FileNotFound");
  }
  SPIFFS.remove(path);
  httpServer.send(200, "text/plain", "");
  path = String();
}
void handleFileUpload() {
  if (httpServer.uri() != "/edit") {
    return;
  }
  HTTPUpload& upload = httpServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) {
      filename = "/" + filename;
    }
    DBG_OUTPUT_PORT.print("handleFileUpload Name: "); DBG_OUTPUT_PORT.println(filename);
    fsUploadFile = SPIFFS.open(filename, "w");
    filename = String();
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    //DBG_OUTPUT_PORT.print("handleFileUpload Data: "); DBG_OUTPUT_PORT.println(upload.currentSize);
    if (fsUploadFile) {
      fsUploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (fsUploadFile) {
      fsUploadFile.close();
    }
    DBG_OUTPUT_PORT.print("handleFileUpload Size: "); DBG_OUTPUT_PORT.println(upload.totalSize);
  }
}



void saveState() {
  EEPROM_writeAnything(0, state);
  EEPROM.commit();
}

int applyBrightness(int data) {
  return map(data, 0, 255, 0, state.brightness);
}

void paint(int r, int g, int b, bool save = true) {
  for (int i = 0; i < NUM_LEDS; i++) {
    state.pixels[i].R = applyBrightness(r);
    state.pixels[i].G = applyBrightness(g);
    state.pixels[i].B = applyBrightness(b);
    //.setPixelColor(i, pixels.Color(r,g,b)); // Moderately bright green color.
    yield();
  }

  ledstrip.show(state.pixels);
  //pixels.setBrightness(state.brightness);
  //pixels.show();
  if (save)
  {
    state.r = r;
    state.g = g;
    state.b = b;
    state.operational = true;
  }
}
void paintRange(int r, int g, int b, int start = 0, int end = NUM_LEDS) {

  for (int i = start; i < end; i++) {
    state.pixels[i].R = applyBrightness(r);
    state.pixels[i].G = applyBrightness(g);
    state.pixels[i].B = applyBrightness(b);
    //pixels.setPixelColor(i, pixels.Color(r,g,b)); // Moderately bright green color.
    yield();
  }
  //pixels.setBrightness(state.brightness);
  //pixels.show();
  ledstrip.show(state.pixels);
}

void switch_l(bool affectState = true) {
  if (affectState)
    state.operational = !state.operational;
  if (state.operational) {
    paint(state.r, state.g, state.b, false);
  } else
  {
    paint(0, 0, 0, false);
    int br = state.brightness;
    state.brightness = 10;
    paintRange(0, 64, 0, 0, 1);
    state.brightness = br;
  }
  if (affectState)
    saveState();
}

void heartBeat() {
  yield();
  ESP.wdtFeed();
  if (pingFailures > state.pingMaxFailures)
  {
    paintRange(150, 0, 0, 0, 10);
    delay(300);
    ESP.restart();
  }

  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= interval) {

    if (!Ping.ping(WiFi.gatewayIP(), 1)) {
      pingFailures++;
      paintRange(150, 0, 0, 0, pingFailures);
    }
    else
    {
      if(state.operational)
      {
        paintRange(state.r, state.g, state.b, 0, pingFailures);        
      }
      else
      {
        paint(0, 0, 0, false);
      }
      pingFailures = 0;
    }
    // save the last time you blinked the LED
    previousMillis = currentMillis;
    if (hb_counter >= 255) rising = false;
    if (hb_counter <= 0) rising = true;
    hb_counter += 10 * (rising ? 1 : -1);
    if ((WiFi.waitForConnectResult() != WL_CONNECTED))
    {
      //      pixels.setPixelColor(0, pixels.Color(0,0,hb_counter));
      //      pixels.setPixelColor(NUMPIXELS-1, pixels.Color(0,0,hb_counter));
    }
    else
    {
      //      pixels.setPixelColor(0, pixels.Color(hb_counter,0,0));
      //      pixels.setPixelColor(NUMPIXELS-1, pixels.Color(hb_counter,0,0));
    }
    //pixels.show();
    // set the LED with the ledState of the variable:
  }
  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    WiFi.begin(ssid, password);
    Serial.println("WiFi failed, retrying.");
  }

}



void setup() {

  pinMode(hwBtn, INPUT_PULLUP);
  EEPROM.begin(sizeof(state));


  stateStruct inMemory;
  EEPROM_readAnything(0, inMemory);
  if (String(inMemory.ver).equals(String(sysVersion)))
  {
    EEPROM_readAnything(0, state);
  }
  else
  {
    saveState();
  }
  switch_l(false);


  SPIFFS.begin();
  Serial.begin(115200);
  Serial.println("** Scan Networks **");
  byte numSsid = WiFi.scanNetworks();

  Serial.print("SSID List:");
  Serial.println(numSsid);

  WiFi.mode(WIFI_AP_STA);
  //WiFi.config(ip, dns, gateway, subnet);
  WiFi.begin(ssid, password);
  Serial.println("Connecting.... \nStatuses:");
  // Connect to wifi and print the IP address over serial
  while (status != WL_CONNECTED) {
    delay(500);
    status = WiFi.status();
    switch (status)
    {
WL_CONNECTED: Serial.println("OK"); break;
WL_NO_SHIELD: Serial.println("NO SHIELD???"); break;
WL_IDLE_STATUS: Serial.println("Idle"); break;
WL_NO_SSID_AVAIL: Serial.println("NO AP!"); break;
WL_SCAN_COMPLETED: Serial.println("SCAN COMPLETED C:"); break;
WL_CONNECT_FAILED: Serial.println("OH NO FAILED!"); break;
WL_CONNECTION_LOST: Serial.println("CONN LOST!"); break;
WL_DISCONNECTED: Serial.println("DISCONNECTED!"); break;

      default: Serial.println("UNKNOWN;");
    }
    //break;
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  WiFi.softAP(state.wwwu, state.wwwp);
  //[{"type":"file","name":"iro.min.js.gz"},{"type":"file","name":"jquery-3.3.1.min.js.gz"},{"type":"file","name":"styles.css.gz"}]

  httpUpdater.setup(&httpServer, "/fwupdate", state.wwwu, state.wwwp);

  httpServer.on("/edit", HTTP_POST, []() {
    httpServer.send(200, "text/plain", "");
  }, handleFileUpload);
  httpServer.on("/edit", HTTP_DELETE, handleFileDelete);
  httpServer.on("/list", HTTP_GET, handleFileList);
  httpServer.on("/prop", HTTP_POST, handlePropertyUpdate);

  httpServer.on("/status", []() {
    StaticJsonBuffer<200> jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();
    root["uptime"] = millis() / 1000;
    root["temperature"] = state.temperature;
    root["state"] = state.operational;
    root["udp"] = state.udpon;
    root["brightness"] = state.brightness;
    root["signal"] = WiFi.RSSI();
    root["r"] = state.r;
    root["g"] = state.g;
    root["b"] = state.b;
    
    root["udpParseModeNew"] = state.udpParseModeNew;
    root["version"] = state.ver;
    root["udpport"] = state.udpport;
    root["pingMaxFailures"] = state.pingMaxFailures;
    root["udpTimeout"] = state.udpTimeout;  

    String json = "";
    root.printTo(json);
    httpServer.send(200, "application/json", json);
  });
  httpServer.on("/setColor", []() {
#ifdef HTTP_BASIC
    if (!httpServer.authenticate(state.wwwu, state.wwwp)) {
      return httpServer.requestAuthentication();
    }
#endif
    int R = String(httpServer.arg("R")).toInt();
    int G = String(httpServer.arg("G")).toInt();
    int B = String(httpServer.arg("B")).toInt();
    paint(R, G, B);
    state.temperature = 0;
    httpServer.sendHeader("Location", String("/"), true);
    httpServer.send(302, "text/plain", "OK");
    state.r = R;
    state.g = G;
    state.b = B;
    state.operational = true;
    saveState();
  });

  httpServer.on("/setBrightness", []() {
#ifdef HTTP_BASIC
    if (!httpServer.authenticate(state.wwwu, state.wwwp)) {
      return httpServer.requestAuthentication();
    }
#endif
    int bri = String(httpServer.arg("bri")).toInt();
    state.brightness = bri;
    paint(state.r, state.g, state.b);
    httpServer.sendHeader("Location", String("/"), true);
    httpServer.send(302, "text/plain", "OK");
    saveState();
  });


  httpServer.on("/halt", []() {
#ifdef HTTP_BASIC
    if (!httpServer.authenticate(state.wwwu, state.wwwp)) {
      return httpServer.requestAuthentication();
    }
#endif
    paint(0, 0, 0);
    state.operational = false;
    httpServer.sendHeader("Location", String("/"), true);
    httpServer.send(302, "text/plain", "OK");
    saveState();
  });
  httpServer.on("/switch", []() {
#ifdef HTTP_BASIC
    if (!httpServer.authenticate(state.wwwu, state.wwwp)) {
      return httpServer.requestAuthentication();
    }
#endif
    switch_l();
    httpServer.sendHeader("Location", String("/"), true);
    httpServer.send(302, "text/plain", "OK");
  });
  httpServer.on("/switchUdp", []() {
#ifdef HTTP_BASIC
    if (!httpServer.authenticate(state.wwwu, state.wwwp)) {
      return httpServer.requestAuthentication();
    }
#endif
    udpKeepAlive = millis();
    state.udpon = !state.udpon;
    //state.operational=!state.udpon;
    httpServer.sendHeader("Location", String("/"), true);
    httpServer.send(302, "text/plain", "OK");
  });
  httpServer.on("/setColorTemp", []() {
#ifdef HTTP_BASIC
    if (!httpServer.authenticate(state.wwwu, state.wwwp)) {
      return httpServer.requestAuthentication();
    }
#endif
    int temperature = String(httpServer.arg("t")).toInt();
    state.temperature = temperature;
    Kelvin2RGB kel(temperature, 100);
    paint(kel.Red, kel.Green, kel.Blue);
    httpServer.sendHeader("Location", String("/"), true);
    httpServer.send(302, "text/plain", "OK");
    saveState();
  });
  httpServer.on("/reboot", []() {
#ifdef HTTP_BASIC
    if (!httpServer.authenticate(state.wwwu, state.wwwp)) {
      return httpServer.requestAuthentication();
    }
#endif
    saveState();
    ESP.restart();
    httpServer.send(200, "text/plain", "Login OK");
  });
  httpServer.on("/settings.html", []() {//TODO "/secure/FILENAME"?
    if (!httpServer.authenticate(state.wwwu, state.wwwp)) {
      return httpServer.requestAuthentication();
    }
    if (!handleFileRead(httpServer.uri())) {
      httpServer.send(404, "text/plain", "FileNotFound");
    }
  });

  httpServer.onNotFound([]() {
    if (!handleFileRead(httpServer.uri())) {
      httpServer.send(404, "text/plain", "FileNotFound");
    }
  });

  httpServer.begin();
  port.begin(localPort);
  ledstrip.init(NUM_LEDS);


}

uint8_t N = 0;
#if PRINT_FPS
uint16_t fpsCounter = 0;
uint32_t secondTimer = 0;
#endif



void loop() {
  //  if ((state.udpon)&&((millis() - udpKeepAlive)>state.udpTimeout)) {
  if ((state.udpon) && ((millis() - udpKeepAlive) > udpTimeout)) {

    Serial.println("Disabling udp mode due timeout: " + String(state.udpTimeout));
    state.udpon = false;
    //state.operational=true;
    switch_l(false);
  }

  if (digitalRead(hwBtn) == LOW)
  {
    delay(100);
    bool switch_todo = false;
    if (digitalRead(hwBtn) == LOW) {
      switch_todo = true;
      state.udpon = false;
    }
    delay(300);
    while ((digitalRead(hwBtn) == LOW) && state.operational)
    {
      switch_todo = false;
      state.brightness += (rise ? 1 : -1);
      if (state.brightness >= 255)  rise = false;
      if (state.brightness <= 0   ) rise = true;
      switch_l(false);
      delay(3);
    }
    if (switch_todo) switch_l();
  }
  if (!state.udpon) {
    heartBeat();
    httpServer.handleClient();
  }

  if (state.udpParseModeNew) {// for https://github.com/segfault16/modular-led-controller-workstation
    uint16_t packetSize = port.parsePacket();

    // Prevent a crash and buffer overflow
    if (packetSize > BUFFER_LEN) {
      return;
    }

    if (packetSize) {
      uint16_t len = port.read(packetBuffer, BUFFER_LEN);

      // Decode byte sequence and display on LED strip
      N = 0;
      if (len > 4)udpKeepAlive = millis();
      if (state.udpon)
        for (uint16_t i = 0; i < len; i += 3) {
          if (N >= NUM_LEDS) {
            return;
          }
          packetBuffer[len] = 0;
          state.pixels[N].R = (uint8_t)packetBuffer[i + 0];
          state.pixels[N].G = (uint8_t)packetBuffer[i + 1];
          state.pixels[N].B = (uint8_t)packetBuffer[i + 2];
          N += 1;
        }
    }
    if (state.udpon)ledstrip.show(state.pixels);

  } else {//for https://github.com/scottlawsonbc/audio-reactive-led-strip

    // Read data over socket
    int packetSize = port.parsePacket();
    // If packets have been received, interpret the command
    if (packetSize) {
      int len = port.read(packetBuffer, BUFFER_LEN);
      if (len > 4)udpKeepAlive = millis();
      if (state.udpon) {
        for (int i = 0; i < len; i += 4) {
          packetBuffer[len] = 0;
          N = packetBuffer[i];
          state.pixels[N].R = (uint8_t)packetBuffer[i + 1];
          state.pixels[N].G = (uint8_t)packetBuffer[i + 2];
          state.pixels[N].B = (uint8_t)packetBuffer[i + 3];
        }
        ledstrip.show(state.pixels);
#if PRINT_FPS
        fpsCounter++;
#endif
      }
    }
#if PRINT_FPS
    if (millis() - secondTimer >= 1000U) {
      secondTimer = millis();
      Serial.printf("FPS: %d\n", fpsCounter);
      fpsCounter = 0;
    }
#endif
  }
  //else port.flush();


}
