#include "secrets.h"  // Enthält die WLAN-Anmeldedaten
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Adafruit_ThinkInk.h>
#include <time.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>

ThinkInk_290_Tricolor_Z10 display(D0, D1, D3, -1, D2);
ESP8266WiFiMulti WiFiMulti;

// Definition der Haltestellen-IDs
#define STOP_ID1  "7001103"    // Knielinger Allee
#define STOP_ID2  "7000521"    // Kußmaulstr
#define LIMIT "6"              // Anzahl der Abfahrten, die abgefragt werden sollen

// Display-Konstanten
#define TOP 15
#define SKIP 18
#define COL0_WIDTH 36
#include "FreeSansBold9pt8b.h"

// Struktur zur Speicherung der Abfahrtsinformationen
struct DepartureInfo {
  String route;
  String direction;
  String time;
};

// Struktur zur Speicherung der Haltestelleninformationen
struct StopInfo {
  String stopName;
  std::vector<DepartureInfo> departures;
};

// Funktion zur Konvertierung von UTF8 zu Extended ASCII
byte utf8ascii(byte ascii) {
  static byte c1;
  if (ascii < 128) {
    c1 = 0;
    return ascii;
  }
  byte last = c1;
  c1 = ascii;
  switch (last) {
    case 0xC2: return ascii;
    case 0xC3: return (ascii | 0xC0);
    case 0x82: if(ascii == 0xAC) return(0x80);
  }
  return 0;
}

// Funktion zur Konvertierung eines UTF8-Strings zu Extended ASCII
String utf8ascii(String s) {
  String r = "";
  char c;
  for (int i = 0; i < s.length(); i++) {
    c = utf8ascii(s.charAt(i));
    if (c != 0) r += c;
  }
  return r;
}

// Funktion zur Berechnung der Breite eines Strings in Pixeln
uint16_t get_dsp_length(String str) {
  int16_t x, y;
  uint16_t w, h;
  display.getTextBounds(utf8ascii(str), 0, 0, &x, &y, &w, &h);
  return w;
}

// Funktion zum Parsen der Zeitinformationen
void parse_time(struct tm *timeinfo, const JsonObject &obj) {
  memset(timeinfo, 0, sizeof(struct tm));
  if(obj.containsKey("year")) timeinfo->tm_year = atoi(obj["year"]) - 1900;
  if(obj.containsKey("month")) timeinfo->tm_mon = atoi(obj["month"]) - 1;
  if(obj.containsKey("day")) timeinfo->tm_mday = atoi(obj["day"]);
  if(obj.containsKey("hour")) timeinfo->tm_hour = atoi(obj["hour"]);
  if(obj.containsKey("minute")) timeinfo->tm_min = atoi(obj["minute"]);
}

// Funktion zum Abrufen und Parsen der Daten für eine Haltestelle
void fetchAndParseData(const char* stopId, StopInfo& stopInfo) {
  std::unique_ptr<BearSSL::WiFiClientSecure>client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  HTTPClient https;
  Serial.print("[HTTPS] begin...\n");
  String url = "https://projekte.kvv-efa.de/sl3-alone/XSLT_DM_REQUEST?outputFormat=JSON&coordOutputFormat=WGS84[dd.ddddd]&depType=stopEvents&locationServerActive=1&mode=direct&name_dm=";
  url += stopId;
  url += "&type_dm=stop&useOnlyStops=1&useRealtime=1&limit=" LIMIT;
  if (https.begin(*client, url)) {
    Serial.print("[HTTPS] GET...");
    int httpCode = https.GET();
    if (httpCode > 0) {
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        parse_reply(https.getStream(), stopInfo);
      }
    } else {
      Serial.printf(" failed, error: %s\n", https.errorToString(httpCode).c_str());
    }
    https.end();
  } else {
    Serial.printf("[HTTPS] Unable to connect\n");
  }
}

// Funktion zum Parsen der Antwort von der KVV-API
void parse_reply(Stream &payload, StopInfo& stopInfo) {
  DynamicJsonDocument doc(4096);
  StaticJsonDocument<300> filter;
  filter["dateTime"] = true;
  filter["dm"]["points"]["point"]["name"] = true;
  filter["departureList"][0]["countdown"] = true;
  filter["departureList"][0]["realDateTime"]["hour"] = true;
  filter["departureList"][0]["realDateTime"]["minute"] = true;
  filter["departureList"][0]["dateTime"]["hour"] = true;
  filter["departureList"][0]["dateTime"]["minute"] = true;
  filter["departureList"][0]["servingLine"]["direction"] = true;
  filter["departureList"][0]["servingLine"]["symbol"] = true;

  DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return;
  }

  JsonObject obj = doc.as<JsonObject>();
  // Get stop name
  const char *stopName = obj["dm"]["points"]["point"]["name"];
  const char *c = stopName;
  for(int i = 0; i < strlen(c); i++)
    if(c[i] == ',') {
      for(i++; c[i] == ' '; i++);
      stopName = c + i;
    }
  stopInfo.stopName = String(stopName);

  // Parse time from reply into timeinfo
  struct tm timeinfo;
  parse_time(&timeinfo, obj["dateTime"]);

  // Create a list for the indices ordered by countdown value
  int order[obj["departureList"].size()];
  for (size_t i = 0; i < obj["departureList"].size(); i++) order[i] = i;
  for (size_t i = 0; i < obj["departureList"].size() - 1; i++) {
    for (size_t j = i + 1; j < obj["departureList"].size(); j++) {
      int ci = obj["departureList"][order[i]]["countdown"];
      int cj = obj["departureList"][order[j]]["countdown"];
      if (ci > cj) {
        int temp = order[i];
        order[i] = order[j];
        order[j] = temp;
      }
    }
  }

  for(int i = 0; i < obj["departureList"].size(); i++) {
    JsonObject nobj = obj["departureList"][i];
    const char *direction = nobj["servingLine"]["direction"];
    char *c = (char*)direction;
    while(*c && *c != '>') c++;
    if(*c) {
      c--;
      while(*c == ' ') c--;
      c[1] = '\0';
    }
    String destination(direction);
    const char *route = nobj["servingLine"]["symbol"];
    int countdown = atoi(nobj["countdown"]);
    struct tm deptime;
    if(nobj.containsKey("realDateTime")) parse_time(&deptime, nobj["realDateTime"]);
    else parse_time(&deptime, nobj["dateTime"]);
    char time[8];
    if(countdown <= 0) strcpy(time, "sofort");
    else if(countdown < 10) sprintf(time, "%d min", countdown);
    else sprintf(time, "%d:%02d", deptime.tm_hour, deptime.tm_min);

    Serial.printf("[%s] %s %s\n", route, direction, time);

    DepartureInfo info;
    info.route = String(route);
    info.direction = destination;
    info.time = String(time);
    stopInfo.departures.push_back(info);
  }
}

// Setup-Funktion
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  Serial.println();
  Serial.println();
  Serial.println();
  WiFi.mode(WIFI_STA);
  WiFiMulti.addAP(SECRET_WIFI_SSID, SECRET_WIFI_PASSWORD);  // Verwendung der Secrets
  while((WiFiMulti.run() != WL_CONNECTED)) {
    Serial.print('.');
    digitalWrite(LED_BUILTIN, HIGH);
    delay(50);
    digitalWrite(LED_BUILTIN, LOW);
    delay(50);
  }

  StopInfo stopInfo1, stopInfo2;
  fetchAndParseData(STOP_ID1, stopInfo1);
  fetchAndParseData(STOP_ID2, stopInfo2);

  // Anzeige vorbereiten
  display.begin(THINKINK_TRICOLOR);
  display.setFont(&FreeSansBold9pt8b);
  display.clearBuffer();
  display.setTextSize(1);

  // Kopfzeile mit Haltestellennamen
  display.fillRect(0, 0, 296, SKIP, EPD_BLACK);
  String combinedStopName = stopInfo1.stopName + " & " + stopInfo2.stopName;
  display.setTextColor(EPD_WHITE);
  display.setCursor(COL0_WIDTH+2, TOP);
  display.print(utf8ascii(combinedStopName));

  // Zeitstempel anzeigen
  char timeStamp[15];
  struct tm timeinfo;
  time_t now;
  time(&now);
  localtime_r(&now, &timeinfo);
  sprintf(timeStamp, "%d.%d.%02d %d:%02d",
    timeinfo.tm_mday, timeinfo.tm_mon+1, timeinfo.tm_year-100,
    timeinfo.tm_hour, timeinfo.tm_min);
  uint16_t x, y, w, h;
  display.getTextBounds(timeStamp, 0, 0, &x, &y, &w, &h);
  display.setTextColor(EPD_WHITE);
  display.setCursor(295-w-2, TOP);
  display.print(timeStamp);

  // Kombinierte Abfahrten anzeigen
  int maxDepartures = 6;
  int count = 0;
  int i1 = 0, i2 = 0;

  while (count < maxDepartures && (i1 < stopInfo1.departures.size() || i2 < stopInfo2.departures.size())) {
    if (i1 < stopInfo1.departures.size()) {
      DepartureInfo info = stopInfo1.departures[i1];
      String stopPrefix = stopInfo1.stopName.substring(0, 3) + ": ";

      uint16_t w, dw;
      display.fillRect(0, TOP+SKIP*count+5, COL0_WIDTH, SKIP-2, EPD_RED);
      display.setTextColor(EPD_WHITE);

      // Haltestellenpräfix anzeigen
      display.setCursor(2, TOP+SKIP+SKIP*count);
      display.print(stopPrefix);

      // Liniennummer anzeigen
      w = get_dsp_length(info.route);
      display.setCursor(COL0_WIDTH/2 - w/2 + 30, TOP+SKIP+SKIP*count);
      display.print(info.route);

      // Richtung und Zeit anzeigen
      display.setTextColor(EPD_BLACK, EPD_WHITE);
      String displayText = info.direction + " " + info.time;
      dw = get_dsp_length(displayText);
      w = get_dsp_length(info.time);
      if(COL0_WIDTH + 30 + dw >= 295 - 2) {
        String shortenedDir = info.direction;
        shortenedDir.concat("...");
        dw = get_dsp_length(shortenedDir + " " + info.time);
        while(COL0_WIDTH + 30 + dw >= 295 - 2) {
          if(shortenedDir.length() <= 4) break;
          shortenedDir = shortenedDir.substring(0, shortenedDir.length()-4);
          shortenedDir.concat("...");
          dw = get_dsp_length(shortenedDir + " " + info.time);
        }
        displayText = shortenedDir + " " + info.time;
      }
      display.setCursor(COL0_WIDTH + 30, TOP+SKIP+SKIP*count);
      display.print(utf8ascii(displayText));

      i1++;
      count++;
    }

    if (count < maxDepartures && i2 < stopInfo2.departures.size()) {
      DepartureInfo info = stopInfo2.departures[i2];
      String stopPrefix = stopInfo2.stopName.substring(0, 3) + ": ";

      uint16_t w, dw;
      display.fillRect(0, TOP+SKIP*count+5, COL0_WIDTH, SKIP-2, EPD_RED);
      display.setTextColor(EPD_WHITE);

      display.setCursor(2, TOP+SKIP+SKIP*count);
      display.print(stopPrefix);

      w = get_dsp_length(info.route);
      display.setCursor(COL0_WIDTH/2 - w/2 + 30, TOP+SKIP+SKIP*count);
      display.print(info.route);

      display.setTextColor(EPD_BLACK, EPD_WHITE);
      String displayText = info.direction + " " + info.time;
      dw = get_dsp_length(displayText);
      w = get_dsp_length(info.time);
      if(COL0_WIDTH + 30 + dw >= 295 - 2) {
        String shortenedDir = info.direction;
        shortenedDir.concat("...");
        dw = get_dsp_length(shortenedDir + " " + info.time);
        while(COL0_WIDTH + 30 + dw >= 295 - 2) {
          if(shortenedDir.length() <= 4) break;
          shortenedDir = shortenedDir.substring(0, shortenedDir.length()-4);
          shortenedDir.concat("...");
          dw = get_dsp_length(shortenedDir + " " + info.time);
        }
        displayText = shortenedDir + " " + info.time;
      }
      display.setCursor(COL0_WIDTH + 30, TOP+SKIP+SKIP*count);
      display.print(utf8ascii(displayText));

      i2++;
      count++;
    }
  }

  display.display();
  Serial.println("Going to sleep ...");
  ESP.deepSleep(0);
}

void loop() {
  Serial.printf("loop() should never be reached!\n");
  delay(1000);
}
