/************************
   Version history
   * 0.99.61
     Fix for previous change
   * 0.99.60
     Prevent dryer from alerting more than once every 10 minutes
   * 0.99.50
     Use PushOver instead of MQTT
   * 0.99.15 - Jan 18/2016
     Use MQTT authorization
     Read/Store MQTT config params from EEPROM
     Add MQTT config to Web UI
   * 0.99.14.1 - Jan 15/2016
     Don't alert for washer motion started - reduce alerts by 1/2
     Wait a delay period before announcing washer stopped to see if it changes again
   * 0.99.14 - Jan 12/2016
     Migrate to MQTT for notifications
   * 0.99.13 - Jan 7/2016
     Add /temperature route to just return text/plain current temperature
   * 0.99.12 - Jan 2/2016
     Add DS18B20 for temperature sensing and reporting
     Use SPIFFS filesystem for hosting web assets
   * 0.99.11 - Dec 25/2015
     Save/recall wifi settings from eeprom
   * 0.99.10 - Dec 23/2015
     Save settings in eeprom
     Edit from web UI
   * 0.99.8 - Dec 17/2015
     Include array of samples in JSON output /status
   * 0.99.7 - Dec 17/2015
     Add status output for standard deviation
     Reduce loop delay to 1/2s
     Reduce motion tolerance to 3
     Increase samples to 20
   * 0.99.6 - Dec 17/2015
     Add notification for motion starting in addition to stopping
   * 0.99.5 - Dec 12/2015
     Add status page and handling
 */

#define VERSION "0.99.61"  // version string
#define CODE 0xc0debeef   // EEPROM setup code
//#define DEBUG true      // enable debugging code

#include "Arduino.h"
#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <OneWire.h>
#include <FS.h>
#include <DallasTemperature.h>
#include <DNSServer.h>
#include <WiFiManager.h>
#include "pushover.h"

#define SAMPLES 20  // number of samples to use for standard deviation
#define RELAY 5     // GPIO pin the relay is connected to
#define ONEWIRE 4   // GPIO pin for DS18B20 temp sensor
#define MOTION_DELAY 600000  // wait 5 minutes after last motion stopped before announcing

/*** Function prototypes ***/
float deviation (float x[], int n);
void debug(String, bool);
void readSettings();
void writeSettings();
void printConfig();
String getContentType(String filename);
void handleRoot();
void handleStatus();
void handleTempReq();
void handleUpdate();
void handleNotFound();
void reconnect();

/*** initialize classes ***/
WiFiClient wifiClient;
ESP8266WebServer httpServer(80);
OneWire ow(ONEWIRE);
DallasTemperature DS18B20(&ow);

/* settings structure */
struct settings {
        int code;
        char ver[16];
        int wait;
        float tolerance;
};

settings localConfig;

/*** other global variables ***/
int count = 0;
int curr_relay = 0;
int last_relay = 0;
char pushover_uri[] = "api.pushover.net";
char pushover_token[] = PUSH_TOK;
char pushover_userkey[] = PUSH_KEY;

bool curr_motion = false;
bool last_motion = false;
bool has_moved = false;
bool shouldSaveConfig = false;
float motion_dev = 0.0;
float max_dev = 0.0;
float temp;
float samples[SAMPLES];
unsigned long motion_stopped_time = 0;
unsigned long dryer_alerted = 0;

void saveConfigCallback () {
        Serial.println("Should save config");
        shouldSaveConfig = true;
}

byte pushover(const char *pushovermessage)
{
        int length;
        String message = pushovermessage;

        length = 81 + message.length();

        if(wifiClient.connect(pushover_uri,80))
        {
                wifiClient.println("POST /1/messages.json HTTP/1.1");
                wifiClient.println("Host: api.pushover.net");
                wifiClient.println("Connection: close\r\nContent-Type: application/x-www-form-urlencoded");
                wifiClient.print("Content-Length: ");
                wifiClient.print(length);
                wifiClient.println("\r\n");;
                wifiClient.print("token=");
                wifiClient.print(pushover_token);
                wifiClient.print("&user=");
                wifiClient.print(pushover_userkey);
                wifiClient.print("&message=");
                wifiClient.print(message);
                while(wifiClient.connected())
                {
                        while(wifiClient.available())
                        {
                                char ch = wifiClient.read();
                                Serial.write(ch);
                        }
                }
                wifiClient.stop();
        }
}


/*** Execute once at top of runtime ***/
void setup() {
        pinMode(RELAY, INPUT_PULLUP);

        Serial.begin(9600);
        EEPROM.begin(1024);
        SPIFFS.begin();

        WiFiManager wfman;
        wfman.setSaveConfigCallback(saveConfigCallback);
        wfman.autoConnect("SmartLaundry");
        Serial.println("connected!");

        delay(2000);

        readSettings();

        /* Stuff for ArduinoOTA updates */
        ArduinoOTA.setHostname("smartlaundry");
        ArduinoOTA.onStart([]() {
        });

        ArduinoOTA.onEnd([]() {
        });

        ArduinoOTA.begin();
        /* End OTA setup code */

        httpServer.on("/", handleRoot);
        httpServer.on("/status", handleStatus);
        httpServer.on("/temperature", handleTempReq);
        httpServer.on("/update", handleUpdate);
        httpServer.onNotFound(handleNotFound);
        httpServer.begin();

        last_relay = digitalRead(RELAY);
        delay(localConfig.wait);
	std::string message = std::string("SmartLaundry v") + std::string(VERSION) + std::string(" booted");
        //pushover((char *)"SmartLaundry Booted");
        pushover(message.c_str());
}

/*** execute forever ***/
void loop() {
        if (count + 1 == SAMPLES)
        {
                do {
                        DS18B20.requestTemperatures();
                        temp = DS18B20.getTempCByIndex(0);
                } while (temp == 85.0 || temp == (-127.0));
        }

        // handle OTA update tasks
        ArduinoOTA.handle();

        // handle web requests
        httpServer.handleClient();

        // read state of relay
        curr_relay = digitalRead(RELAY);

        debug("last_relay: ", 0);
        debug(String(last_relay), 1);
        debug("curr_relay: ", 0);
        debug(String(curr_relay), 1);

        // Collect 10 motion samples
        samples[count++] = analogRead(A0);
        if (count == SAMPLES)
        {
                // when we've got 10 calculate standard deviation
                motion_dev = deviation(samples, count);

                // track mex deviation in debug mode
                if (motion_dev > max_dev)
                        max_dev = motion_dev;
                debug("std. dev: ", 0);
                debug(String(motion_dev), 1);
                debug("max. dev: ", 0);
                debug(String(max_dev), 1);

                // if current deviation is larger than preset tolerance, assume that we're moving
                curr_motion = motion_dev > localConfig.tolerance;
                count = 0;
        }

        // If the state of being in motion recently changed and we're currently not moving, signal cycle end
        if (curr_motion != last_motion && !curr_motion)
        {
                motion_stopped_time = millis();
        }

        if (has_moved && !curr_motion && (motion_stopped_time + MOTION_DELAY < millis()))
        {
                pushover((char *)"Washer cycle stopped");
                has_moved = false;
        }

        // If the state of being in motion recently changed and we're currently not moving, signal cycle end
        if (curr_motion != last_motion && curr_motion)
        {
                has_moved = true;
        }

        last_motion = curr_motion;

        // If the state of the relay changed and it's currently open, signal dryer buzzer
        if (last_relay != curr_relay && curr_relay == HIGH && (millis() - dryer_alerted > 600000))
        {
                pushover((char *)"Dryer buzzer sounded");
		dryer_alerted = millis();
        }
        last_relay = curr_relay;

        delay(localConfig.wait); // sleep 1s allowing the CPU to do any background tasks
        yield(); // let the ESP8266 complete any background tasks
}

/*** Function definitions ***/

void debug(String msg, bool newline)
{
#ifdef DEBUG
        if (newline)
        {
                Serial.println(msg);
        } else
        {
                Serial.print(msg);
        }
#endif
}

/* calculate standard deviation */
float deviation (float x[], int n)
{
        float mean = 0.0;
        float sum_deviation = 0.0;

        for (int i = 0; i < n; i++)
        {
                mean += x[i];
        }
        mean = mean / n;

        for (int i = 0; i < n; i++)
        {
                sum_deviation += (x[i] - mean) * (x[i] - mean);
        }

        return sqrt(sum_deviation / n);
}

void readSettings()
{
        debug("reading configuration from eeprom", 1);

        EEPROM.get(0, localConfig);

        if (localConfig.code != CODE)
        {
                debug("settings not found, using defaults", 1);

                localConfig.code = CODE;
                strcpy(localConfig.ver, VERSION);
                localConfig.wait = 500;
                localConfig.tolerance = 5.0f;
                writeSettings();
        }
}

void writeSettings()
{
        debug("writing settings to eeprom", 1);
        printConfig();
        EEPROM.put(0, localConfig);
        EEPROM.end();
}

void printConfig()
{
#ifdef DEBUG
        Serial.println(localConfig.code, HEX);
        Serial.println(localConfig.ver);
        Serial.println(localConfig.wait);
        Serial.println(localConfig.ssid);
        Serial.println(localConfig.pass);
        Serial.println(localConfig.tolerance);
#endif
}

String getContentType(String filename) {
        if (httpServer.hasArg("download")) return "application/octet-stream";
        else if (filename.endsWith(".htm")) return "text/html";
        else if (filename.endsWith(".html")) return "text/html";
        else if (filename.endsWith(".css")) return "text/css";
        else if (filename.endsWith(".js")) return "application/javascript";
        else if (filename.endsWith(".png")) return "image/png";
        else if (filename.endsWith(".gif")) return "image/gif";
        else if (filename.endsWith(".jpg")) return "image/jpeg";
        else if (filename.endsWith(".ico")) return "image/x-icon";
        else if (filename.endsWith(".xml")) return "text/xml";
        else if (filename.endsWith(".pdf")) return "application/x-pdf";
        else if (filename.endsWith(".zip")) return "application/x-zip";
        else if (filename.endsWith(".gz")) return "application/x-gzip";
        return "text/plain";
}

void handleRoot()
{
        String response;

        File f = SPIFFS.open("/index.html", "r");
        if (!f) {
                response = "File not found\n";
        } else
        {
                response = f.readString();
        }
        f.close();

        httpServer.send(200, "text/html", response);
}

void handleStatus()
{
        String relay_status = "closed";
        if (curr_relay == 1)
                relay_status = "open";
        String json = "{";
        json += "\"relay\": \"" + relay_status + "\",";
        json += "\"motion\": \"" + String(samples[count]) + "\",";
        json += "\"std_dev\": \"" + String(motion_dev) + "\",";
        json += "\"temperature\": \"" + String(temp) + "\",";
        json += "\"configuration\": {";
        json += "\"delay\":" + String(localConfig.wait) + ",\n";
        json += "\"sample_count\":" + String(SAMPLES) + ",\n";
        json += "\"version\":\"" + String(VERSION) + "\",\n";
        json += "\"tolerance\":\"" + String(localConfig.tolerance) + "\"},\n";
        json += "\"samples\": [";
        for (int i = 0; i < SAMPLES; i++)
        {
                json += "\"" + String(samples[i]) + "\"";
                if (i + 1 < SAMPLES)
                        json += ",";
        }
        json += "]}";

        httpServer.send(200, "application/json", json);
}

void handleTempReq()
{
        String temperature = String(temp);
        httpServer.send(200, "text/plain", temperature);
}

void handleUpdate()
{
        String message = "";
        for (uint8_t i = 0; i < httpServer.args(); i++)
        {
                if (httpServer.argName(i) == "wait")
                {
                        int w = httpServer.arg(i).toInt();
                        if (w <= 250)
                        {
                                message += "Delay too small, or invalid.\n";
                        } else {
                                localConfig.wait = w;
                        }
                }

                if (httpServer.argName(i) == "tolerance")
                {
                        float tol = httpServer.arg(i).toFloat();
                        if (tol <= 1)
                        {
                                message += "Motion tolerance too low, or invalid.\n";
                        } else {
                                localConfig.tolerance = tol;
                        }
                }
        }

        writeSettings();
        message += "Update complete.";
        httpServer.send(200, "text/plain", message);
}

void handleNotFound()
{
        String response;
        String request = httpServer.uri();
        String mimetype = "text/html";
        File f;

        if (SPIFFS.exists(request))
        {
                File f = SPIFFS.open(request, "r");
                if (!f) {
                        response = "Requested file: \"" + request + "\" exists, but there was a problem loading it\n";
                        httpServer.send(503, "text/plain", response);
                } else
                {
                        mimetype = getContentType(request);
                        httpServer.streamFile(f, mimetype);
                }
                f.close();
        } else
        {
                String response = "File Not Found\n\n";
                response += "URI: ";
                response += httpServer.uri();
                response += "\nMethod: ";
                response += (httpServer.method() == HTTP_GET) ? "GET" : "POST";
                response += "\nArguments: ";
                response += httpServer.args();
                response += "\n";
                for (uint8_t i = 0; i < httpServer.args(); i++) {
                        response += " " + httpServer.argName(i) + ": " + httpServer.arg(i) + "\n";
                }
                httpServer.send(404, "text/plain", response);
        }
}
