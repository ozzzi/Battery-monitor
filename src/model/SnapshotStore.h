#pragma once
#include <Arduino.h>
#include <functional>
#include "model/Snapshot.h"

namespace SnapshotStore {
void begin();
void update(const std::function<void(Snapshot&)>& fn);
void updateIf(const std::function<bool(Snapshot&)>& fn);
void copy(Snapshot& out);
uint32_t seq();
uint8_t moduleCount();
}
