#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <AM232X.h>
#include <PubSubClient.h>
#include "setenv.h"

const uint8_t i2c_SDA = 1;
const uint8_t i2c_SCL = 0;

const uint8_t pinBMEminus = 3; //  питание датчика -
const uint8_t pinBMEplus = 19; //  питание датчика +
const uint8_t pinVcc = 4;      //  напряжение батареи

TwoWire i2cAM2320 = TwoWire(0);
AM232X AM2320(&i2cAM2320);

WiFiClient espClient;
PubSubClient client(espClient);

//------------------------------------------------
const char *ssid = WiFi_SSID;
const char *pass = WiFi_PASS;

const char *mqtt_client = MQTT_CLIENT_N;
const char *mqtt_client2 = MQTT_CLIENT_T;
const char *mqtt_user = MQTT_USER;
const char *mqtt_pass = MQTT_PASS;
const char *mqtt_server = MQTT_SERVER;
const char *mqtt_port = MQTT_PORT;

const char *outTopicTemp = "/Temp";
const char *outTopicPres = "/Pres";
const char *outTopicHum = "/Hum";
const char *outTopicVcc = "/Vcc";

//------------------------------------------------
uint8_t countConnect = 20;        //  кол-во попыток соединения
uint16_t countPause = 500;        //  пауза между попытками
uint32_t timeSleep = 30000000;    //  время сна
uint16_t TimeBeforeBedtime = 500; //  время до засыпания
uint8_t countMaxSleep = 120;      //  не передавать данные не более
                                  //      countMaxSleep х timeSleep (60мин)

RTC_DATA_ATTR struct
{
    float t = 0;            //  температура
    float v = 0;            //  напряжение
    uint8_t h = 0;          //  влажность
    uint8_t countSleep = 0; //  счетчик предельного кол-ва циклов сна
} data;

float diffTemp = 0.25; //  разница температур
int diffHum = 5;       //  разница влажностей
float diffVcc = 0.1;   //  разница зарядки акб

RTC_DATA_ATTR bool flagNotWork = false;

//-----------------------------------
inline bool mqtt_subscribe(PubSubClient &client, const String &topic)
{
    Serial.print("Subscribing to: ");
    Serial.println(topic);
    return client.subscribe(topic.c_str());
}

//-----------------------------------
inline bool mqtt_publish(PubSubClient &client, const String &topic, const String &value)
{
    Serial.print(topic);
    Serial.print(" = ");
    Serial.println(value);
    return client.publish(topic.c_str(), value.c_str());
}

//-----------------------------------
void mqttDataOut(float temp, uint8_t hum, float vcc)
{
    String topic = "/";
    topic += mqtt_client2;
    topic += outTopicTemp;
    while (!mqtt_publish(client, topic, (String)temp))
    {
        mqtt_publish(client, topic, (String)temp);
    }

    topic = "/";
    topic += mqtt_client2;
    topic += outTopicHum;
    while (!mqtt_publish(client, topic, (String)hum))
    {
        mqtt_publish(client, topic, (String)hum);
    }

    topic = "/";
    topic += mqtt_client2;
    topic += outTopicVcc;
    while (!mqtt_publish(client, topic, (String)vcc))
    {
        mqtt_publish(client, topic, (String)vcc);
    }
}

//-----------------------------------
bool reconnect()
{
    client.setServer(mqtt_server, String(mqtt_port).toInt());

    Serial.print("MQTT connect : ");
    Serial.println(mqtt_server);

    while (!(client.connect(mqtt_client, mqtt_user, mqtt_pass)) && countConnect--)
    {
        Serial.print(countConnect);
        Serial.print('>');
        delay(countPause);
    }

    if (client.connected())
    {
        Serial.println("MQTT connected - OK !");
        return true;
    }
    else
    {
        return false;
    }
}

//-----------------------------------
bool setupWiFi(const char *wifi_ssid, const char *wifi_pass)
{
    WiFi.begin(wifi_ssid, wifi_pass);

    Serial.print("Setup WiFi: ");
    Serial.println(ssid);

    while ((WiFi.status() != WL_CONNECTED) && countConnect--)
    {
        Serial.print(countConnect);
        Serial.print('>');
        delay(countPause);
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        // индикация IP
        Serial.print("\nWiFi connected - OK !\n");
        Serial.println(WiFi.localIP());
        // индикация силы сигнала
        int8_t dBm = WiFi.RSSI();
        Serial.print("RSSI dBm = ");
        Serial.println(dBm);
        uint8_t quality_RSSI = 2 * (dBm + 100);
        if (quality_RSSI >= 100)
            quality_RSSI = 100;
        Serial.print("RSSI % = ");
        Serial.println(quality_RSSI);
        Serial.println("=================");
        return true;
    }
    else
    {
        return false;
    }
}

//-----------------------------------
// медиана на 3 значения со своим буфером
uint16_t medianRoom(uint16_t newValRoom)
{
    RTC_DATA_ATTR static uint16_t bufRoom[3] = {0, 0, 0};
    RTC_DATA_ATTR static byte countRoom = 0;

    Serial.println();
    Serial.print(countRoom);
    Serial.print(" - ");
    Serial.print(bufRoom[0]);
    Serial.print("...");
    Serial.print(bufRoom[1]);
    Serial.print("...");
    Serial.print(bufRoom[2]);
    Serial.println();

    if (!(bufRoom[0] + bufRoom[1] + bufRoom[2]))
        bufRoom[0] = bufRoom[1] = bufRoom[2] = newValRoom;

    bufRoom[countRoom] = newValRoom;
    if (countRoom++ >= 2)
        countRoom = 0;
    uint16_t dataRoom = (max(bufRoom[0], bufRoom[1]) == max(bufRoom[1], bufRoom[2]))
                            ? max(bufRoom[0], bufRoom[2])
                            : max(bufRoom[1], min(bufRoom[0], bufRoom[2]));
    // return expRunningAverage(data);
    return dataRoom;
}

//-----------------------------------
bool readData()
{
    i2cAM2320.begin(i2c_SDA, i2c_SCL, 100000);
    delay(2000);

    if (!AM2320.begin())
    {
        Serial.println("am2320 not found !");
        flagNotWork = true;
        return false;
    }
    AM2320.wakeUp();
    i2cAM2320.read();

    float temp = AM2320.getTemperature();
    uint8_t hum = AM2320.getHumidity();

    float vcc = 0;
    Serial.println("\n=================");

    uint16_t dataVcc = 0;
    for (uint8_t i = 0; i < 3; ++i)
    {
        uint16_t temp = analogRead(pinVcc);
        Serial.print(temp);
        if (i < 2)
            Serial.print("...");
        dataVcc += temp;
    }
    Serial.print(" = ");
    Serial.print(dataVcc / 3);
    vcc = 2.8 / 4095 * medianRoom(dataVcc / 3) * 3.6 / 2.8;
    Serial.println();

    Serial.print("Temperature = ");
    Serial.print(temp);
    Serial.println(" *C");

    Serial.print("Humidity = ");
    Serial.print(hum);
    Serial.println(" %");

    Serial.print("Vcc = ");
    Serial.print(vcc);
    Serial.println(" v");

    Serial.println("=================\n");

    if (abs(data.t - temp) >= diffTemp || abs(data.h - hum) >= diffHum ||
        abs(data.v - vcc) >= diffVcc || flagNotWork)
    {
        data.t = temp;
        data.h = hum;
        data.v = vcc;

        data.countSleep = countMaxSleep;
        return true;
    }
    else
    {
        if (--data.countSleep)
        {
            return false;
        }
        else
        {
            return true;
        }
        Serial.print("\ncountSleep = ");
        Serial.println(data.countSleep);
    }
}

//-----------------------------------
void setup()
{
    Serial.begin(115200);
    pinMode(pinBMEplus, OUTPUT);
    digitalWrite(pinBMEplus, HIGH);
    pinMode(pinBMEminus, OUTPUT);
    digitalWrite(pinBMEminus, LOW);
    delay(20);

    if (!readData())
    {
        esp_deep_sleep(timeSleep);
    }

    if (!setupWiFi(ssid, pass))
    {
        flagNotWork = true;
        esp_deep_sleep(timeSleep);
    }
    else
    {
        flagNotWork = false;
    }

    if (!reconnect())
    {
        flagNotWork = true;
        esp_deep_sleep(timeSleep);
    }
    else
    {
        flagNotWork = false;
    }

    mqttDataOut(data.t, data.h, data.v);

    Serial.println("=================");
    Serial.flush();

    delay(TimeBeforeBedtime);
    esp_deep_sleep(timeSleep);
}

void loop() {}