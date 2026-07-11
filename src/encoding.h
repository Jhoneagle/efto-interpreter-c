#ifndef clox_encoding_h
#define clox_encoding_h

#include "common.h"

// Text encodings supported by string.encode / string.decode. The interpreter's
// internal string representation is always UTF-8; these codecs convert at the
// bytes boundary.
typedef enum {
  ENC_UTF8,
  ENC_UTF16LE,
  ENC_UTF16BE,
  ENC_UTF32LE,
  ENC_UTF32BE,
  ENC_LATIN1,
  ENC_ASCII,
  ENC_UNKNOWN
} Encoding;

// Resolve an encoding name (case-insensitive; '-', '_', spaces ignored), e.g.
// "UTF-8", "utf16le", "ISO-8859-1", "US-ASCII". Returns ENC_UNKNOWN if unknown.
Encoding encodingFromName(const char* name, int len);

// Decode the UTF-8 sequence at s[i]. Sets *cp and returns its byte length
// (1..4), or 0 on malformed / overlong / surrogate / out-of-range input.
int utf8DecodeAt(const uint8_t* s, int len, int i, uint32_t* cp);

// Encode a codepoint into out[0..3]. Returns byte count (1..4), or 0 if the
// codepoint is invalid (> 0x10FFFF or a surrogate).
int utf8Encode(uint32_t cp, uint8_t out[4]);

// Convert internal UTF-8 (utf8[0..len)) into `enc`. On success returns a
// malloc'd buffer (caller frees) and sets *outLen. On failure returns NULL and
// sets *errMsg to a static description. Empty input yields a non-NULL buffer.
uint8_t* encodeFromUtf8(const uint8_t* utf8, int len, Encoding enc,
                        int* outLen, const char** errMsg);

// Convert bytes in `enc` into internal UTF-8. Same contract as encodeFromUtf8.
uint8_t* decodeToUtf8(const uint8_t* data, int len, Encoding enc,
                      int* outLen, const char** errMsg);

#endif
