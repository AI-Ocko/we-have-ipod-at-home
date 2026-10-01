#include "translit.h"

#include <stdint.h>
#include <string.h>

// Base letters for U+00C0..U+00FF, one per code point. Entries whose real
// replacement is two letters (AE, Th, ss, ae, th) are handled before this
// table is consulted; the letter here is only a filler.
static const char LATIN1[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
    "aaaaaaaceeeeiiiidnooooo/ouuuuyty";

// Base letters for U+0100..U+017F. IJ/ij and OE/oe are handled separately.
static const char LATIN_EXT_A[] =
    "AaAaAa"       // 0100
    "CcCcCcCc"     // 0106
    "DdDd"         // 010E
    "EeEeEeEeEe"   // 0112
    "GgGgGgGg"     // 011C
    "HhHh"         // 0124
    "IiIiIiIiIi"   // 0128
    "Jj"           // 0132 (IJ ligatures, filler)
    "Jj"           // 0134
    "Kkk"          // 0136
    "LlLlLlLlLl"   // 0139
    "NnNnNn"       // 0143
    "n"            // 0149
    "Nn"           // 014A
    "OoOoOo"       // 014C
    "Oo"           // 0152 (OE ligatures, filler)
    "RrRrRr"       // 0154
    "SsSsSsSs"     // 015A
    "TtTtTt"       // 0162
    "UuUuUuUuUuUu" // 0168
    "Ww"           // 0174
    "Yy"           // 0176
    "Y"            // 0178
    "ZzZzZz"       // 0179
    "s";           // 017F

_Static_assert(sizeof(LATIN1) == 64 + 1, "LATIN1 must cover U+00C0..U+00FF");
_Static_assert(sizeof(LATIN_EXT_A) == 128 + 1,
               "LATIN_EXT_A must cover U+0100..U+017F");

#define INVALID_CODE_POINT 0xFFFFFFFFu

// Decode one code point and advance *pp past it. A malformed sequence consumes
// a single byte and yields INVALID_CODE_POINT. Never reads past the string's
// terminator: a NUL is not a continuation byte, so it ends the sequence.
static uint32_t next_code_point(const unsigned char **pp) {
  const unsigned char *p = *pp;
  uint32_t cp = p[0];
  int extra;

  if (cp < 0x80) {
    *pp = p + 1;
    return cp;
  }
  if ((cp & 0xE0) == 0xC0) {
    extra = 1;
    cp &= 0x1F;
  } else if ((cp & 0xF0) == 0xE0) {
    extra = 2;
    cp &= 0x0F;
  } else if ((cp & 0xF8) == 0xF0) {
    extra = 3;
    cp &= 0x07;
  } else {
    *pp = p + 1;
    return INVALID_CODE_POINT;
  }
  for (int i = 1; i <= extra; i++) {
    if ((p[i] & 0xC0) != 0x80) {
      *pp = p + 1;
      return INVALID_CODE_POINT;
    }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  *pp = p + extra + 1;
  return cp;
}

// Return the LCD replacement for a code point, or NULL if there is none.
// `one` is caller-provided scratch for single-character results.
static const char *replacement_for(uint32_t cp, char one[2]) {
  one[1] = '\0';

  if (cp < 0x80) {
    // The HD44780's character ROM shows a yen sign for '\' and an arrow for
    // '~', so swap them for look-alikes.
    if (cp == '\\') {
      return "/";
    }
    if (cp == '~') {
      return "-";
    }
    if (cp < 0x20 || cp == 0x7F) {
      return " ";
    }
    one[0] = (char)cp;
    return one;
  }

  switch (cp) {
  case 0x00A0: // no-break space
    return " ";
  case 0x00A1: // inverted exclamation mark
    return "!";
  case 0x00BF: // inverted question mark
    return "?";
  case 0x00AB: // guillemets
  case 0x00BB:
    return "\"";
  case 0x00C6:
    return "AE";
  case 0x00DE:
    return "Th";
  case 0x00DF:
    return "ss";
  case 0x00E6:
    return "ae";
  case 0x00FE:
    return "th";
  case 0x0132:
    return "IJ";
  case 0x0133:
    return "ij";
  case 0x0152:
    return "OE";
  case 0x0153:
    return "oe";
  case 0x2018: // curly single quotes, low quote, prime
  case 0x2019:
  case 0x201A:
  case 0x2032:
    return "'";
  case 0x201C: // curly double quotes, low quote, double prime
  case 0x201D:
  case 0x201E:
  case 0x2033:
    return "\"";
  case 0x2010: // hyphens and dashes
  case 0x2011:
  case 0x2012:
  case 0x2013:
  case 0x2014:
  case 0x2015:
    return "-";
  case 0x2022: // bullet
    return "*";
  case 0x2026: // ellipsis
    return "...";
  default:
    break;
  }

  if (cp >= 0x00C0 && cp <= 0x00FF) {
    one[0] = LATIN1[cp - 0x00C0];
    return one;
  }
  if (cp >= 0x0100 && cp <= 0x017F) {
    one[0] = LATIN_EXT_A[cp - 0x0100];
    return one;
  }
  return NULL;
}

translit_stats_t translit(const char *utf8, char *out, size_t out_size) {
  translit_stats_t stats = {0, 0};
  size_t used = 0;

  if (out_size > 0) {
    out[0] = '\0';
  }
  if (!utf8) {
    return stats;
  }

  const unsigned char *p = (const unsigned char *)utf8;
  while (*p) {
    char one[2];
    const char *rep = replacement_for(next_code_point(&p), one);
    stats.chars++;
    if (!rep) {
      rep = "?";
      stats.unknown++;
    }
    // Keep reading after the output fills, so the counts describe the whole
    // input; translit_title's fallback rule depends on that.
    for (; *rep; rep++) {
      if (out_size > 0 && used + 1 < out_size) {
        out[used++] = *rep;
      }
    }
  }
  if (out_size > 0) {
    out[used] = '\0';
  }
  return stats;
}

void translit_title(const char *title, const char *file_name, char *out,
                    size_t out_size) {
  if (title && title[0]) {
    translit_stats_t stats = translit(title, out, out_size);
    if (stats.unknown * 2 <= stats.chars) {
      return;
    }
  }

  // Fall back to the file name without its extension. A leading dot is part
  // of the name, not an extension.
  char stem[256];
  const char *name = file_name ? file_name : "";
  size_t len = strlen(name);
  const char *dot = strrchr(name, '.');
  if (dot && dot != name) {
    len = (size_t)(dot - name);
  }
  if (len >= sizeof(stem)) {
    len = sizeof(stem) - 1;
  }
  memcpy(stem, name, len);
  stem[len] = '\0';
  translit(stem, out, out_size);
}
