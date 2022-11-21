//-----------------------------------------------------------------------------
// 2022 Ahoy, https://ahoydtu.de
// Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
//-----------------------------------------------------------------------------

#ifndef __PUB_MQTT_H__
#define __PUB_MQTT_H__

#ifdef ESP8266
    #include <ESP8266WiFi.h>
#elif defined(ESP32)
    #include <WiFi.h>
#endif

#include "../utils/dbg.h"
#include "../utils/ahoyTimer.h"
#include "../config/config.h"
#include <espMqttClient.h>
#include <ArduinoJson.h>
#include "../defines.h"
#include "../hm/hmSystem.h"


#define QOS_0   0

//https://bert.emelis.net/espMqttClient/

template<class HMSYSTEM>
class PubMqtt {
    public:
        PubMqtt() {
            mTxCnt = 0;
        }

        ~PubMqtt() { }

        void setup(cfgMqtt_t *cfg_mqtt, const char *devName, const char *version, HMSYSTEM *sys, uint32_t *utcTs, uint32_t *sunrise, uint32_t *sunset) {
            mCfgMqtt        = cfg_mqtt;
            mDevName        = devName;
            mVersion        = version;
            mSys            = sys;
            mUtcTimestamp   = utcTs;
            mSunrise        = sunrise;
            mSunset         = sunset;

            mHWifiCon = WiFi.onStationModeGotIP(std::bind(&PubMqtt::onWifiConnect, this, std::placeholders::_1));
            mHWifiDiscon = WiFi.onStationModeDisconnected(std::bind(&PubMqtt::onWifiDisconnect, this, std::placeholders::_1));


            if((strlen(mCfgMqtt->user) > 0) && (strlen(mCfgMqtt->pwd) > 0))
                mClient.setCredentials(mCfgMqtt->user, mCfgMqtt->pwd);
            mClient.setClientId(mDevName); // TODO: add mac?
            mClient.setServer(mCfgMqtt->broker, mCfgMqtt->port);
            mClient.onConnect(std::bind(&PubMqtt::onConnect, this, std::placeholders::_1));
            mClient.onDisconnect(std::bind(&PubMqtt::onDisconnect, this, std::placeholders::_1));
            mClient.onSubscribe(std::bind(&PubMqtt::onSubscribe, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
            mClient.onPublish(std::bind(&PubMqtt::onPublish, this, std::placeholders::_1));
            //mClient.setWill
        }

        void loop() {
            mClient.loop();
        }

        void tickerSecond() {
            sendIvData();
        }

        void tickerMinute() {
            char val[12];
            snprintf(val, 12, "%ld", millis() / 1000);
            publish("uptime", val);
            publish("wifi_rssi", String(WiFi.RSSI()).c_str());
        }

        void tickerHour() {
            publish("sunrise", String(*mSunrise).c_str(), true);
            publish("sunset", String(*mSunset).c_str(), true);
        }

        void publish(const char *subTopic, const char *payload, bool retained = false) {
            char topic[MQTT_TOPIC_LEN + 2];
            snprintf(topic, (MQTT_TOPIC_LEN + 2), "%s/%s", mCfgMqtt->topic, subTopic);
            mClient.publish(topic, QOS_0, retained, payload);
            mTxCnt++;
        }

        void subscribe(const char *subTopic) {
            char topic[MQTT_TOPIC_LEN + 20];
            snprintf(topic, (MQTT_TOPIC_LEN + 20), "%s/%s", mCfgMqtt->topic, subTopic);
            mClient.subscribe(topic, QOS_0);
        }

        inline bool isConnected() {
            return mClient.connected();
        }

        inline uint32_t getTxCnt(void) {
            return mTxCnt;
        }

        void payloadEventListener(uint8_t cmd) {
            mSendList.push(cmd);
        }

        void sendMqttDiscoveryConfig(const char *topic) {
            DPRINTLN(DBG_VERBOSE, F("sendMqttDiscoveryConfig"));

            char stateTopic[64], discoveryTopic[64], buffer[512], name[32], uniq_id[32];
            for (uint8_t id = 0; id < mSys->getNumInverters(); id++) {
                Inverter<> *iv = mSys->getInverterByPos(id);
                if (NULL != iv) {
                    record_t<> *rec = iv->getRecordStruct(RealTimeRunData_Debug);
                    DynamicJsonDocument deviceDoc(128);
                    deviceDoc["name"] = iv->config->name;
                    deviceDoc["ids"] = String(iv->config->serial.u64, HEX);
                    deviceDoc["cu"] = F("http://") + String(WiFi.localIP().toString());
                    deviceDoc["mf"] = "Hoymiles";
                    deviceDoc["mdl"] = iv->config->name;
                    JsonObject deviceObj = deviceDoc.as<JsonObject>();
                    DynamicJsonDocument doc(384);

                    for (uint8_t i = 0; i < rec->length; i++) {
                        if (rec->assign[i].ch == CH0) {
                            snprintf(name, 32, "%s %s", iv->config->name, iv->getFieldName(i, rec));
                        } else {
                            snprintf(name, 32, "%s CH%d %s", iv->config->name, rec->assign[i].ch, iv->getFieldName(i, rec));
                        }
                        snprintf(stateTopic, 64, "%s/%s/ch%d/%s", topic, iv->config->name, rec->assign[i].ch, iv->getFieldName(i, rec));
                        snprintf(discoveryTopic, 64, "%s/sensor/%s/ch%d_%s/config", MQTT_DISCOVERY_PREFIX, iv->config->name, rec->assign[i].ch, iv->getFieldName(i, rec));
                        snprintf(uniq_id, 32, "ch%d_%s", rec->assign[i].ch, iv->getFieldName(i, rec));
                        const char *devCls = getFieldDeviceClass(rec->assign[i].fieldId);
                        const char *stateCls = getFieldStateClass(rec->assign[i].fieldId);

                        doc["name"] = name;
                        doc["stat_t"] = stateTopic;
                        doc["unit_of_meas"] = iv->getUnit(i, rec);
                        doc["uniq_id"] = String(iv->config->serial.u64, HEX) + "_" + uniq_id;
                        doc["dev"] = deviceObj;
                        doc["exp_aft"] = MQTT_INTERVAL + 5;  // add 5 sec if connection is bad or ESP too slow @TODO: stimmt das wirklich als expire!?
                        if (devCls != NULL)
                            doc["dev_cla"] = devCls;
                        if (stateCls != NULL)
                            doc["stat_cla"] = stateCls;

                        serializeJson(doc, buffer);
                        publish(discoveryTopic, buffer, true);
                        doc.clear();
                    }

                    yield();
                }
            }
        }

    private:
        void onWifiConnect(const WiFiEventStationModeGotIP& event) {
            DPRINTLN(DBG_VERBOSE, F("MQTT connecting"));
            mClient.connect();
        }

        void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
            DPRINTLN(DBG_WARN, F("TODO: MQTT reconnect!"));
        }

        void onConnect(bool sessionPreset) {
            DPRINTLN(DBG_INFO, F("MQTT connected"));

            publish("version", mVersion, true);
            publish("device", mDevName, true);
            publish("uptime", "0");

            subscribe("devcontrol/#"); // TODO: register onMessage callback!
        }

        void onDisconnect(espMqttClientTypes::DisconnectReason reason) {
            DBGPRINT(F("MQTT disconnected, reason: "));
            switch (reason) {
                case espMqttClientTypes::DisconnectReason::TCP_DISCONNECTED:
                    DBGPRINTLN(F("TCP disconnect"));
                    break;
                case espMqttClientTypes::DisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION:
                    DBGPRINTLN(F("wrong protocol version"));
                    break;
                case espMqttClientTypes::DisconnectReason::MQTT_IDENTIFIER_REJECTED:
                    DBGPRINTLN(F("identifier rejected"));
                    break;
                case espMqttClientTypes::DisconnectReason::MQTT_SERVER_UNAVAILABLE:
                    DBGPRINTLN(F("broker unavailable"));
                    break;
                case espMqttClientTypes::DisconnectReason::MQTT_MALFORMED_CREDENTIALS:
                    DBGPRINTLN(F("malformed credentials"));
                    break;
                case espMqttClientTypes::DisconnectReason::MQTT_NOT_AUTHORIZED:
                    DBGPRINTLN(F("not authorized"));
                    break;
                default:
                    DBGPRINTLN(F("unknown"));
            }
        }

        void onSubscribe(uint16_t packetId, const espMqttClientTypes::SubscribeReturncode* codes, size_t len) {
            DPRINTLN(DBG_INFO, F("MQTT Subscribe"));
            Serial.print("  packetId: ");
            Serial.println(packetId);
            for (size_t i = 0; i < len; ++i) {
                Serial.print("  qos: ");
                Serial.println(static_cast<uint8_t>(codes[i]));
            }
        }

        void onPublish(uint16_t packetId) {
            Serial.println("Publish acknowledged.");
            Serial.print("  packetId: ");
            Serial.println(packetId);
        }


        /*void reconnect(void) {
            DPRINTLN(DBG_DEBUG, F("mqtt.h:reconnect"));
            DPRINTLN(DBG_DEBUG, F("MQTT mClient->_state ") + String(mClient->state()) );

            #ifdef ESP8266
                DPRINTLN(DBG_DEBUG, F("WIFI mEspClient.status ") + String(mEspClient.status()) );
            #endif

            boolean resub = false;
            if(!mClient->connected() && (millis() - mLastReconnect) > MQTT_RECONNECT_DELAY ) {
                mLastReconnect = millis();
                if(strlen(mDevName) > 0) {
                    // der Server und der Port müssen neu gesetzt werden,
                    // da ein MQTT_CONNECTION_LOST -3 die Werte zerstört hat.
                    mClient->setServer(mCfgMqtt->broker, mCfgMqtt->port);
                    mClient->setBufferSize(MQTT_MAX_PACKET_SIZE);

                    char lwt[MQTT_TOPIC_LEN + 7 ]; // "/uptime" --> + 7 byte
                    snprintf(lwt, MQTT_TOPIC_LEN + 7, "%s/uptime", mCfgMqtt->topic);

                    if((strlen(mCfgMqtt->user) > 0) && (strlen(mCfgMqtt->pwd) > 0))
                        resub = mClient->connect(mDevName, mCfgMqtt->user, mCfgMqtt->pwd, lwt, 0, false, "offline");
                    else
                        resub = mClient->connect(mDevName, lwt, 0, false, "offline");
                        // ein Subscribe ist nur nach einem connect notwendig
                    if(resub) {
                        char topic[MQTT_TOPIC_LEN + 13 ]; // "/devcontrol/#" --> + 6 byte
                        // ToDo: "/devcontrol/#" is hardcoded
                        snprintf(topic, MQTT_TOPIC_LEN + 13, "%s/devcontrol/#", mCfgMqtt->topic);
                        DPRINTLN(DBG_INFO, F("subscribe to ") + String(topic));
                        mClient->subscribe(topic); // subscribe to mTopic + "/devcontrol/#"
                    }
                }
            }
        }*/

        const char *getFieldDeviceClass(uint8_t fieldId) {
            uint8_t pos = 0;
            for (; pos < DEVICE_CLS_ASSIGN_LIST_LEN; pos++) {
                if (deviceFieldAssignment[pos].fieldId == fieldId)
                    break;
            }
            return (pos >= DEVICE_CLS_ASSIGN_LIST_LEN) ? NULL : deviceClasses[deviceFieldAssignment[pos].deviceClsId];
        }

        const char *getFieldStateClass(uint8_t fieldId) {
            uint8_t pos = 0;
            for (; pos < DEVICE_CLS_ASSIGN_LIST_LEN; pos++) {
                if (deviceFieldAssignment[pos].fieldId == fieldId)
                    break;
            }
            return (pos >= DEVICE_CLS_ASSIGN_LIST_LEN) ? NULL : stateClasses[deviceFieldAssignment[pos].stateClsId];
        }

        void sendIvData(void) {
            if(mSendList.empty())
                return;

            char topic[32 + MAX_NAME_LENGTH], val[40];
            float total[4];
            bool sendTotal = false;
            bool totalIncomplete = false;

            while(!mSendList.empty()) {
                memset(total, 0, sizeof(float) * 4);
                for (uint8_t id = 0; id < mSys->getNumInverters(); id++) {
                    Inverter<> *iv = mSys->getInverterByPos(id);
                    if (NULL == iv)
                        continue; // skip to next inverter

                    record_t<> *rec = iv->getRecordStruct(mSendList.front());

                    if(mSendList.front() == RealTimeRunData_Debug) {
                        // inverter status
                        uint8_t status = MQTT_STATUS_AVAIL_PROD;
                        if (!iv->isAvailable(*mUtcTimestamp, rec)) {
                            status = MQTT_STATUS_NOT_AVAIL_NOT_PROD;
                            totalIncomplete = true;
                        }
                        else if (!iv->isProducing(*mUtcTimestamp, rec)) {
                            if (MQTT_STATUS_AVAIL_PROD == status)
                                status = MQTT_STATUS_AVAIL_NOT_PROD;
                        }
                        snprintf(topic, 32 + MAX_NAME_LENGTH, "%s/available_text", iv->config->name);
                        snprintf(val, 40, "%s%s%s%s",
                            (status == MQTT_STATUS_NOT_AVAIL_NOT_PROD) ? "not yet " : "",
                            "available and ",
                            (status == MQTT_STATUS_AVAIL_NOT_PROD) ? "not " : "",
                            (status == MQTT_STATUS_NOT_AVAIL_NOT_PROD) ? "" : "producing"
                        );
                        publish(topic, val);

                        snprintf(topic, 32 + MAX_NAME_LENGTH, "%s/available", iv->config->name);
                        snprintf(val, 40, "%d", status);
                        publish(topic, val);

                        snprintf(topic, 32 + MAX_NAME_LENGTH, "%s/last_success", iv->config->name);
                        snprintf(val, 40, "%i", iv->getLastTs(rec) * 1000);
                        publish(topic, val);
                    }

                    // data
                    if(iv->isAvailable(*mUtcTimestamp, rec)) {
                        for (uint8_t i = 0; i < rec->length; i++) {
                            snprintf(topic, 32 + MAX_NAME_LENGTH, "%s/ch%d/%s", iv->config->name, rec->assign[i].ch, fields[rec->assign[i].fieldId]);
                            snprintf(val, 40, "%.3f", iv->getValue(i, rec));
                            publish(topic, val);

                            // calculate total values for RealTimeRunData_Debug
                            if (mSendList.front() == RealTimeRunData_Debug) {
                                if (CH0 == rec->assign[i].ch) {
                                    switch (rec->assign[i].fieldId) {
                                        case FLD_PAC:
                                            total[0] += iv->getValue(i, rec);
                                            break;
                                        case FLD_YT:
                                            total[1] += iv->getValue(i, rec);
                                            break;
                                        case FLD_YD:
                                            total[2] += iv->getValue(i, rec);
                                            break;
                                        case FLD_PDC:
                                            total[3] += iv->getValue(i, rec);
                                            break;
                                    }
                                }
                                sendTotal = true;
                            }
                            yield();
                        }
                    }
                }

                mSendList.pop(); // remove from list once all inverters were processed

                if ((true == sendTotal) && (false == totalIncomplete)) {
                    uint8_t fieldId;
                    for (uint8_t i = 0; i < 4; i++) {
                        switch (i) {
                            default:
                            case 0:
                                fieldId = FLD_PAC;
                                break;
                            case 1:
                                fieldId = FLD_YT;
                                break;
                            case 2:
                                fieldId = FLD_YD;
                                break;
                            case 3:
                                fieldId = FLD_PDC;
                                break;
                        }
                        snprintf(topic, 32 + MAX_NAME_LENGTH, "total/%s", fields[fieldId]);
                        snprintf(val, 40, "%.3f", total[i]);
                        publish(topic, val);
                    }
                }
            }
        }

//        void cbMqtt(char *topic, byte *payload, unsigned int length) {
//            // callback handling on subscribed devcontrol topic
//            DPRINTLN(DBG_INFO, F("cbMqtt"));
//            // subcribed topics are mTopic + "/devcontrol/#" where # is <inverter_id>/<subcmd in dec>
//            // eg. mypvsolar/devcontrol/1/11 with payload "400" --> inverter 1 active power limit 400 Watt
//            const char *token = strtok(topic, "/");
//            while (token != NULL) {
//                if (strcmp(token, "devcontrol") == 0) {
//                    token = strtok(NULL, "/");
//                    uint8_t iv_id = std::stoi(token);
//
//                    if (iv_id >= 0 && iv_id <= MAX_NUM_INVERTERS) {
//                        Inverter<> *iv = mSys->getInverterByPos(iv_id);
//                        if (NULL != iv) {
//                            if (!iv->devControlRequest) {  // still pending
//                                token = strtok(NULL, "/");
//
//                                switch (std::stoi(token)) {
//                                    // Active Power Control
//                                    case ActivePowerContr:
//                                        token = strtok(NULL, "/");  // get ControlMode aka "PowerPF.Desc" in DTU-Pro Code from topic string
//                                        if (token == NULL)          // default via mqtt ommit the LimitControlMode
//                                            iv->powerLimit[1] = AbsolutNonPersistent;
//                                        else
//                                            iv->powerLimit[1] = std::stoi(token);
//                                        if (length <= 5) {  // if (std::stoi((char*)payload) > 0) more error handling powerlimit needed?
//                                            if (iv->powerLimit[1] >= AbsolutNonPersistent && iv->powerLimit[1] <= RelativPersistent) {
//                                                iv->devControlCmd = ActivePowerContr;
//                                                iv->powerLimit[0] = std::stoi(std::string((char *)payload, (unsigned int)length));  // THX to @silversurfer
//                                                /*if (iv->powerLimit[1] & 0x0001)
//                                                    DPRINTLN(DBG_INFO, F("Power limit for inverter ") + String(iv->id) + F(" set to ") + String(iv->powerLimit[0]) + F("%"));
//                                                else
//                                                    DPRINTLN(DBG_INFO, F("Power limit for inverter ") + String(iv->id) + F(" set to ") + String(iv->powerLimit[0]) + F("W"));*/
//
//                                                DPRINTLN(DBG_INFO, F("Power limit for inverter ") + String(iv->id) + F(" set to ") + String(iv->powerLimit[0]) + String(iv->powerLimit[1] & 0x0001) ? F("%") : F("W"));
//                                            }
//                                            iv->devControlRequest = true;
//                                        } else {
//                                            DPRINTLN(DBG_INFO, F("Invalid mqtt payload recevied: ") + String((char *)payload));
//                                        }
//                                        break;
//
//                                    // Turn On
//                                    case TurnOn:
//                                        iv->devControlCmd = TurnOn;
//                                        DPRINTLN(DBG_INFO, F("Turn on inverter ") + String(iv->id));
//                                        iv->devControlRequest = true;
//                                        break;
//
//                                    // Turn Off
//                                    case TurnOff:
//                                        iv->devControlCmd = TurnOff;
//                                        DPRINTLN(DBG_INFO, F("Turn off inverter ") + String(iv->id));
//                                        iv->devControlRequest = true;
//                                        break;
//
//                                    // Restart
//                                    case Restart:
//                                        iv->devControlCmd = Restart;
//                                        DPRINTLN(DBG_INFO, F("Restart inverter ") + String(iv->id));
//                                        iv->devControlRequest = true;
//                                        break;
//
//                                    // Reactive Power Control
//                                    case ReactivePowerContr:
//                                        iv->devControlCmd = ReactivePowerContr;
//                                        if (true) {  // if (std::stoi((char*)payload) > 0) error handling powerlimit needed?
//                                            iv->devControlCmd = ReactivePowerContr;
//                                            iv->powerLimit[0] = std::stoi(std::string((char *)payload, (unsigned int)length));
//                                            iv->powerLimit[1] = 0x0000;  // if reactivepower limit is set via external interface --> set it temporay
//                                            DPRINTLN(DBG_DEBUG, F("Reactivepower limit for inverter ") + String(iv->id) + F(" set to ") + String(iv->powerLimit[0]) + F("W"));
//                                            iv->devControlRequest = true;
//                                        }
//                                        break;
//
//                                    // Set Power Factor
//                                    case PFSet:
//                                        // iv->devControlCmd = PFSet;
//                                        // uint16_t power_factor = std::stoi(strtok(NULL, "/"));
//                                        DPRINTLN(DBG_INFO, F("Set Power Factor not implemented for inverter ") + String(iv->id));
//                                        break;
//
//                                    // CleanState lock & alarm
//                                    case CleanState_LockAndAlarm:
//                                        iv->devControlCmd = CleanState_LockAndAlarm;
//                                        DPRINTLN(DBG_INFO, F("CleanState lock & alarm for inverter ") + String(iv->id));
//                                        iv->devControlRequest = true;
//                                        break;
//
//                                    default:
//                                        DPRINTLN(DBG_INFO, "Not implemented");
//                                        break;
//                                }
//                            }
//                        }
//                    }
//                    break;
//                }
//                token = strtok(NULL, "/");
//            }
//            DPRINTLN(DBG_INFO, F("app::cbMqtt finished"));
//        }

        espMqttClient mClient;
        cfgMqtt_t *mCfgMqtt;
        WiFiEventHandler mHWifiCon, mHWifiDiscon;

        uint32_t *mSunrise, *mSunset;
        HMSYSTEM *mSys;
        uint32_t *mUtcTimestamp;
        uint32_t mTxCnt;
        std::queue<uint8_t> mSendList;

        const char *mDevName, *mVersion;
        //uint32_t mLastReconnect;
};

#endif /*__PUB_MQTT_H__*/