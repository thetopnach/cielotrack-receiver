#include "health.h"

#include <stddef.h>

bool health_counter_moved(uint32_t *mark, uint32_t now) {
    if (mark == NULL) return false;
    /* Inequality rather than greater-than: these are unsigned and free to wrap, and a
     * wrapped counter has still moved. Treating a wrap as "no change" would go quiet at
     * exactly the moment a receiver had been running long enough to be worth watching. */
    bool moved = (*mark != now);
    *mark = now;
    return moved;
}
