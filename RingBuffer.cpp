#include "RingBuffer.h"
#include <esp_heap_caps.h>

RingBuffer::RingBuffer(size_t size)
  : capacity(size), writeIndex(0), readIndex(0), availableCount(0) {
  // Allocate large audio buffer in PSRAM
  buffer = (int16_t*)heap_caps_malloc(size * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  mutex = xSemaphoreCreateMutex();
}

RingBuffer::~RingBuffer() {
  if (buffer) free(buffer);
  vSemaphoreDelete(mutex);
}

bool RingBuffer::write(int16_t* data, size_t count) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  for (size_t i = 0; i < count; i++) {
    buffer[writeIndex] = data[i];
    writeIndex = (writeIndex + 1) % capacity;
    if (availableCount < capacity) {
      availableCount++;
    } else {
      // Overwrite oldest sample
      readIndex = (readIndex + 1) % capacity;
    }
  }
  xSemaphoreGive(mutex);
  return true;
}

size_t RingBuffer::read(int16_t* dest, size_t count) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  size_t toRead = min(count, availableCount);
  for (size_t i = 0; i < toRead; i++) {
    dest[i] = buffer[readIndex];
    readIndex = (readIndex + 1) % capacity;
  }
  availableCount -= toRead;
  xSemaphoreGive(mutex);
  return toRead;
}

// NEW: peek the LAST 'count' samples without removing them
bool RingBuffer::peekLast(int16_t* dest, size_t count) {
  xSemaphoreTake(mutex, portMAX_DELAY);

  if (availableCount == 0) {
    xSemaphoreGive(mutex);
    return false;
  }

  if (count > availableCount) {
    count = availableCount;
  }

  // Index of the first of the last 'count' samples
  size_t start = (writeIndex + capacity - count) % capacity;

  for (size_t i = 0; i < count; i++) {
    dest[i] = buffer[(start + i) % capacity];
  }

  xSemaphoreGive(mutex);
  return true;
}

size_t RingBuffer::available() {
  xSemaphoreTake(mutex, portMAX_DELAY);
  size_t avail = availableCount;
  xSemaphoreGive(mutex);
  return avail;
}

void RingBuffer::reset() {
  xSemaphoreTake(mutex, portMAX_DELAY);
  readIndex = writeIndex = availableCount = 0;
  xSemaphoreGive(mutex);
}
