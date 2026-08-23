#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Whether a cumulative counter moved since the last check-in, updating the mark.
 *
 * Every counter a receiver keeps runs from boot and never resets: frames seen, messages
 * decoded, contacts dropped. That makes them right for reporting scale and wrong for
 * reporting health, because "has this ever happened" and "is this happening" are
 * different questions and only the second belongs in a fault flag.
 *
 * Reading the total directly is what made the BLE board report "detections are being
 * heard but cannot be queued" for the rest of its uptime after one dense pass overflowed
 * the queue — nineteen contacts lost, hours earlier, with an empty queue and a radio
 * seeing a hundred advertisements a second. A fault flag that cannot clear itself is one
 * an operator learns to scroll past.
 *
 * The totals still go in the payload. What they must not do is decide the flag.
 */
bool health_counter_moved(uint32_t *mark, uint32_t now);
