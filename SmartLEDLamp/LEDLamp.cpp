#include <Arduino.h>
#include <Esp.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <HardwareSerial.h>
#include <include/wl_definitions.h>
#include <IPAddress.h>
#include <pins_arduino.h>
#include <cstdint>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include "defines.h"
#ifdef IR_ENABLE
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>
#endif // IR_ENABLE
#include <FS.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <WebSocketsServer.h>

#include "Log.h"
#include "LEDMatrix.h"
#include "App.h"
#include "VisualizerApp.h"
#include "FadeAndScrollVisualizer.h"
#include "FireVisualizer.h"
#include "DemoReelVisualizer.h"
#include "VUMeterVisualizer.h"
#include "NoiseWithPaletteVisualizer.h"
#include "Runnable.h"
#include "TurnOnRunnable.h"
#include "TurnOffRunnable.h"

#ifdef MQTT_ENABLE
#include <PubSubClient.h>

WiFiClient mqttWiFiClient;
PubSubClient mqttClient(mqttWiFiClient);
#endif // MQTT_ENABLE

#ifdef ARTNET_ENABLE
#include <ArtnetWifi.h>

ArtnetWifi artnet;
#endif // ARTNET_ENABLE

ESP8266WebServer server(80);

WebSocketsServer webSocket = WebSocketsServer(81);
uint8_t socketNumber = 0;
unsigned long messageNumber;

long lastMatrixRefreshMs = 0;

LEDMatrix matrix(LEDS_WIDTH, LEDS_HEIGHT);

TurnOnRunnable turnOnRunnable(&matrix);
TurnOffRunnable turnOffRunnable(&matrix);

#ifdef IR_ENABLE
IRrecv irrecv(PIN_RECV_IR);

long irCodes[] = { 0xff3ac5, 0xffba45, 0xff827d, 0xff02fd, 0xff1ae5, 0xff9a65,
		0xffa25d, 0xff22dd, 0xff2ad5, 0xffaa55, 0xff926d, 0xff12ed, 0xff0af5,
		0xff8a75, 0xffb24d, 0xff32cd, 0xff38c7, 0xffb847, 0xff7887, 0xfff807,
		0xff18e7, 0xff9867, 0xff58a7, 0xffd827, 0xff28d7, 0xffa857, 0xff6897,
		0xffe817, 0xff08f7, 0xff8877, 0xff48b7, 0xffc837, 0xff30cf, 0xffb04f,
		0xff708f, 0xfff00f, 0xff10ef, 0xff906f, 0xff50af, 0xffd02f, 0xff20df,
		0xffa05f, 0xff609f, 0xffe01f };
#endif

boolean isOn = false;
boolean isPlaying = true;

App* pCurrentApp = NULL;
VisualizerApp* pVisApp1;
VisualizerApp* pVisApp2;
VisualizerApp* pVisApp3;
VisualizerApp* pVisApp4;
VisualizerApp* pVisApp5;

Runnable* pCurrentRunnable = NULL;

int sensorPin = A0;
long sensorValue = 0;

long lmillis = 0;
uint8_t lastButton = 0;
int r = 255, g = 255, b = 255;
float brightness = 1.0f;
float targetBrightness = 1.0f;

String lampHostname;
float calibRed = 1.0f;
float calibGreen = 1.0f;
float calibBlue = 1.0f;

float val = 0.0f;
float maxVal = 0.0f;
float minVal = 2.0f;
float curVal = 0.0f;

long lastSensorBurstRead = 0;
long lastMillis = 0;
long lastBrightnessAdjustMillis = 0;

void onButton(uint8_t btn);
void update();
void switchApp(App* pApp);
void writeConfiguration();

#ifdef MQTT_ENABLE
void mqttCallback(char* topic, byte* payload, unsigned int length) {
	Logger.info("Message arrived [%s]", topic);

	Logger.info("Lamp state: %s, msg: %d", isOn ? "ON" : "OFF", payload[0]);

	if (payload[0] == '0' && isOn) {
		Logger.debug("Turning lamp OFF");
		turnOffRunnable.init();
		pCurrentRunnable = &turnOffRunnable;
	} else if (payload[0] == '1' && !isOn) {
		Logger.debug("Turning lamp ON");
		turnOnRunnable.init();
		pCurrentRunnable = &turnOnRunnable;
	}
	update();
}

void mqttReconnect() {
	// Loop until we're reconnected
	while (!mqttClient.connected()) {
		Logger.info("Attempting MQTT connection...");
		if (mqttClient.connect(lampHostname.c_str())) {
			Logger.info("Connected to MQTT server");
			mqttClient.subscribe(MQTT_TOPIC);
		} else {
			Logger.info("failed, rc=%d. Trying again in a few seconds",
					mqttClient.state());
			delay(5000);
		}
	}
}
#endif // MQTT_ENABLE

#ifdef ARTNET_ENABLE
void onDmxFrame(uint16_t universe, uint16_t length, uint8_t sequence,
		uint8_t* data) {

	int pixel = 0;
	uint16_t x = 0, y = 0;
	for (int i = 0; i < length; i += 3) {
		matrix.setPixel(x, y, data[i], data[i + 1], data[i + 2]);
		if (++x >= LEDS_WIDTH) {
			++y;
			x = 0;
		}
	}

	matrix.update();
}
#endif // ARTNET_ENABLE

void connectToWiFi() {
	WiFi.hostname(lampHostname);
	WiFi.mode(WIFI_STA);
	WiFi.begin();
	WiFiManager wifiManager;
	wifiManager.setTimeout(60 * 2);
	char apName[32];
	sprintf(apName, "SmartLEDLamp-%06x", ESP.getChipId());
	if (!wifiManager.autoConnect(apName)) {
		delay(1000);
		Logger.info("Failed to connect, resetting...");
		ESP.reset();
		delay(3000);
	}

	if (WiFi.isConnected()) {
		Logger.info("IP: %s", WiFi.localIP().toString().c_str());
	}
}

void handleAction() {
	String act = server.arg("act");
	if (act.length() != 0) {
		if (act == "off" && isOn) {
			turnOffRunnable.init();
			pCurrentRunnable = &turnOffRunnable;
		} else if (act == "on" && !isOn) {
			turnOnRunnable.init();
			pCurrentRunnable = &turnOnRunnable;
		}
		update();
	}

	String btn = server.arg("btn");
	if (btn.length() != 0) {
		long btnNo = btn.toInt();

		if (btnNo >= 0) {
			onButton((uint8_t) btnNo);
		}
	}

	String brightness = server.arg("brightness");
	if (brightness.length() != 0) {
		long bghtness = brightness.toInt();

		if (bghtness >= 0 && bghtness <= 100) {
			if (bghtness == 0)
				bghtness = 1;
			targetBrightness = (float) (bghtness / 100.0);
		}
	}

	server.send(200, "text/plain", "OK");
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload,
		size_t lenght) {
	socketNumber = num;

	switch (type) {
	case WStype_DISCONNECTED:
		break;
	case WStype_CONNECTED:
		webSocket.sendTXT(num, "Connected");
		break;
	case WStype_TEXT:
		if (payload[0] == '!') {
			Logger.debug("Received RPC '%s'", &payload[1]);

			char* token = strtok((char*) &payload[1], " ");

			if (strcmp("setCalibration", token) == 0) {
				token = strtok(NULL, " ");
				double red = strtod(token, NULL);
				token = strtok(NULL, " ");
				double green = strtod(token, NULL);
				token = strtok(NULL, " ");
				double blue = strtod(token, NULL);

				calibRed = red;
				calibGreen = green;
				calibBlue = blue;

				Logger.debug("Calibrating to %f, %f, %f", red, green, blue);
				matrix.setCalibration(calibRed, calibGreen, calibBlue);
			} else if (strcmp("setHostname", token) == 0) {
				token = strtok(NULL, " ");
				lampHostname = token;
				if (lampHostname.length() > 31)
					lampHostname = lampHostname.substring(0, 31);
				lampHostname.trim();

				Logger.debug("Setting hostname to %s", lampHostname.c_str());
			} else if (strcmp("saveConfiguration", token) == 0) {
				writeConfiguration();
				Logger.debug("Configuration saved");
			} else if (strcmp("getConfiguration", token) == 0) {
				String ret = "<getConfiguration " + lampHostname + " "
						+ calibRed + " " + calibGreen + " " + calibBlue;
				webSocket.sendTXT(num, ret.c_str());
			}

			update();
		}
		break;
	}
}

void startWebServer() {
	server.on("/action/", handleAction);
	server.begin();

	server.serveStatic("/", SPIFFS, "/", "no-cache");

	Logger.info("HTTP server started");

	webSocket.onEvent(webSocketEvent);
	webSocket.begin();

	Logger.info("WebSocket server started");
}

void setupOTA() {
	ArduinoOTA.setPort(8266);
	ArduinoOTA.setHostname(lampHostname.c_str());

	ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
		Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
	});
	ArduinoOTA.onError([](ota_error_t error) {
		Serial.printf("Error[%u]: ", error);
		if (error == OTA_AUTH_ERROR)
		Serial.println("Auth Failed");
		else
		if (error == OTA_BEGIN_ERROR)
		Serial.println("Begin Failed");
		else
		if (error == OTA_CONNECT_ERROR)
		Serial.println("Connect Failed");
		else
		if (error == OTA_RECEIVE_ERROR)
		Serial.println("Receive Failed");
		else
		if (error == OTA_END_ERROR)
		Serial.println("End Failed");
	});
	ArduinoOTA.begin();
}

void switchApp(App* pApp) {
	if (pApp == pCurrentApp)
		return;

	if (!pApp) {
		EEPROM.write(0, 0);
	} else if (pApp == pVisApp1) {
		EEPROM.write(0, 1);
	} else if (pApp == pVisApp2) {
		EEPROM.write(0, 2);
	} else if (pApp == pVisApp3) {
		EEPROM.write(0, 3);
	} else if (pApp == pVisApp4) {
		EEPROM.write(0, 4);
	} else if (pApp == pVisApp5) {
		EEPROM.write(0, 5);
	}
	EEPROM.commit();

	if (pCurrentApp) {
		pCurrentApp->stop();
	}
	if (pApp) {
		pApp->start();
	}

	pCurrentApp = pApp;

	if (pCurrentApp) {
		update();
	}
}

void readConfiguration() {
	File configFile = SPIFFS.open("/config.json", "r");
	if (!configFile) {
		Logger.info("No config file found, using defaults");
		return;
	}

	size_t size = configFile.size();
	if (size > 1024) {
		Logger.error("Config file size is too large");
		return;
	}

	StaticJsonDocument<200> jsonDoc;
	DeserializationError err = deserializeJson(jsonDoc, configFile);
	if (err) {
		Logger.error("Failed to parse config file");
		return;
	}

	char defaultHostname[15];
	sprintf(defaultHostname, "LEDLamp-%06x", ESP.getChipId());

	lampHostname = jsonDoc["hostname"] | defaultHostname;

	Logger.info("Hostname is %s", lampHostname.c_str());

	calibRed = jsonDoc["calibration"]["red"];
	calibGreen = jsonDoc["calibration"]["green"];
	calibBlue = jsonDoc["calibration"]["blue"];

	configFile.close();
}

void writeConfiguration() {
	File configFile = SPIFFS.open("/config.json", "w");

	StaticJsonDocument<200> jsonDoc;
	if (lampHostname)
		jsonDoc["hostname"] = lampHostname;

	JsonObject calibration = jsonDoc.createNestedObject("calibration");
	calibration["red"] = calibRed;
	calibration["green"] = calibGreen;
	calibration["blue"] = calibBlue;

	serializeJsonPretty(jsonDoc, configFile);

	configFile.close();
}

void setup() {
	// Turn off blue status LED
	pinMode(BUILTIN_LED, OUTPUT);
	digitalWrite(BUILTIN_LED, HIGH);

	delay(300);

	Logger.begin();
	Serial.println("\n\n\n\n");
	Logger.info("Starting Smart LED Lamp %s", VERSION);
	Logger.info("Free Sketch Space: %i", ESP.getFreeSketchSpace());

	SPIFFS.begin();

	matrix.init();
	matrix.clear();

	readConfiguration();

	matrix.setCalibration(calibRed, calibGreen, calibBlue);

	connectToWiFi();
	setupOTA();

	matrix.clear();

	startWebServer();

#ifdef ARTNET_ENABLE
	artnet.setArtDmxCallback(onDmxFrame);
	artnet.begin();
#endif // ARTNET_ENABLE

	matrix.clear();

	pVisApp1 = new VisualizerApp(&matrix);
	pVisApp1->setVisualizer(0, new FadeAndScrollVisualizer(1, 20));

	pVisApp2 = new VisualizerApp(&matrix);
	pVisApp2->setVisualizer(0, new FireVisualizer());

	pVisApp3 = new VisualizerApp(&matrix);
	pVisApp3->setVisualizer(0, new DemoReelVisualizer());

	pVisApp4 = new VisualizerApp(&matrix);
	pVisApp4->setVisualizer(0, new VUMeterVisualizer(&curVal));

	pVisApp5 = new VisualizerApp(&matrix);
	pVisApp5->setVisualizer(0, new NoiseWithPaletteVisualizer());

#ifdef IR_ENABLE
	irrecv.enableIRIn(); // Start the receiver
#endif

#ifdef MQTT_ENABLE
	mqttClient.setServer(MQTT_SERVER, 1883);
	mqttClient.setCallback(mqttCallback);
#endif // MQTT_ENABLE

	update();

	pinMode(sensorPin, INPUT);

	EEPROM.begin(16);
	uint8_t lastApp = EEPROM.read(0);
	if (lastApp >= 0 && lastApp <= 5) {
		if (lastApp == 0)
			switchApp(NULL);
		else if (lastApp == 1)
			switchApp(pVisApp1);
		else if (lastApp == 2)
			switchApp(pVisApp2);
		else if (lastApp == 3)
			switchApp(pVisApp3);
		else if (lastApp == 4)
			switchApp(pVisApp4);
		else if (lastApp == 5)
			switchApp(pVisApp5);
	}
}

void update() {
	if (isOn) {
		if (pCurrentApp) {
			pCurrentApp->update();
		} else {
			matrix.fill(r, g, b);
		}
	} else {
		matrix.clear();
	}
	matrix.update();
}

void onButton(uint8_t btn) {
	Logger.debug("Button '%i' pressed", btn);

	if (btn == BTN_POWER) {
		if (!isOn) {
			turnOnRunnable.init();
			pCurrentRunnable = &turnOnRunnable;
		} else {
			turnOffRunnable.init();
			pCurrentRunnable = &turnOffRunnable;
		}
		delay(200);
		update();
	}

	if (!isOn) {
		return;
	}

	if (pCurrentApp) {
		if (pCurrentApp->onButtonPressed(btn))
			return;
	}

	switch (btn) {
	case BTN_BRIGHTER:
		brightness += 0.05f;
		if (brightness > 1.0f)
			brightness = 1.0f;
		matrix.setBrightness(brightness);
		targetBrightness = brightness;
		matrix.update();
		break;
	case BTN_DARKER:
		brightness -= 0.05f;
		if (brightness < 0.05f)
			brightness = 0.05f;
		matrix.setBrightness(brightness);
		targetBrightness = brightness;
		matrix.update();
		break;
	case BTN_PAUSE:
		isPlaying = !isPlaying;
		delay(200);
		break;

	case BTN_R:
		switchApp(NULL);
		g = b = 0;
		r = 0xff;
		update();
		break;
	case BTN_G:
		switchApp(NULL);
		r = b = 0;
		g = 0xff;
		update();
		break;
	case BTN_B:
		switchApp(NULL);
		r = g = 0;
		b = 0xff;
		update();
		break;
	case BTN_W:
		switchApp(NULL);
		r = g = b = 0xff;
		update();
		break;

	case 9:
		switchApp(NULL);
		r = 0xf5;
		g = 0x28;
		b = 0x0a;
		update();
		break;
	case 10:
		switchApp(NULL);
		r = 0x00;
		g = 0xff;
		b = 0x14;
		update();
		break;
	case 11:
		switchApp(NULL);
		r = 0x19;
		g = 0x19;
		b = 0xff;
		update();
		break;
	case 12:
		switchApp(NULL);
		r = 0xf5;
		g = 0x69;
		b = 0x1e;
		update();
		break;

	case 13:
		switchApp(NULL);
		r = 0xff;
		g = 0x28;
		b = 0x0f;
		update();
		break;
	case 14:
		switchApp(NULL);
		r = 0x00;
		g = 0x7d;
		b = 0x69;
		update();
		break;
	case 15:
		switchApp(NULL);
		r = 0x00;
		g = 0x00;
		b = 0x14;
		update();
		break;
	case 16:
		switchApp(NULL);
		r = 0xf5;
		g = 0x69;
		b = 0x1e;
		update();
		break;

	case 17:
		switchApp(NULL);
		r = 0xf5;
		g = 0x28;
		b = 0x0a;
		update();
		break;
	case 18:
		switchApp(NULL);
		r = 0x00;
		g = 0x46;
		b = 0x37;
		update();
		break;
	case 19:
		switchApp(NULL);
		r = 0xb4;
		g = 0x00;
		b = 0x69;
		update();
		break;
	case 20:
		switchApp(NULL);
		r = 0x4b;
		g = 0x55;
		b = 0xff;
		update();
		break;

	case 21:
		switchApp(NULL);
		r = 0xfa;
		g = 0xec;
		b = 0x00;
		update();
		break;
	case 22:
		switchApp(NULL);
		r = 0x00;
		g = 0x23;
		b = 0x0a;
		update();
		break;
	case 23:
		switchApp(NULL);
		r = 0xe6;
		g = 0x00;
		b = 0x37;
		update();
		break;
	case 24:
		switchApp(NULL);
		r = 0x4b;
		g = 0x55;
		b = 0xff;
		update();
		break;

	case BTN_RED_UP:
		r += 5;
		if (r > 255)
			r = 255;
		update();
		break;
	case BTN_RED_DOWN:
		r -= 5;
		if (r < 0)
			r = 0;
		update();
		break;
	case BTN_GREEN_UP:
		g += 5;
		if (g > 255)
			g = 255;
		update();
		break;
	case BTN_GREEN_DOWN:
		g -= 5;
		if (g < 0)
			g = 0;
		update();
		break;
	case BTN_BLUE_UP:
		b += 5;
		if (b > 255)
			b = 255;
		update();
		break;
	case BTN_BLUE_DOWN:
		b -= 5;
		if (b < 0)
			b = 0;
		update();
		break;

	case BTN_FLASH:
		switchApp(pVisApp5);
		break;

	case BTN_JUMP3:
		switchApp(pVisApp1);
		break;
	case BTN_JUMP7:
		switchApp(pVisApp2);
		break;
	case BTN_FADE3:
		switchApp(pVisApp3);
		break;
	case BTN_FADE7:
		switchApp(pVisApp4);
		break;
	}

}

void readAnalogPeek() {
	maxVal = -10.0f;
	minVal = 10.0f;
//	String message = "# " + String(messageNumber) + " ";
	for (int i = 0; i < 300; ++i) {
		sensorValue = analogRead(sensorPin);
//		if (i & 1) {
//			message = message + String(sensorValue) + ";";
//		}
		float newVal = sensorValue / 1024.0f;
		if (newVal > maxVal)
			maxVal = newVal;

		if (newVal < minVal)
			minVal = newVal;

		if (maxVal - minVal > curVal) {
			curVal = maxVal - minVal;
		}
	}
//	message[message.length() - 1] = '\0';
//	webSocket.sendTXT(socketNumber, message);
	lastSensorBurstRead = millis();
}

void loop() {
	long currentMillis = millis();

//	if (currentMillis > lastSensorBurstRead + 6 /*5*/) {
//		readAnalogPeek();
//	}

	ArduinoOTA.handle();
	server.handleClient();
	webSocket.loop();
	Logger.loop();

	if (brightness != targetBrightness
			&& currentMillis > lastBrightnessAdjustMillis + 25) {
		lastBrightnessAdjustMillis = currentMillis;

		int brightnessDiff = 100 * targetBrightness - 100 * brightness;

		if (abs(brightnessDiff) <= 1) {
			brightness = targetBrightness;
		}

		if (brightness > targetBrightness) {
			brightness -= 0.01;
		} else if (brightness < targetBrightness) {
			brightness += 0.01;
		}
		Logger.debug("Setting brightness to %f (%f)", brightness,
				targetBrightness);
		matrix.setBrightness(brightness);

		if ((!pCurrentRunnable && !pCurrentApp) || !isPlaying) {
			update();
		}
	}

	if (pCurrentRunnable) {
		pCurrentRunnable->run();
		update();
	}

	if (isOn && isPlaying) {
		if (pCurrentApp) {
			pCurrentApp->run();
		}
#ifdef ARTNET_ENABLE
		else {
			artnet.read();
		}
#endif // ARTNET_ENABLE
	} else if (currentMillis > lastMatrixRefreshMs + 5000) {
		lastMatrixRefreshMs = currentMillis;
		update();
	}

#ifdef IR_ENABLE
	decode_results results;

	if (currentMillis > lmillis + 200) {
		lastButton = 0;
	}

	if (irrecv.decode(&results)) {
		if (results.value == 0xffffffff && lastButton != BTN_POWER) {
			lmillis = currentMillis;
		} else if ((results.value & 0xff000000) == 0) {
			for (int i = 0; i < sizeof(irCodes) / sizeof(irCodes[0]); ++i) {
				if (irCodes[i] == results.value) {
					lastButton = i + 1;
					lmillis = currentMillis;
					break;
				}
			}
		}
		if (lastButton)
			onButton(lastButton);
		irrecv.resume(); // Receive the next value
	}
#endif

#ifdef MQTT_ENABLE
	if (!mqttClient.connected()) {
		mqttReconnect();
	}
	mqttClient.loop();
#endif

}
