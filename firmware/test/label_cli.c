/* Prints the C decoder's UA-type labels, one per line, so the differential test can
 * compare them with odid_decode.py's table without linking Python to C. */
#include <stdio.h>

#include "../odid_decode.h"

int main(void) {
    for (int i = 0; i <= 255; i++) {
        printf("%d\t%s\n", i, odid_ua_type_label((uint8_t)i));
    }
    return 0;
}
