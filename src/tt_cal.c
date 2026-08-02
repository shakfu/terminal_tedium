/*
 * CV calibration: per-channel offset and scale, persisted as JSON.
 *
 * Why this exists at all: the inputs are 12 bits over a 10 V span, which is
 * 2.44 mV per code. A 1V/oct semitone is 83.3 mV, so one code is about 1.76
 * cents. The divider and op-amp tolerances alone will put an uncalibrated
 * channel tens of cents out and leave channels mismatched with each other, so
 * pitch tracking is not usable without measured per-channel constants.
 *
 * The JSON is hand-parsed. Pulling in a JSON library for four numbers per
 * channel would be the only external dependency in the whole core.
 */

#include "tedium/tedium.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Nominal transfer: the front end inverts and centres a +/-5 V input on the
 * 12-bit range, so code 0 is +5 V and code 4095 is just under -5 V. */
#define TT_NOMINAL_OFFSET 2048.0f
#define TT_NOMINAL_SCALE  (-10.0f / 4096.0f)

void tt_cal_nominal(tt_cal *cal, tt_board board)
{
    int i;
    (void)board;
    for (i = 0; i < TT_MAX_CV; i++) {
        cal->ch[i].offset = TT_NOMINAL_OFFSET;
        cal->ch[i].scale  = TT_NOMINAL_SCALE;
    }
}

size_t tt_cal_default_path(char *buf, size_t len)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    int n;

    if (xdg && *xdg)
        n = snprintf(buf, len, "%s/terminal_tedium/cal.json", xdg);
    else if (home && *home)
        n = snprintf(buf, len, "%s/.config/terminal_tedium/cal.json", home);
    else
        n = snprintf(buf, len, "./cal.json");

    return (n < 0) ? 0 : (size_t)n;
}

/* ------------------------------------------------------------------ */
/* a very small JSON reader                                            */
/* ------------------------------------------------------------------ */

/* Advance past whitespace. */
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Find the next occurrence of "key" used as an object key, starting at p and
 * stopping at end. Returns a pointer just past the colon, or NULL. */
static const char *find_key(const char *p, const char *end, const char *key)
{
    size_t klen = strlen(key);

    while (p < end) {
        const char *q = memchr(p, '"', (size_t)(end - p));
        if (!q) return NULL;
        q++;
        if ((size_t)(end - q) >= klen && memcmp(q, key, klen) == 0 &&
            q[klen] == '"') {
            const char *r = skip_ws(q + klen + 1);
            if (*r == ':') return skip_ws(r + 1);
        }
        p = q;
    }
    return NULL;
}

/* Read a JSON number. Returns 0 on success. */
static int read_num(const char *p, const char *end, float *out)
{
    char tmp[64];
    size_t n = 0;
    char *stop = NULL;
    double v;

    while (p < end && n + 1 < sizeof(tmp) &&
           (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' ||
            (*p >= '0' && *p <= '9')))
        tmp[n++] = *p++;
    tmp[n] = '\0';
    if (n == 0) return -1;

    v = strtod(tmp, &stop);
    if (stop == tmp) return -1;
    *out = (float)v;
    return 0;
}

int tt_cal_load_file(tt_cal *cal, const char *path)
{
    char pathbuf[512];
    FILE *f;
    long size;
    char *text = NULL;
    const char *p, *end, *arr;
    int ch = 0;

    if (!path) {
        tt_cal_default_path(pathbuf, sizeof(pathbuf));
        path = pathbuf;
    }

    f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) goto fail;
    size = ftell(f);
    if (size < 0 || size > (1 << 20)) goto fail;
    if (fseek(f, 0, SEEK_SET) != 0) goto fail;

    text = (char *)malloc((size_t)size + 1);
    if (!text) goto fail;
    if (fread(text, 1, (size_t)size, f) != (size_t)size) goto fail;
    text[size] = '\0';
    fclose(f);
    f = NULL;

    end = text + size;

    arr = find_key(text, end, "channels");
    if (!arr || *arr != '[') { free(text); errno = EINVAL; return -1; }

    /* Each element is an object carrying "offset" and "scale". Anything the
     * file does not specify keeps its nominal value, so a partially written
     * calibration degrades sensibly instead of zeroing a channel. */
    p = arr + 1;
    while (p < end && ch < TT_MAX_CV) {
        const char *obj_end, *kp;
        float v;

        p = skip_ws(p);
        if (*p == ']' || *p == '\0') break;
        if (*p != '{') { p++; continue; }

        obj_end = memchr(p, '}', (size_t)(end - p));
        if (!obj_end) break;

        kp = find_key(p, obj_end, "offset");
        if (kp && read_num(kp, obj_end, &v) == 0) cal->ch[ch].offset = v;

        kp = find_key(p, obj_end, "scale");
        if (kp && read_num(kp, obj_end, &v) == 0) cal->ch[ch].scale = v;

        ch++;
        p = obj_end + 1;
    }

    free(text);
    return (ch > 0) ? 0 : -1;

fail:
    if (f) fclose(f);
    free(text);
    return -1;
}

/* Create every directory component of path except the last, mkdir -p style.
 * Failures are ignored; fopen reports the real problem a moment later. */
static int mkdir_parents(const char *path)
{
    char buf[512];
    size_t n = strlen(path), i;
    char *q;

    if (n >= sizeof(buf)) return -1;
    memcpy(buf, path, n + 1);

    for (i = n; i > 0; i--)
        if (buf[i - 1] == '/') { buf[i - 1] = '\0'; break; }
    if (i == 0) return 0;   /* no directory component */

    q = buf;
    if (*q == '/') q++;
    for (; *q; q++) {
        if (*q == '/') {
            *q = '\0';
            (void)mkdir(buf, 0755);
            *q = '/';
        }
    }
    (void)mkdir(buf, 0755);
    return 0;
}

int tt_cal_save_file(const tt_cal *cal, const char *path)
{
    char pathbuf[512];
    char tmppath[560];
    FILE *f;
    int i;

    if (!path) {
        tt_cal_default_path(pathbuf, sizeof(pathbuf));
        path = pathbuf;
    }
    mkdir_parents(path);

    /* Write to a sibling temp file and rename, so an interrupted save cannot
     * leave a half-written calibration behind. */
    snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);

    f = fopen(tmppath, "wb");
    if (!f) return -1;

    fprintf(f, "{\n");
    fprintf(f, "  \"version\": 1,\n");
    fprintf(f, "  \"comment\": \"volts = (raw_code - offset) * scale\",\n");
    fprintf(f, "  \"channels\": [\n");
    for (i = 0; i < TT_MAX_CV; i++) {
        fprintf(f, "    { \"offset\": %.6f, \"scale\": %.9g }%s\n",
                (double)cal->ch[i].offset, (double)cal->ch[i].scale,
                (i + 1 < TT_MAX_CV) ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");

    if (fflush(f) != 0 || ferror(f)) { fclose(f); remove(tmppath); return -1; }
    fclose(f);

    if (rename(tmppath, path) != 0) { remove(tmppath); return -1; }
    return 0;
}
