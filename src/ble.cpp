#include "config.h"
#if BLE_ENABLED

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "ble.h"
#include "dosing.h"
#include "schedule.h"

// UUIDs — 128-bit, custom service. Finalize when the app is built.
#define SVC_UUID    "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CMD_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a8"  // write
#define STATUS_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"  // read + notify

// Opcodes — minimal working set for development (extend when app is designed)
#define OP_FEED_NOW  0x01
#define OP_SET_TIME  0x02  // payload: 4 bytes little-endian unix epoch

static NimBLEServer*         pServer     = nullptr;
static NimBLECharacteristic* pStatusChar = nullptr;
static bool active    = false;
static uint32_t startMs = 0;

class CmdCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        auto val = c->getValue();
        if (val.size() == 0) return;

        uint8_t op = (uint8_t)val[0];
        switch (op) {
            case OP_FEED_NOW:
                dosingFeed(FeedMode::BUTTON);
                break;
            case OP_SET_TIME:
                if (val.size() >= 5) {
                    uint32_t epoch = (uint8_t)val[1]
                        | ((uint8_t)val[2] << 8)
                        | ((uint8_t)val[3] << 16)
                        | ((uint8_t)val[4] << 24);
                    scheduleSetTime(epoch);
                }
                break;
            default:
                ESP_LOGW("ble", "unknown opcode 0x%02X", op);
                break;
        }
    }
};

void bleInit() {
    NimBLEDevice::init(BLE_DEVICE_NAME);
    pServer = NimBLEDevice::createServer();

    auto* svc = pServer->createService(SVC_UUID);

    auto* cmd = svc->createCharacteristic(CMD_UUID, NIMBLE_PROPERTY::WRITE);
    cmd->setCallbacks(new CmdCallbacks());

    pStatusChar = svc->createCharacteristic(
        STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    svc->start();
    ESP_LOGI("ble", "GATT server ready");
}

void bleStart() {
    NimBLEDevice::getAdvertising()->start();
    active  = true;
    startMs = millis();
    ESP_LOGI("ble", "advertising started (%us window)", BLE_WINDOW_SEC);
}

void bleStop() {
    NimBLEDevice::getAdvertising()->stop();
    // Disconnect all active clients; NimBLE 1.x exposes per-connection disconnect.
    uint8_t count = pServer->getConnectedCount();
    for (uint8_t i = 0; i < count; i++) {
        pServer->disconnect(pServer->getPeerInfo(i).getConnHandle());
    }
    active = false;
    ESP_LOGI("ble", "advertising stopped");
}

bool bleIsActive() { return active; }

void bleTick() {
    if (!active) return;
    if (millis() - startMs >= (uint32_t)BLE_WINDOW_SEC * 1000) {
        ESP_LOGI("ble", "window expired");
        bleStop();
    }
}

#endif // BLE_ENABLED
