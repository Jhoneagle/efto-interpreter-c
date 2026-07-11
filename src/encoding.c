// Text codecs for string.encode / string.decode. The interpreter's internal
// string type is UTF-8; these functions convert between UTF-8 and the supported
// byte encodings. All conversions are strict: malformed input or codepoints
// outside the target encoding's range are reported as errors (the caller turns
// them into catchable ValueErrors). errors="replace" is a possible future arg.

#include <stdlib.h>
#include <string.h>

#include "encoding.h"

Encoding encodingFromName(const char* name, int len) {
  // Normalize: lowercase, drop '-', '_', and spaces.
  char norm[32];
  int n = 0;
  for (int i = 0; i < len && n < (int)sizeof(norm) - 1; i++) {
    char c = name[i];
    if (c == '-' || c == '_' || c == ' ') continue;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    norm[n++] = c;
  }
  norm[n] = '\0';

  if (strcmp(norm, "utf8") == 0) return ENC_UTF8;
  if (strcmp(norm, "utf16le") == 0) return ENC_UTF16LE;
  if (strcmp(norm, "utf16be") == 0) return ENC_UTF16BE;
  if (strcmp(norm, "utf32le") == 0) return ENC_UTF32LE;
  if (strcmp(norm, "utf32be") == 0) return ENC_UTF32BE;
  if (strcmp(norm, "latin1") == 0 || strcmp(norm, "iso88591") == 0)
    return ENC_LATIN1;
  if (strcmp(norm, "ascii") == 0 || strcmp(norm, "usascii") == 0)
    return ENC_ASCII;
  return ENC_UNKNOWN;
}

int utf8DecodeAt(const uint8_t* s, int len, int i, uint32_t* cp) {
  uint8_t c = s[i];
  if (c < 0x80) {
    *cp = c;
    return 1;
  } else if ((c & 0xE0) == 0xC0) {
    if (i + 1 >= len) return 0;
    uint8_t c1 = s[i + 1];
    if ((c1 & 0xC0) != 0x80) return 0;
    uint32_t v = ((uint32_t)(c & 0x1F) << 6) | (c1 & 0x3F);
    if (v < 0x80) return 0;  // overlong
    *cp = v;
    return 2;
  } else if ((c & 0xF0) == 0xE0) {
    if (i + 2 >= len) return 0;
    uint8_t c1 = s[i + 1], c2 = s[i + 2];
    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return 0;
    uint32_t v = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(c1 & 0x3F) << 6) |
                 (c2 & 0x3F);
    if (v < 0x800) return 0;                     // overlong
    if (v >= 0xD800 && v <= 0xDFFF) return 0;     // surrogate
    *cp = v;
    return 3;
  } else if ((c & 0xF8) == 0xF0) {
    if (i + 3 >= len) return 0;
    uint8_t c1 = s[i + 1], c2 = s[i + 2], c3 = s[i + 3];
    if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
      return 0;
    uint32_t v = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(c1 & 0x3F) << 12) |
                 ((uint32_t)(c2 & 0x3F) << 6) | (c3 & 0x3F);
    if (v < 0x10000 || v > 0x10FFFF) return 0;    // overlong / out of range
    *cp = v;
    return 4;
  }
  return 0;
}

int utf8Encode(uint32_t cp, uint8_t out[4]) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  if (cp < 0x80) {
    out[0] = (uint8_t)cp;
    return 1;
  } else if (cp < 0x800) {
    out[0] = (uint8_t)(0xC0 | (cp >> 6));
    out[1] = (uint8_t)(0x80 | (cp & 0x3F));
    return 2;
  } else if (cp < 0x10000) {
    out[0] = (uint8_t)(0xE0 | (cp >> 12));
    out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (uint8_t)(0x80 | (cp & 0x3F));
    return 3;
  } else {
    out[0] = (uint8_t)(0xF0 | (cp >> 18));
    out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
  }
}

// --- growable byte buffer ---

typedef struct {
  uint8_t* data;
  int len;
  int cap;
} EncBuf;

static bool ebEnsure(EncBuf* b, int extra) {
  if (b->len + extra <= b->cap) return true;
  int cap = b->cap < 16 ? 16 : b->cap;
  while (cap < b->len + extra) cap *= 2;
  uint8_t* grown = (uint8_t*)realloc(b->data, cap);
  if (grown == NULL) return false;
  b->data = grown;
  b->cap = cap;
  return true;
}

static void put16(uint8_t* p, uint16_t v, bool be) {
  if (be) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
  else { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
}

static void put32(uint8_t* p, uint32_t v, bool be) {
  if (be) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
  } else {
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
  }
}

static uint16_t read16(const uint8_t* p, bool be) {
  return be ? (uint16_t)((p[0] << 8) | p[1])
            : (uint16_t)((p[1] << 8) | p[0]);
}

static uint32_t read32(const uint8_t* p, bool be) {
  return be ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                  ((uint32_t)p[2] << 8) | p[3]
            : ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
                  ((uint32_t)p[1] << 8) | p[0];
}

uint8_t* encodeFromUtf8(const uint8_t* utf8, int len, Encoding enc,
                        int* outLen, const char** errMsg) {
  EncBuf b = { NULL, 0, 0 };
  if (!ebEnsure(&b, len + 4)) { *errMsg = "out of memory"; return NULL; }

  int i = 0;
  while (i < len) {
    uint32_t cp;
    int sz = utf8DecodeAt(utf8, len, i, &cp);
    if (sz == 0) { free(b.data); *errMsg = "invalid UTF-8 input"; return NULL; }
    i += sz;

    if (!ebEnsure(&b, 4)) { free(b.data); *errMsg = "out of memory"; return NULL; }
    switch (enc) {
      case ENC_UTF8: {
        uint8_t tmp[4];
        int k = utf8Encode(cp, tmp);
        memcpy(b.data + b.len, tmp, k);
        b.len += k;
        break;
      }
      case ENC_LATIN1:
        if (cp > 0xFF) {
          free(b.data);
          *errMsg = "codepoint out of Latin-1 range";
          return NULL;
        }
        b.data[b.len++] = (uint8_t)cp;
        break;
      case ENC_ASCII:
        if (cp > 0x7F) {
          free(b.data);
          *errMsg = "codepoint out of ASCII range";
          return NULL;
        }
        b.data[b.len++] = (uint8_t)cp;
        break;
      case ENC_UTF16LE:
      case ENC_UTF16BE: {
        bool be = (enc == ENC_UTF16BE);
        if (cp <= 0xFFFF) {
          put16(b.data + b.len, (uint16_t)cp, be);
          b.len += 2;
        } else {
          uint32_t v = cp - 0x10000;
          put16(b.data + b.len, (uint16_t)(0xD800 + (v >> 10)), be);
          b.len += 2;
          put16(b.data + b.len, (uint16_t)(0xDC00 + (v & 0x3FF)), be);
          b.len += 2;
        }
        break;
      }
      case ENC_UTF32LE:
      case ENC_UTF32BE:
        put32(b.data + b.len, cp, enc == ENC_UTF32BE);
        b.len += 4;
        break;
      default:
        free(b.data);
        *errMsg = "unknown encoding";
        return NULL;
    }
  }

  *outLen = b.len;
  return b.data;
}

// Append a codepoint as UTF-8 to the buffer. Returns false (freeing on failure
// is the caller's job via the ENC_FAIL pattern) on invalid codepoint / OOM.
static bool emitUtf8(EncBuf* b, uint32_t cp) {
  uint8_t tmp[4];
  int k = utf8Encode(cp, tmp);
  if (k == 0) return false;
  if (!ebEnsure(b, k)) return false;
  memcpy(b->data + b->len, tmp, k);
  b->len += k;
  return true;
}

uint8_t* decodeToUtf8(const uint8_t* data, int len, Encoding enc,
                      int* outLen, const char** errMsg) {
  EncBuf b = { NULL, 0, 0 };
  if (!ebEnsure(&b, len + 4)) { *errMsg = "out of memory"; return NULL; }

  switch (enc) {
    case ENC_UTF8: {
      int i = 0;
      while (i < len) {
        uint32_t cp;
        int sz = utf8DecodeAt(data, len, i, &cp);
        if (sz == 0) { free(b.data); *errMsg = "invalid UTF-8"; return NULL; }
        i += sz;
        if (!emitUtf8(&b, cp)) { free(b.data); *errMsg = "invalid codepoint"; return NULL; }
      }
      break;
    }
    case ENC_LATIN1:
      for (int i = 0; i < len; i++) {
        if (!emitUtf8(&b, data[i])) { free(b.data); *errMsg = "invalid codepoint"; return NULL; }
      }
      break;
    case ENC_ASCII:
      for (int i = 0; i < len; i++) {
        if (data[i] > 0x7F) { free(b.data); *errMsg = "byte out of ASCII range"; return NULL; }
        if (!emitUtf8(&b, data[i])) { free(b.data); *errMsg = "invalid codepoint"; return NULL; }
      }
      break;
    case ENC_UTF16LE:
    case ENC_UTF16BE: {
      if (len % 2 != 0) { free(b.data); *errMsg = "UTF-16 length not a multiple of 2"; return NULL; }
      bool be = (enc == ENC_UTF16BE);
      int i = 0;
      while (i < len) {
        uint32_t u = read16(data + i, be);
        i += 2;
        if (u >= 0xD800 && u <= 0xDBFF) {
          if (i + 2 > len) { free(b.data); *errMsg = "unpaired high surrogate"; return NULL; }
          uint32_t lo = read16(data + i, be);
          if (lo < 0xDC00 || lo > 0xDFFF) { free(b.data); *errMsg = "invalid surrogate pair"; return NULL; }
          i += 2;
          uint32_t cp = 0x10000 + (((u - 0xD800) << 10) | (lo - 0xDC00));
          if (!emitUtf8(&b, cp)) { free(b.data); *errMsg = "invalid codepoint"; return NULL; }
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
          free(b.data); *errMsg = "unexpected low surrogate"; return NULL;
        } else {
          if (!emitUtf8(&b, u)) { free(b.data); *errMsg = "invalid codepoint"; return NULL; }
        }
      }
      break;
    }
    case ENC_UTF32LE:
    case ENC_UTF32BE: {
      if (len % 4 != 0) { free(b.data); *errMsg = "UTF-32 length not a multiple of 4"; return NULL; }
      bool be = (enc == ENC_UTF32BE);
      for (int i = 0; i < len; i += 4) {
        uint32_t cp = read32(data + i, be);
        if (!emitUtf8(&b, cp)) { free(b.data); *errMsg = "invalid codepoint"; return NULL; }
      }
      break;
    }
    default:
      free(b.data);
      *errMsg = "unknown encoding";
      return NULL;
  }

  *outLen = b.len;
  return b.data;
}
