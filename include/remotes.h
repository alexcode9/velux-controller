#pragma once

#include <Arduino.h>

enum class RemoteAction : uint8_t { Open, Close, Stop };

// Preset every button latch low and leave the pins as inputs. Call this
// before Serial or Wi-Fi so a later press cannot glitch high.
void remotesBegin();

// Advance pulse and gap timers. Wraparound-safe. Call this even when the
// network is down so a held button is always released on time.
void remotesUpdate(uint32_t now);

// True while a button pin is actively driven low.
bool remotesHolding();

// Queue a cover command, or preempt the active pulse when this remote's
// STOP arrives mid-pulse. remoteIndex is 0 or 1.
void remotesEnqueue(uint8_t remoteIndex, RemoteAction action);
