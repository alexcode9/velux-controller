#include "remotes.h"

#include "config.h"

// One button is driven at a time, across both remotes.
//
// Overflow policy, with at most one queued command per remote:
// - Open/Close replaces a queued Open/Close for the same remote.
// - Open/Close is dropped when that remote already has a queued STOP.
// - STOP removes any queued command for that remote and is inserted ahead of
//   the other remote's non-STOP commands, behind STOPs already at the front.
// - STOP for the remote that is currently pulsing a non-STOP is not queued.
//   That pulse is released immediately and STOP starts with no gap.
// - A STOP that arrives after the pin is already released waits out the gap.
// - If an Open/Close still cannot fit, it is dropped.
// - If a STOP still cannot fit, the oldest non-STOP is dropped to make room.
//   If every entry is a STOP, the oldest entry is dropped.

namespace {

enum class Phase : uint8_t { Idle, Pulsing, Gap };

struct Command {
  uint8_t remote;
  RemoteAction action;
};

Command queue_[kCommandQueueCapacity];
uint8_t count_ = 0;

Phase phase_ = Phase::Idle;
uint8_t activeRemote_ = 0;
RemoteAction activeAction_ = RemoteAction::Stop;
uint8_t activePin_ = 0;
uint32_t phaseStartedMs_ = 0;

const char* actionName(RemoteAction action) {
  switch (action) {
    case RemoteAction::Open:
      return "OPEN";
    case RemoteAction::Close:
      return "CLOSE";
    case RemoteAction::Stop:
      return "STOP";
  }
  return "?";
}

uint8_t pinFor(uint8_t remote, RemoteAction action) {
  switch (action) {
    case RemoteAction::Open:
      return kRemoteUpPin[remote];
    case RemoteAction::Close:
      return kRemoteDownPin[remote];
    case RemoteAction::Stop:
      return kRemoteStopPin[remote];
  }
  return kRemoteStopPin[remote];
}

// Latch low while the driver is still off, then enable the driver.
void pressPin(uint8_t pin) {
  digitalWrite(pin, LOW);
  pinMode(pin, OUTPUT);
}

// High impedance. GPIO16 uses INPUT, never INPUT_PULLDOWN_16.
void releasePin(uint8_t pin) {
  pinMode(pin, INPUT);
}

void removeAt(uint8_t index) {
  for (uint8_t i = index; i + 1 < count_; ++i) {
    queue_[i] = queue_[i + 1];
  }
  --count_;
}

void removeRemote(uint8_t remote) {
  uint8_t i = 0;
  while (i < count_) {
    if (queue_[i].remote == remote) {
      removeAt(i);
    } else {
      ++i;
    }
  }
}

int findRemote(uint8_t remote) {
  for (uint8_t i = 0; i < count_; ++i) {
    if (queue_[i].remote == remote) {
      return i;
    }
  }
  return -1;
}

bool dropOldestNonStop() {
  for (uint8_t i = 0; i < count_; ++i) {
    if (queue_[i].action != RemoteAction::Stop) {
      Serial.printf("queue full; dropped %s for remote %u to queue STOP\n",
                    actionName(queue_[i].action),
                    queue_[i].remote + 1);
      removeAt(i);
      return true;
    }
  }
  return false;
}

void insertStop(uint8_t remote) {
  uint8_t index = 0;
  while (index < count_ && queue_[index].action == RemoteAction::Stop) {
    ++index;
  }
  for (uint8_t i = count_; i > index; --i) {
    queue_[i] = queue_[i - 1];
  }
  queue_[index].remote = remote;
  queue_[index].action = RemoteAction::Stop;
  ++count_;
}

void startPulse(uint8_t remote, RemoteAction action, uint32_t now) {
  const uint8_t pin = pinFor(remote, action);
  pressPin(pin);
  phase_ = Phase::Pulsing;
  activeRemote_ = remote;
  activeAction_ = action;
  activePin_ = pin;
  phaseStartedMs_ = now;
  Serial.printf("press remote %u %s GPIO%u\n", remote + 1, actionName(action), pin);
}

void enqueueStop(uint8_t remote, uint32_t now) {
  if (phase_ == Phase::Pulsing && activeRemote_ == remote) {
    removeRemote(remote);
    if (activeAction_ == RemoteAction::Stop) {
      return;
    }
    Serial.printf("preempt remote %u with STOP\n", remote + 1);
    releasePin(activePin_);
    Serial.printf("release remote %u GPIO%u\n", remote + 1, activePin_);
    startPulse(remote, RemoteAction::Stop, now);
    return;
  }

  removeRemote(remote);
  if (count_ >= kCommandQueueCapacity) {
    if (!dropOldestNonStop() && count_ > 0) {
      Serial.printf("queue full; dropped %s for remote %u to queue STOP\n",
                    actionName(queue_[0].action),
                    queue_[0].remote + 1);
      removeAt(0);
    }
  }
  if (count_ >= kCommandQueueCapacity) {
    Serial.printf("queue full; dropped STOP for remote %u\n", remote + 1);
    return;
  }
  insertStop(remote);
}

void enqueueMove(uint8_t remote, RemoteAction action) {
  const int existing = findRemote(remote);
  if (existing >= 0) {
    if (queue_[existing].action == RemoteAction::Stop) {
      Serial.printf("dropped %s for remote %u; STOP is queued\n",
                    actionName(action),
                    remote + 1);
      return;
    }
    queue_[existing].action = action;
    return;
  }
  if (count_ >= kCommandQueueCapacity) {
    Serial.printf("queue full; dropped %s for remote %u\n", actionName(action), remote + 1);
    return;
  }
  queue_[count_].remote = remote;
  queue_[count_].action = action;
  ++count_;
}

}  // namespace

void remotesBegin() {
  const uint8_t pins[] = {
      kRemoteUpPin[0],   kRemoteStopPin[0], kRemoteDownPin[0],
      kRemoteUpPin[1],   kRemoteStopPin[1], kRemoteDownPin[1],
  };
  for (uint8_t pin : pins) {
    // Preset the output latch before any later pinMode(OUTPUT). Writing LOW
    // does not enable the driver. INPUT leaves the pad high impedance, and
    // on GPIO16 it does not enable the pull-down.
    digitalWrite(pin, LOW);
    pinMode(pin, INPUT);
  }
}

void remotesUpdate(uint32_t now) {
  if (phase_ == Phase::Pulsing) {
    if (now - phaseStartedMs_ < kButtonPulseMs) {
      return;
    }
    releasePin(activePin_);
    Serial.printf("release remote %u GPIO%u\n", activeRemote_ + 1, activePin_);
    phase_ = Phase::Gap;
    phaseStartedMs_ = now;
  }

  if (phase_ == Phase::Gap) {
    if (now - phaseStartedMs_ < kInterCommandGapMs) {
      return;
    }
    phase_ = Phase::Idle;
  }

  if (phase_ != Phase::Idle || count_ == 0) {
    return;
  }

  const Command next = queue_[0];
  removeAt(0);
  startPulse(next.remote, next.action, now);
}

bool remotesHolding() {
  return phase_ == Phase::Pulsing;
}

void remotesEnqueue(uint8_t remoteIndex, RemoteAction action) {
  if (remoteIndex >= kRemoteCount) {
    return;
  }
  if (action == RemoteAction::Stop) {
    enqueueStop(remoteIndex, millis());
    return;
  }
  enqueueMove(remoteIndex, action);
}
