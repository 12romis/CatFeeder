#pragma once

void bleInit();

// Start advertising (call on button wake or when BLE window should open).
void bleStart();

// Stop advertising and disconnect any client.
void bleStop();

bool bleIsActive();

// Called from loop(): closes the BLE window after BLE_WINDOW_SEC.
void bleTick();
