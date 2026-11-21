#include "model/SnapshotStore.h"
#include <new>

namespace {
Snapshot* work = nullptr;
Snapshot* published = nullptr;
SemaphoreHandle_t mtx = nullptr;

Snapshot* alloc() {
  void* p = ps_malloc(sizeof(Snapshot));
  if (!p) p = malloc(sizeof(Snapshot));
  return new (p) Snapshot();
}
}  // namespace

namespace SnapshotStore {

void begin() {
  work = alloc();
  published = alloc();
  mtx = xSemaphoreCreateMutex();
}

void update(const std::function<void(Snapshot&)>& fn) {
  fn(*work);
  xSemaphoreTake(mtx, portMAX_DELAY);
  memcpy((void*)published, (const void*)work, sizeof(Snapshot));
  xSemaphoreGive(mtx);
}

void updateIf(const std::function<bool(Snapshot&)>& fn) {
  if (!fn(*work)) return;
  xSemaphoreTake(mtx, portMAX_DELAY);
  memcpy((void*)published, (const void*)work, sizeof(Snapshot));
  xSemaphoreGive(mtx);
}

void copy(Snapshot& out) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  memcpy((void*)&out, (const void*)published, sizeof(Snapshot));
  xSemaphoreGive(mtx);
}

uint32_t seq() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  uint32_t v = published->seq;
  xSemaphoreGive(mtx);
  return v;
}

uint8_t moduleCount() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  uint8_t v = published->module_count;
  xSemaphoreGive(mtx);
  return v;
}

}  // namespace SnapshotStore
