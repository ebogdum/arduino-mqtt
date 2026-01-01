#include <string.h>

#include "helpers.h"

// Simpler, faster bit reading using standard shift/mask
uint8_t lwmqtt_read_bits(uint8_t byte, int pos, int num) {
  return (byte >> pos) & ((1u << num) - 1);
}

// Simpler, faster bit writing
void lwmqtt_write_bits(uint8_t *byte, uint8_t value, int pos, int num) {
  uint8_t mask = ((1u << num) - 1) << pos;
  *byte = (*byte & ~mask) | ((value << pos) & mask);
}

lwmqtt_err_t lwmqtt_read_data(uint8_t **buf, const uint8_t *buf_end, uint8_t **data, size_t len) {
  // Early exit for zero length (common case)
  if (LWMQTT_UNLIKELY(len == 0)) {
    *data = NULL;
    return LWMQTT_SUCCESS;
  }

  // Check buffer capacity
  if (LWMQTT_UNLIKELY((size_t)(buf_end - *buf) < len)) {
    return LWMQTT_BUFFER_TOO_SHORT;
  }

  // Read data and advance pointer
  *data = *buf;
  *buf += len;

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_write_data(uint8_t **buf, const uint8_t *buf_end, uint8_t *data, size_t len) {
  // Early exit for zero length
  if (LWMQTT_UNLIKELY(len == 0)) {
    return LWMQTT_SUCCESS;
  }

  // Check buffer capacity
  if (LWMQTT_UNLIKELY((size_t)(buf_end - *buf) < len)) {
    return LWMQTT_BUFFER_TOO_SHORT;
  }

  // Write data and advance pointer
  memcpy(*buf, data, len);
  *buf += len;

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_read_num(uint8_t **buf, const uint8_t *buf_end, uint16_t *num) {
  if (LWMQTT_UNLIKELY((size_t)(buf_end - *buf) < 2)) {
    *num = 0;
    return LWMQTT_BUFFER_TOO_SHORT;
  }

  // Read big-endian uint16 using bit shift
  *num = ((uint16_t)(*buf)[0] << 8) | (*buf)[1];
  *buf += 2;

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_write_num(uint8_t **buf, const uint8_t *buf_end, uint16_t num) {
  if (LWMQTT_UNLIKELY((size_t)(buf_end - *buf) < 2)) {
    return LWMQTT_BUFFER_TOO_SHORT;
  }

  // Write big-endian uint16 using bit shift
  (*buf)[0] = (uint8_t)(num >> 8);
  (*buf)[1] = (uint8_t)num;
  *buf += 2;

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_read_string(uint8_t **buf, const uint8_t *buf_end, lwmqtt_string_t *str) {
  uint16_t len;
  lwmqtt_err_t err = lwmqtt_read_num(buf, buf_end, &len);
  if (LWMQTT_UNLIKELY(err != LWMQTT_SUCCESS)) return err;

  err = lwmqtt_read_data(buf, buf_end, (uint8_t **)&str->data, len);
  if (LWMQTT_UNLIKELY(err != LWMQTT_SUCCESS)) return err;

  str->len = len;
  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_write_string(uint8_t **buf, const uint8_t *buf_end, lwmqtt_string_t str) {
  lwmqtt_err_t err = lwmqtt_write_num(buf, buf_end, str.len);
  if (LWMQTT_UNLIKELY(err != LWMQTT_SUCCESS)) return err;

  return lwmqtt_write_data(buf, buf_end, (uint8_t *)str.data, str.len);
}

lwmqtt_err_t lwmqtt_read_byte(uint8_t **buf, const uint8_t *buf_end, uint8_t *byte) {
  if (LWMQTT_UNLIKELY(buf_end <= *buf)) {
    *byte = 0;
    return LWMQTT_BUFFER_TOO_SHORT;
  }

  *byte = *(*buf)++;
  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_write_byte(uint8_t **buf, const uint8_t *buf_end, uint8_t byte) {
  if (LWMQTT_UNLIKELY(buf_end <= *buf)) {
    return LWMQTT_BUFFER_TOO_SHORT;
  }

  *(*buf)++ = byte;
  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_varnum_length(uint32_t varnum, int *len) {
  // Use single comparison chain - compiler optimizes this well
  if (varnum < 128) {
    *len = 1;
  } else if (varnum < 16384) {
    *len = 2;
  } else if (varnum < 2097152) {  // Fixed: was 2097151, should be 2097152
    *len = 3;
  } else if (varnum < 268435456) {  // Fixed: was 268435455, should be 268435456 
    *len = 4;
  } else {
    *len = 0;
    return LWMQTT_VARNUM_OVERFLOW;
  }
  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_read_varnum(uint8_t **buf, const uint8_t *buf_end, uint32_t *varnum) {
  uint8_t byte;
  uint8_t shift = 0;
  *varnum = 0;

  do {
    // Check buffer bounds
    if (LWMQTT_UNLIKELY(*buf >= buf_end)) {
      return LWMQTT_BUFFER_TOO_SHORT;
    }

    // Check for overflow (max 4 bytes)
    if (LWMQTT_UNLIKELY(shift >= 28)) {
      return LWMQTT_VARNUM_OVERFLOW;
    }

    // Read byte and accumulate using bit shift (faster than multiply)
    byte = *(*buf)++;
    *varnum |= (uint32_t)(byte & 0x7F) << shift;
    shift += 7;
  } while (byte & 0x80);

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_write_varnum(uint8_t **buf, const uint8_t *buf_end, uint32_t varnum) {
  // Encode variable-length integer using bit operations (faster than divide/modulo)
  do {
    // Check buffer space
    if (LWMQTT_UNLIKELY(*buf >= buf_end)) {
      return LWMQTT_BUFFER_TOO_SHORT;
    }

    // Extract low 7 bits using bit mask
    uint8_t byte = varnum & 0x7F;
    varnum >>= 7;

    // Set continuation bit if more bytes follow
    if (varnum > 0) {
      byte |= 0x80;
    }

    *(*buf)++ = byte;
  } while (varnum > 0);

  return LWMQTT_SUCCESS;
}
