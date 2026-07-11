// JSON encode/decode for Efto values.
//
// Mapping: nil <-> null, bool, int (%lld) / double (%g) <-> number,
// string <-> string (UTF-8), array <-> array, map <-> object (string keys).
//
// Errors are raised as catchable ValueError via raiseError(); the enclosing
// native returns and the VM throws the pending exception. Malformed parse input
// and non-serializable values (non-string map keys, non-finite doubles, cyclic
// nesting past the depth cap) all surface as ValueError.

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "json.h"
#include "memory.h"
#include "object.h"
#include "value_table.h"
#include "vm.h"

#define JSON_MAX_DEPTH 256

// --- Growable char buffer (plain malloc; copied out via copyString) ---

typedef struct {
  char* data;
  size_t count;
  size_t capacity;
} JsonBuf;

static void jbInit(JsonBuf* b) {
  b->data = NULL;
  b->count = 0;
  b->capacity = 0;
}

static void jbFree(JsonBuf* b) {
  free(b->data);
  jbInit(b);
}

static void jbReserve(JsonBuf* b, size_t extra) {
  if (b->count + extra <= b->capacity) return;
  size_t cap = b->capacity < 16 ? 16 : b->capacity;
  while (cap < b->count + extra) cap *= 2;
  b->data = (char*)realloc(b->data, cap);
  b->capacity = cap;
}

static void jbPutc(JsonBuf* b, char c) {
  jbReserve(b, 1);
  b->data[b->count++] = c;
}

static void jbPut(JsonBuf* b, const char* s, size_t n) {
  jbReserve(b, n);
  memcpy(b->data + b->count, s, n);
  b->count += n;
}

static void jbPuts(JsonBuf* b, const char* s) {
  jbPut(b, s, strlen(s));
}

static void jbCodepoint(JsonBuf* b, uint32_t cp) {
  if (cp <= 0x7F) {
    jbPutc(b, (char)cp);
  } else if (cp <= 0x7FF) {
    jbPutc(b, (char)(0xC0 | (cp >> 6)));
    jbPutc(b, (char)(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    jbPutc(b, (char)(0xE0 | (cp >> 12)));
    jbPutc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    jbPutc(b, (char)(0x80 | (cp & 0x3F)));
  } else {
    jbPutc(b, (char)(0xF0 | (cp >> 18)));
    jbPutc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
    jbPutc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    jbPutc(b, (char)(0x80 | (cp & 0x3F)));
  }
}

// ===================== stringify =====================

static bool jsonWriteValue(JsonBuf* b, Value v, int indent, int depth);

static void jsonWriteString(JsonBuf* b, ObjString* s) {
  jbPutc(b, '"');
  for (int i = 0; i < s->length; i++) {
    unsigned char c = (unsigned char)s->chars[i];
    switch (c) {
      case '"':  jbPuts(b, "\\\""); break;
      case '\\': jbPuts(b, "\\\\"); break;
      case '\n': jbPuts(b, "\\n"); break;
      case '\t': jbPuts(b, "\\t"); break;
      case '\r': jbPuts(b, "\\r"); break;
      case '\b': jbPuts(b, "\\b"); break;
      case '\f': jbPuts(b, "\\f"); break;
      default:
        if (c < 0x20) {
          char esc[7];
          snprintf(esc, sizeof(esc), "\\u%04x", c);
          jbPuts(b, esc);
        } else {
          // Bytes >= 0x20, including UTF-8 continuation bytes, pass through.
          jbPutc(b, (char)c);
        }
        break;
    }
  }
  jbPutc(b, '"');
}

static void jsonWriteIndent(JsonBuf* b, int indent, int depth) {
  if (indent <= 0) return;
  jbPutc(b, '\n');
  for (int i = 0; i < indent * depth; i++) jbPutc(b, ' ');
}

static bool jsonWriteValue(JsonBuf* b, Value v, int indent, int depth) {
  if (depth > JSON_MAX_DEPTH) {
    raiseError(vm.valueErrorClass, "json: nesting too deep to serialize.");
    return false;
  }

  switch (v.type) {
    case VAL_NIL:
      jbPuts(b, "null");
      return true;
    case VAL_BOOL:
      jbPuts(b, AS_BOOL(v) ? "true" : "false");
      return true;
    case VAL_INT: {
      char num[32];
      int n = snprintf(num, sizeof(num), "%" PRId64, AS_INT(v));
      jbPut(b, num, (size_t)n);
      return true;
    }
    case VAL_DOUBLE: {
      double d = AS_DOUBLE(v);
      if (!isfinite(d)) {
        raiseError(vm.valueErrorClass,
                   "json: cannot serialize non-finite number.");
        return false;
      }
      char num[32];
      int n = snprintf(num, sizeof(num), "%g", d);
      jbPut(b, num, (size_t)n);
      return true;
    }
    case VAL_OBJ:
      break;  // handled below
  }

  if (IS_STRING(v)) {
    jsonWriteString(b, AS_STRING(v));
    return true;
  }

  if (IS_ARRAY(v)) {
    ObjArray* arr = AS_ARRAY(v);
    jbPutc(b, '[');
    for (int i = 0; i < arr->elements.count; i++) {
      if (i > 0) jbPutc(b, ',');
      jsonWriteIndent(b, indent, depth + 1);
      if (!jsonWriteValue(b, arr->elements.values[i], indent, depth + 1)) {
        return false;
      }
    }
    if (arr->elements.count > 0) jsonWriteIndent(b, indent, depth);
    jbPutc(b, ']');
    return true;
  }

  if (IS_MAP(v)) {
    ObjMap* map = AS_MAP(v);
    jbPutc(b, '{');
    int written = 0;
    for (int i = 0; i < map->entries.capacity; i++) {
      ValueEntry* e = &map->entries.entries[i];
      if (!e->occupied) continue;
      if (!IS_STRING(e->key)) {
        raiseError(vm.valueErrorClass,
                   "json: map keys must be strings.");
        return false;
      }
      if (written > 0) jbPutc(b, ',');
      jsonWriteIndent(b, indent, depth + 1);
      jsonWriteString(b, AS_STRING(e->key));
      jbPutc(b, ':');
      if (indent > 0) jbPutc(b, ' ');
      if (!jsonWriteValue(b, e->value, indent, depth + 1)) return false;
      written++;
    }
    if (written > 0) jsonWriteIndent(b, indent, depth);
    jbPutc(b, '}');
    return true;
  }

  raiseError(vm.valueErrorClass,
             "json: cannot serialize this value type.");
  return false;
}

Value jsonStringifyNative(int argCount, Value* args) {
  if (argCount < 1) {
    runtimeError("json.stringify() expects at least 1 argument.");
    return NIL_VAL;
  }

  int indent = 0;
  if (argCount >= 2 && IS_INT(args[1])) {
    int64_t requested = AS_INT(args[1]);
    if (requested < 0) requested = 0;
    if (requested > 10) requested = 10;
    indent = (int)requested;
  }

  JsonBuf buf;
  jbInit(&buf);
  if (!jsonWriteValue(&buf, args[0], indent, 0)) {
    jbFree(&buf);
    return NIL_VAL;  // raiseError already set the pending exception
  }

  ObjString* result = copyString(buf.data ? buf.data : "", (int)buf.count);
  jbFree(&buf);
  return OBJ_VAL(result);
}

// ===================== parse =====================

typedef struct {
  const char* s;
  int pos;
  int len;
} JsonParser;

static char jpPeek(JsonParser* p) {
  return p->pos < p->len ? p->s[p->pos] : '\0';
}

static void jpSkipWs(JsonParser* p) {
  while (p->pos < p->len) {
    char c = p->s[p->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
    else break;
  }
}

static bool jpMatchLiteral(JsonParser* p, const char* lit) {
  int n = (int)strlen(lit);
  if (p->pos + n <= p->len && memcmp(p->s + p->pos, lit, n) == 0) {
    p->pos += n;
    return true;
  }
  return false;
}

static bool jsonParseValue(JsonParser* p, Value* out, int depth);

static int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Parse a \uXXXX escape (the 'u' already consumed). Returns -1 on bad hex.
static int jsonParseHex4(JsonParser* p) {
  if (p->pos + 4 > p->len) return -1;
  int cp = 0;
  for (int i = 0; i < 4; i++) {
    int d = hexDigit(p->s[p->pos + i]);
    if (d < 0) return -1;
    cp = (cp << 4) | d;
  }
  p->pos += 4;
  return cp;
}

static bool jsonParseString(JsonParser* p, Value* out) {
  // Assumes current char is the opening quote.
  p->pos++;  // consume "
  JsonBuf buf;
  jbInit(&buf);

  for (;;) {
    if (p->pos >= p->len) {
      jbFree(&buf);
      raiseError(vm.valueErrorClass,
                 "json: unterminated string at %d.", p->pos);
      return false;
    }
    char c = p->s[p->pos++];
    if (c == '"') break;
    if (c == '\\') {
      if (p->pos >= p->len) {
        jbFree(&buf);
        raiseError(vm.valueErrorClass,
                   "json: unterminated escape at %d.", p->pos);
        return false;
      }
      char e = p->s[p->pos++];
      switch (e) {
        case '"':  jbPutc(&buf, '"'); break;
        case '\\': jbPutc(&buf, '\\'); break;
        case '/':  jbPutc(&buf, '/'); break;
        case 'b':  jbPutc(&buf, '\b'); break;
        case 'f':  jbPutc(&buf, '\f'); break;
        case 'n':  jbPutc(&buf, '\n'); break;
        case 'r':  jbPutc(&buf, '\r'); break;
        case 't':  jbPutc(&buf, '\t'); break;
        case 'u': {
          int cp = jsonParseHex4(p);
          if (cp < 0) {
            jbFree(&buf);
            raiseError(vm.valueErrorClass,
                       "json: invalid \\u escape at %d.", p->pos);
            return false;
          }
          // Combine a surrogate pair if a low surrogate follows.
          if (cp >= 0xD800 && cp <= 0xDBFF &&
              p->pos + 1 < p->len && p->s[p->pos] == '\\' &&
              p->s[p->pos + 1] == 'u') {
            int save = p->pos;
            p->pos += 2;  // consume "\u"
            int lo = jsonParseHex4(p);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
            } else {
              // Not a valid low surrogate; rewind and emit as-is.
              p->pos = save;
            }
          }
          jbCodepoint(&buf, (uint32_t)cp);
          break;
        }
        default:
          jbFree(&buf);
          raiseError(vm.valueErrorClass,
                     "json: invalid escape '\\%c' at %d.", e, p->pos - 1);
          return false;
      }
    } else if ((unsigned char)c < 0x20) {
      jbFree(&buf);
      raiseError(vm.valueErrorClass,
                 "json: control character in string at %d.", p->pos - 1);
      return false;
    } else {
      jbPutc(&buf, c);
    }
  }

  *out = OBJ_VAL(copyString(buf.data ? buf.data : "", (int)buf.count));
  jbFree(&buf);
  return true;
}

static bool jsonParseNumber(JsonParser* p, Value* out) {
  int start = p->pos;
  bool isDouble = false;
  if (jpPeek(p) == '-') p->pos++;
  while (p->pos < p->len) {
    char c = p->s[p->pos];
    if (c >= '0' && c <= '9') {
      p->pos++;
    } else if (c == '.' || c == 'e' || c == 'E') {
      isDouble = true;
      p->pos++;
    } else if (c == '+' || c == '-') {
      p->pos++;  // exponent sign
    } else {
      break;
    }
  }

  int n = p->pos - start;
  if (n == 0 || (n == 1 && p->s[start] == '-')) {
    raiseError(vm.valueErrorClass, "json: invalid number at %d.", start);
    return false;
  }

  char stackbuf[64];
  char* buf = stackbuf;
  if (n >= (int)sizeof(stackbuf)) buf = (char*)malloc(n + 1);
  memcpy(buf, p->s + start, n);
  buf[n] = '\0';

  char* end;
  bool ok = true;
  if (!isDouble) {
    errno = 0;
    long long ll = strtoll(buf, &end, 10);
    if (*end == '\0' && errno != ERANGE) {
      *out = INT_VAL((int64_t)ll);
      goto done;
    }
    // Oversized integer: fall through to double.
  }
  {
    errno = 0;
    double d = strtod(buf, &end);
    if (*end != '\0') {
      ok = false;
    } else {
      *out = DOUBLE_VAL(d);
    }
  }

done:
  if (buf != stackbuf) free(buf);
  if (!ok) {
    raiseError(vm.valueErrorClass, "json: invalid number at %d.", start);
    return false;
  }
  return true;
}

static bool jsonParseArray(JsonParser* p, Value* out, int depth) {
  p->pos++;  // consume [
  ObjArray* arr = newArray();
  push(OBJ_VAL(arr));  // root while building

  jpSkipWs(p);
  if (jpPeek(p) == ']') {
    p->pos++;
    *out = pop();
    return true;
  }

  for (;;) {
    Value elem;
    if (!jsonParseValue(p, &elem, depth + 1)) return false;
    push(elem);  // protect across writeValueArray's possible GC
    writeValueArray(&arr->elements, elem);
    pop();

    jpSkipWs(p);
    char c = jpPeek(p);
    if (c == ',') { p->pos++; continue; }
    if (c == ']') { p->pos++; break; }
    raiseError(vm.valueErrorClass,
               "json: expected ',' or ']' at %d.", p->pos);
    return false;
  }

  *out = pop();
  return true;
}

static bool jsonParseObject(JsonParser* p, Value* out, int depth) {
  p->pos++;  // consume {
  ObjMap* map = newMap();
  push(OBJ_VAL(map));  // root while building

  jpSkipWs(p);
  if (jpPeek(p) == '}') {
    p->pos++;
    *out = pop();
    return true;
  }

  for (;;) {
    jpSkipWs(p);
    if (jpPeek(p) != '"') {
      raiseError(vm.valueErrorClass,
                 "json: expected string key at %d.", p->pos);
      return false;
    }
    Value key;
    if (!jsonParseString(p, &key)) return false;
    push(key);  // [map, key]

    jpSkipWs(p);
    if (jpPeek(p) != ':') {
      raiseError(vm.valueErrorClass, "json: expected ':' at %d.", p->pos);
      return false;
    }
    p->pos++;  // consume :

    Value val;
    if (!jsonParseValue(p, &val, depth + 1)) return false;
    push(val);  // [map, key, val]
    valueTableSet(&map->entries, key, val);
    pop();      // val
    pop();      // key

    jpSkipWs(p);
    char c = jpPeek(p);
    if (c == ',') { p->pos++; continue; }
    if (c == '}') { p->pos++; break; }
    raiseError(vm.valueErrorClass,
               "json: expected ',' or '}' at %d.", p->pos);
    return false;
  }

  *out = pop();
  return true;
}

static bool jsonParseValue(JsonParser* p, Value* out, int depth) {
  if (depth > JSON_MAX_DEPTH) {
    raiseError(vm.valueErrorClass, "json: nesting too deep to parse.");
    return false;
  }

  jpSkipWs(p);
  if (p->pos >= p->len) {
    raiseError(vm.valueErrorClass, "json: unexpected end of input.");
    return false;
  }

  char c = jpPeek(p);
  switch (c) {
    case '{': return jsonParseObject(p, out, depth);
    case '[': return jsonParseArray(p, out, depth);
    case '"': return jsonParseString(p, out);
    case 't':
      if (jpMatchLiteral(p, "true")) { *out = BOOL_VAL(true); return true; }
      break;
    case 'f':
      if (jpMatchLiteral(p, "false")) { *out = BOOL_VAL(false); return true; }
      break;
    case 'n':
      if (jpMatchLiteral(p, "null")) { *out = NIL_VAL; return true; }
      break;
    default:
      if (c == '-' || (c >= '0' && c <= '9')) {
        return jsonParseNumber(p, out);
      }
      break;
  }

  raiseError(vm.valueErrorClass,
             "json: unexpected character at %d.", p->pos);
  return false;
}

Value jsonParseNative(int argCount, Value* args) {
  if (argCount != 1 || !IS_STRING(args[0])) {
    runtimeError("json.parse() expects a string argument.");
    return NIL_VAL;
  }

  ObjString* input = AS_STRING(args[0]);
  JsonParser p = { input->chars, 0, input->length };

  // Save the stack level so a mid-parse error can discard any partially-built
  // containers that were pushed as GC roots.
  Value* savedTop = vm.stackTop;

  Value result;
  if (!jsonParseValue(&p, &result, 0)) {
    vm.stackTop = savedTop;
    return NIL_VAL;  // raiseError already set the pending exception
  }

  jpSkipWs(&p);
  if (p.pos != p.len) {
    vm.stackTop = savedTop;
    raiseError(vm.valueErrorClass,
               "json: trailing characters at %d.", p.pos);
    return NIL_VAL;
  }

  return result;
}
