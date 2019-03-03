/*
 Name:		AmsToMqttBridge.ino
 Created:	3/13/2018 7:40:28 PM
 Author:	roarf
*/

#include <PubSubClient.h>
#include <HanReader.h>
#include <Kamstrup.h>
#include "configuration.h"
#include "accesspoint.h"

#define WIFI_CONNECTION_TIMEOUT 30000;
#define LED_PIN 2 // The blue on-board LED of the ESP

/**
 * Firmware updater
 */

#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>

const int FW_VERSION = 1011;
//const char* mqtt_topic_fw_info = "esp/test/info";

// Object used to boot as Access Point
accesspoint ap;

// WiFi client and MQTT client
WiFiClient *client;
PubSubClient mqtt;

// Object used for debugging
HardwareSerial* debugger = NULL;

// The HAN Port reader, used to read serial data and decode DLMS
HanReader hanReader;

// the setup function runs once when you press reset or power the board
void setup() 
{
	// Uncomment to debug over the same port as used for HAN communication
	debugger = &Serial;
	
	if (debugger) {
		// Setup serial port for debugging
		debugger->begin(2400, SERIAL_8N1);
		while (!&debugger);
    debugger->setDebugOutput(true);
		debugger->println("Started...");
	}

	// Assign pin for boot as AP
	delay(1000);
	pinMode(0, INPUT_PULLUP);
	
	// Flash the blue LED, to indicate we can boot as AP now
	pinMode(LED_PIN, OUTPUT);
	digitalWrite(LED_PIN, LOW);
	
	// Initialize the AP
	ap.setup(0, Serial);
	
	// Turn off the blue LED
	digitalWrite(LED_PIN, HIGH);

	if (!ap.isActivated)
	{
		setupWiFi();
		hanReader.setup(&Serial, 2400, SERIAL_8N1, debugger);
		
		// Compensate for the known Kaifa bug
    //hanReader.compensateFor09HeaderBug = true;
		//hanReader.compensateFor09HeaderBug = (ap.config.meterType == 1);
	}
}

// the loop function runs over and over again until power down or reset
void loop()
{
	// Only do normal stuff if we're not booted as AP
	if (!ap.loop())
	{
		// turn off the blue LED
		digitalWrite(LED_PIN, HIGH);

		// allow the MQTT client some resources
		mqtt.loop();
		delay(10); // <- fixes some issues with WiFi stability

		// Reconnect to WiFi and MQTT as needed
		if (!mqtt.connected()) {
			MQTT_connect();
		}
		else
		{
			// Read data from the HAN port
			readHanPort();
		}
	}
	else
	{
		// Continously flash the blue LED when AP mode
		if (millis() / 1000 % 2 == 0)
			digitalWrite(LED_PIN, LOW);
		else
			digitalWrite(LED_PIN, HIGH);
	}
}

void setupWiFi()
{
	// Turn off AP
	WiFi.enableAP(false);

	// Connect to WiFi
	WiFi.begin(ap.config.ssid, ap.config.ssidPassword);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  
	// Initialize WiFi and MQTT clients
	if (ap.config.isSecure())
		client = new WiFiClientSecure();
	else
		client = new WiFiClient();
	mqtt = PubSubClient(*client);
	mqtt.setServer(ap.config.mqtt, ap.config.mqttPort);

	// Direct incoming MQTT messages
	if (ap.config.mqttSubscribeTopic != 0 && strlen(ap.config.mqttSubscribeTopic) > 0)
		mqtt.setCallback(mqttMessageReceived);

	// Connect to the MQTT server
	MQTT_connect();

	// Notify everyone we're here!
	sendMqttData("Connected! MeterType:");
  sendMqttData((String)ap.config.meterType);
}

void mqttMessageReceived(char* topic, unsigned char* payload, unsigned int length)
{
	// make the incoming message a null-terminated string
	char message[1000];
	for (int i = 0; i < length; i++)
		message[i] = payload[i];
	message[length] = 0;

	if (debugger) {
		debugger->println("Incoming MQTT message:");
		debugger->print("[");
		debugger->print(topic);
		debugger->print("] ");
		debugger->println(message);
	}

  checkForUpdates(message);
}

void readHanPort()
{
	if (hanReader.read())
	{
		// Flash LED on, this shows us that data is received
		digitalWrite(LED_PIN, LOW);

		// Get the list identifier
		int listSize = hanReader.getListSize();
    mqtt.publish((ap.config.mqttPublishTopic+((String)"/listSize")).c_str(), ((String)listSize).c_str());

		switch (ap.config.meterType)
		{
		/*case 1: // Kaifa
			readHanPort_Kaifa(listSize);
			break;*/
		case 3: // Kamstrup
			readHanPort_Kamstrup(listSize);
			break;
		/*case 3: // Aidon
			readHanPort_Aidon(listSize);
			break;*/
		default:
			debugger->print("Meter type ");
			debugger->print(ap.config.meterType, HEX);
			debugger->println(" is unknown");
			delay(10000);
			break;
		}

		// Flash LED off
		digitalWrite(LED_PIN, HIGH);
	}
}

void readHanPort_Kamstrup(int listSize)
{
  // Make sure we have configured a publish topic
  if (ap.config.mqttPublishTopic == 0 || strlen(ap.config.mqttPublishTopic) == 0)
  {
    return;
  }

	// Check if valid kamstrup list
	if (listSize == (int)Kamstrup::List1 || listSize == (int)Kamstrup::List2 || listSize == (int)Kamstrup::List3 || listSize == (int)Kamstrup::List4)
	{
    String id = "Unknown";
		if (listSize == (int)Kamstrup::List1)
		{
			id = hanReader.getString((int)Kamstrup_List1::ListVersionIdentifier);
		}
		else if (listSize == (int)Kamstrup::List2)
		{
			id = hanReader.getString((int)Kamstrup_List2::ListVersionIdentifier);
		}
    else if (listSize == (int)Kamstrup::List3)
    {
      id = hanReader.getString((int)Kamstrup_List3::ListVersionIdentifier);
    }
    else if (listSize == (int)Kamstrup::List4)
    {
      id = hanReader.getString((int)Kamstrup_List4::ListVersionIdentifier);
    }
    if (debugger) debugger->println(id);

		// Get the timestamp (as unix time) from the package
		time_t time = hanReader.getPackageTime();
		if (debugger) debugger->print("Time of the package is: ");
		if (debugger) debugger->println(time);

		// Any generic useful info here
		mqtt.publish((ap.config.mqttPublishTopic+((String)"/mac")).c_str(), ((String)WiFi.macAddress()).c_str());
    mqtt.publish((ap.config.mqttPublishTopic+((String)"/uptime")).c_str(), ((String)millis()).c_str());
    mqtt.publish((ap.config.mqttPublishTopic+((String)"/time")).c_str(), ((String)time).c_str());
    
    
		// Based on the list number, get all details 
		// according to OBIS specifications for the meter
		if (listSize == (int)Kamstrup::List1)
		{
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/listversionidentifier")).c_str(), (hanReader.getString((int)Kamstrup_List1::ListVersionIdentifier)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/meterid")).c_str(), (hanReader.getString((int)Kamstrup_List1::MeterID)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/metertype")).c_str(), (hanReader.getString((int)Kamstrup_List1::MeterType)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/activeimportpower")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List1::ActiveImportPower)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/reactiveexportpower")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List1::ReactiveImportPower)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/currentl1")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List1::CurrentL1)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/currentl2")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List1::CurrentL2)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/currentl3")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List1::CurrentL3)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/voltagel1")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List1::VoltageL1)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/voltagel2")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List1::VoltageL2)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/voltagel3")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List1::VoltageL3)).c_str());
		}
		else if (listSize == (int)Kamstrup::List2)
		{
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/listversionidentifier")).c_str(), (hanReader.getString((int)Kamstrup_List2::ListVersionIdentifier)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/meterid")).c_str(), (hanReader.getString((int)Kamstrup_List2::MeterID)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/metertype")).c_str(), (hanReader.getString((int)Kamstrup_List2::MeterType)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/activeimportpower")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List2::ActiveImportPower)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/reactiveimportpower")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List2::ReactiveImportPower)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/currentl1")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List2::CurrentL1)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/currentl2")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List2::CurrentL2)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/currentl3")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List2::CurrentL3)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/voltagel1")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List2::VoltageL1)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/voltagel2")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List2::VoltageL2)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/voltagel3")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List2::VoltageL3)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/cumulativeactiveimportenergy")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List2::CumulativeActiveImportEnergy)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/cumulativeactiveexportenergy")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List2::CumulativeActiveExportEnergy)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/cumulativereactiveimportenergy")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List2::CumulativeReactiveImportEnergy)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/cumulativereactiveexportenergy")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List2::CumulativeReactiveExportEnergy)).c_str());
		}
    else if (listSize == (int)Kamstrup::List3)
    {
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/listversionidentifier")).c_str(), (hanReader.getString((int)Kamstrup_List3::ListVersionIdentifier)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/meterid")).c_str(), (hanReader.getString((int)Kamstrup_List3::MeterID)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/metertype")).c_str(), (hanReader.getString((int)Kamstrup_List3::MeterType)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/activeimportpower")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List3::ActiveImportPower)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/reactiveexportpower")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List3::ReactiveImportPower)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/currentl1")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List3::CurrentL1)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/voltagel1")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List3::VoltageL1)).c_str());
    }
    else if (listSize == (int)Kamstrup::List4)
    {
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/listversionidentifier")).c_str(), (hanReader.getString((int)Kamstrup_List4::ListVersionIdentifier)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/meterid")).c_str(), (hanReader.getString((int)Kamstrup_List4::MeterID)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/metertype")).c_str(), (hanReader.getString((int)Kamstrup_List4::MeterType)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/activeimportpower")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List4::ActiveImportPower)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/reactiveimportpower")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List4::ReactiveImportPower)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/currentl1")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List4::CurrentL1)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/voltagel1")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List4::VoltageL1)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/cumulativeactiveimportenergy")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List4::CumulativeActiveImportEnergy)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/cumulativeactiveexportenergy")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List4::CumulativeActiveExportEnergy)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/cumulativereactiveimportenergy")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List4::CumulativeReactiveImportEnergy)).c_str());
      mqtt.publish((ap.config.mqttPublishTopic+((String)"/cumulativereactiveexportenergy")).c_str(), ((String)hanReader.getInt((int)Kamstrup_List4::CumulativeReactiveExportEnergy)).c_str());
    }
	}
}

// Function to connect and reconnect as necessary to the MQTT server.
// Should be called in the loop function and it will take care if connecting.
void MQTT_connect() 
{
	// Connect to WiFi access point.
	if (debugger)
	{
		debugger->println(); 
		debugger->println();
		debugger->print("Connecting to WiFi network ");
		debugger->println(ap.config.ssid);
    debugger->println(ap.config.ssidPassword);
	}

	if (WiFi.status() != WL_CONNECTED)
	{
		// Make one first attempt at connect, this seems to considerably speed up the first connection
		WiFi.disconnect();
		WiFi.begin(ap.config.ssid, ap.config.ssidPassword);
		delay(1000);
	}

	// Wait for the WiFi connection to complete
	long vTimeout = millis() + WIFI_CONNECTION_TIMEOUT;
	while (WiFi.status() != WL_CONNECTED) {
		delay(50);
		if (debugger) debugger->print(".");
		
		// If we timed out, disconnect and try again
		if (vTimeout < millis())
		{
			if (debugger)
			{
				debugger->print("Timout during connect. WiFi status is: ");
				debugger->println(WiFi.status());
			}
			WiFi.disconnect();
			WiFi.begin(ap.config.ssid, ap.config.ssidPassword);
			vTimeout = millis() + WIFI_CONNECTION_TIMEOUT;
		}
		yield();
	}

	if (debugger) {
		debugger->println();
		debugger->println("WiFi connected");
		debugger->println("IP address: ");
		debugger->println(WiFi.localIP());
		debugger->print("\nconnecting to MQTT: ");
		debugger->print(ap.config.mqtt);
		debugger->print(", port: ");
		debugger->print(ap.config.mqttPort);
		debugger->println();
	}

	// Wait for the MQTT connection to complete
	while (!mqtt.connected()) {
		
		// Connect to a unsecure or secure MQTT server
		if ((ap.config.mqttUser == 0 && mqtt.connect(ap.config.mqttClientID)) || 
			(ap.config.mqttUser != 0 && mqtt.connect(ap.config.mqttClientID, ap.config.mqttUser, ap.config.mqttPass)))
		{
			if (debugger) debugger->println("\nSuccessfully connected to MQTT!");

			// Subscribe to the chosen MQTT topic, if set in configuration
			if (ap.config.mqttSubscribeTopic != 0 && strlen(ap.config.mqttSubscribeTopic) > 0)
			{
				mqtt.subscribe(ap.config.mqttSubscribeTopic);
				if (debugger) debugger->printf("  Subscribing to [%s]\r\n", ap.config.mqttSubscribeTopic);
			}
		}
		else
		{
			if (debugger)
			{
				debugger->print(".");
				debugger->print("failed, mqtt.state() = ");
				debugger->print(mqtt.state());
				debugger->println(" trying again in 5 seconds");
			}

			// Wait 2 seconds before retrying
			mqtt.disconnect();
			delay(2000);
		}

		// Allow some resources for the WiFi connection
		yield();
	}
}

// Send a simple string embedded in json over MQTT
void sendMqttData(String data)
{
	// Make sure we have configured a publish topic
	if (ap.config.mqttPublishTopic == 0 || strlen(ap.config.mqttPublishTopic) == 0)
		return;

	// Make sure we're connected
	if (!client->connected() || !mqtt.connected()) {
		MQTT_connect();
	}

  mqtt.publish(ap.config.mqttPublishTopic, ((String)WiFi.macAddress()).c_str());
  mqtt.publish(ap.config.mqttPublishTopic, ((String)millis()).c_str());
  mqtt.publish(ap.config.mqttPublishTopic, data.c_str());
}


void checkForUpdates(String fwImageURL) {
  HTTPClient httpClient;
  
  String newFWVersion = httpClient.getString();

  mqtt.publish((ap.config.mqttPublishTopic+((String)"/info")).c_str(), ((String)"Current firmware version").c_str());
  mqtt.publish((ap.config.mqttPublishTopic+((String)"/info")).c_str(), ((String)FW_VERSION).c_str());
  
  mqtt.publish((ap.config.mqttPublishTopic+((String)"/info")).c_str(), ((String)"Preparing to update").c_str());
  
  mqtt.publish((ap.config.mqttPublishTopic+((String)"/info")).c_str(), ((String)"Downloading new firmware.").c_str());
  mqtt.publish((ap.config.mqttPublishTopic+((String)"/info")).c_str(), fwImageURL.c_str());
      
  t_httpUpdate_return ret = ESPhttpUpdate.update( fwImageURL );
  
  switch(ret) {
     case HTTP_UPDATE_FAILED:
       mqtt.publish((ap.config.mqttPublishTopic+((String)"/info")).c_str(), ((String)HTTP_UPDATE_FAILED).c_str());
       mqtt.publish((ap.config.mqttPublishTopic+((String)"/info")).c_str(), ((String)ESPhttpUpdate.getLastError()).c_str());
       mqtt.publish((ap.config.mqttPublishTopic+((String)"/info")).c_str(), ESPhttpUpdate.getLastErrorString().c_str());
       break;
    case HTTP_UPDATE_NO_UPDATES:
       mqtt.publish((ap.config.mqttPublishTopic+((String)"/info")).c_str(), ((String)HTTP_UPDATE_NO_UPDATES).c_str());
       break;
  }
    
  httpClient.end();
}
