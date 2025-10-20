#include <thread>
#include <volimem/x86constants.h>

#include "utils.h"

void sleep_ms(usize ms) { std::this_thread::sleep_for(Milliseconds(ms)); }
