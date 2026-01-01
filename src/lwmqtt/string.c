#include <string.h>

#include "lwmqtt.h"

lwmqtt_string_t lwmqtt_string(const char *str) {
  // check for null
  if (str == NULL) {
    return (lwmqtt_string_t){0, NULL};
  }

  // get length
  uint16_t len = (uint16_t)strlen(str);

  // check zero length
  if (len == 0) {
    return (lwmqtt_string_t){0, NULL};
  }

  return (lwmqtt_string_t){len, (char *)str};
}

int lwmqtt_strcmp(lwmqtt_string_t a, const char *b) {
  // Handle null/empty b string
  if (b == NULL || *b == '\0') {
    return (a.len == 0) ? 0 : 1;
  }

  // Get length of b without creating a struct
  size_t b_len = strlen(b);

  // Return if lengths are different
  if (a.len != b_len) {
    return (a.len < b_len) ? -1 : 1;
  }

  // Compare memory of same length
  return memcmp(a.data, b, a.len);
}
