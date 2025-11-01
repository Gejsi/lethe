#include <thread>

#include "utils.h"

void sleep_ms(u64 ms) { std::this_thread::sleep_for(Milliseconds(ms)); }
