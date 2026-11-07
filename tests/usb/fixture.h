#pragma once
#include <cstdint>
struct Endpoint { uint8_t* buffer = nullptr; uint32_t size = 0,received = 0; uint32_t flushed = 0; bool open = false,stalled = false,active = false; };
extern Endpoint in[16],out[16];
extern bool busy, fail_start;
extern uint8_t busy_endpoint;

struct _USBD_HandleTypeDef;
void CompleteIn(_USBD_HandleTypeDef& device, uint8_t endpoint);
