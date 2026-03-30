#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <Arduino.h>

class RingBuffer {
private:
  int16_t* buffer;
  size_t capacity;
  size_t writeIndex;
  size_t readIndex;
  size_t availableCount;
  SemaphoreHandle_t mutex;

public:
  RingBuffer(size_t size);
  ~RingBuffer();

  // Write samples into buffer (advances writeIndex, may overwrite oldest)
  bool write(int16_t* data, size_t count);

  // Read and REMOVE up to count samples (advances readIndex, reduces availableCount)
  size_t read(int16_t* dest, size_t count);

  // NEW: Peek the LAST 'count' samples WITHOUT removing them
  bool peekLast(int16_t* dest, size_t count);

  // How many samples are currently stored
  size_t available();

  // Reset indices and counters
  void reset();
};

#endif
