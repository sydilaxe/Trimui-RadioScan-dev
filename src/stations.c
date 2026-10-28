#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "stations.h"
#include "net.h"
#include "dsp.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void trim(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

void station_default_name(const char *url, char *out, int n)
{
    if (!strncasecmp(url, "file://", 7)) { /* the file's name without its folder or extension */
        const char *b = strrchr(url + 7, '/');
        b = b ? b + 1 : url + 7;
        const char *dot = strrchr(b, '.');
        snprintf(out, n, "%.*s", dot && dot > b ? (int)(dot - b) : (int)strlen(b), b);
        return;
    }
    const char *host = strstr(url, "://");
    snprintf(out, n, "%.80s", host ? host + 3 : url);
}

int stations_load(StationList *l, const char *path)
{
    l->count = 0;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof line, f) && l->count < MAX_STATIONS) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *bar = strchr(line, '|');
        char *name = line, *url, *cat = NULL;
        if (bar) { *bar = 0; url = bar + 1; }
        else { url = line; name = NULL; } /* bare URL line */
        char *bar2 = strchr(url, '|');
        if (bar2) { *bar2 = 0; cat = bar2 + 1; }
        trim(url);
        if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8) && strncasecmp(url, "file://", 7)) continue;
        Station *s = &l->items[l->count++];
        memset(s, 0, sizeof *s);
        if (name) { trim(name); snprintf(s->name, sizeof s->name, "%s", name); }
        if (!s->name[0]) station_default_name(url, s->name, sizeof s->name);
        snprintf(s->url, sizeof s->url, "%s", url);
        s->cat = cat ? category_from_text(cat) : CAT_ANY;
    }
    fclose(f);
    return l->count;
}

int stations_save(const StationList *l, const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "# RadioScan stations: one per line as  Name|URL|category\n"
               "# category is voice (talk, news, spoken word), music, or sound (nature, drones, scanners).\n"
               "# MP3 streams over http:// or https://; .pls and .m3u playlist links also work.\n"
               "# A .wav file plays (looped) like a station: file://wav/name.wav is in the app folder, file:///path is absolute.\n"
               "# .wav files put in the app's wav folder are added here by themselves.\n"
               "# Edit this file on the SD card, or add stations in the app (RADIO page: Y = type URL, R2 = scan).\n\n");
    static const char *const cn[] = {"", "voice", "music", "sound"};
    for (int i = 0; i < l->count; i++)
        fprintf(f, "%s|%s%s%s\n", l->items[i].name, l->items[i].url, l->items[i].cat ? "|" : "", cn[l->items[i].cat & 3]);
    fclose(f);
    return rename(tmp, path);
}

int stations_find_url(const StationList *l, const char *url)
{
    for (int i = 0; i < l->count; i++)
        if (!strcmp(l->items[i].url, url)) return i;
    return -1;
}

int category_from_text(const char *t)
{
    static const char *const voice[] = {"voice", "talk", "news", "spoken", "poetry", "story", "book", "lecture",
                                        "interview", "comedy", "drama", "religio", "sermon", "podcast", "politic", NULL};
    static const char *const sound[] = {"sound", "nature", "field", "ambient", "drone", "scanner", "noise", "space",
                                        "meditation", "sleep", "rain", "birds", "experimental", "soundscape", NULL};
    for (int i = 0; voice[i]; i++) if (strcasestr(t, voice[i])) return CAT_VOICE;
    for (int i = 0; sound[i]; i++) if (strcasestr(t, sound[i])) return CAT_SOUND;
    if (strcasestr(t, "music") || t[0]) return CAT_MUSIC;
    return CAT_ANY;
}

int stations_add(StationList *l, const char *name, const char *url, int cat)
{
    int existing = stations_find_url(l, url);
    if (existing >= 0) return existing;
    if (l->count >= MAX_STATIONS) return -1;
    Station *s = &l->items[l->count];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    for (char *p = s->name; *p; p++) if (*p == '|') *p = '/';
    snprintf(s->url, sizeof s->url, "%s", url);
    s->cat = cat;
    return l->count++;
}

void stations_remove(StationList *l, int idx)
{
    if (idx < 0 || idx >= l->count) return;
    memmove(&l->items[idx], &l->items[idx + 1], sizeof(Station) * (l->count - idx - 1));
    l->count--;
}

static int name_cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

int stations_add_wav_folder(StationList *l, const char *dir, const char *url_prefix)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    char *names[MAX_STATIONS];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < MAX_STATIONS) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || len < 5 || strcasecmp(e->d_name + len - 4, ".wav")) continue; /* skips ._ files too */
        names[n] = strdup(e->d_name);
        if (names[n]) n++;
    }
    closedir(d);
    qsort(names, n, sizeof *names, name_cmp);
    int added = 0;
    for (int i = 0; i < n; i++) {
        char url[512], name[96];
        snprintf(url, sizeof url, "file://%s/%s", url_prefix, names[i]);
        free(names[i]);
        if (stations_find_url(l, url) >= 0) continue;
        station_default_name(url, name, sizeof name);
        if (stations_add(l, name, url, CAT_SOUND) >= 0) added++;
    }
    return added;
}

/* ------------------------------------------------------------ directory */

const ScanGenre scan_genres[] = {
    {"talk", CAT_VOICE}, {"news", CAT_VOICE}, {"spoken word", CAT_VOICE}, {"poetry", CAT_VOICE},
    {"storytelling", CAT_VOICE}, {"audiobooks", CAT_VOICE}, {"public radio", CAT_VOICE}, {"community radio", CAT_VOICE},
    {"comedy", CAT_VOICE}, {"science", CAT_VOICE}, {"religion", CAT_VOICE}, {"shortwave", CAT_VOICE},
    {"nature", CAT_SOUND}, {"field recording", CAT_SOUND}, {"ambient", CAT_SOUND}, {"drone", CAT_SOUND},
    {"meditation", CAT_SOUND}, {"sleep", CAT_SOUND}, {"scanner", CAT_SOUND}, {"space", CAT_SOUND},
    {"noise", CAT_SOUND}, {"experimental", CAT_SOUND}, {"soundscape", CAT_SOUND},
    {"classical", CAT_MUSIC}, {"jazz", CAT_MUSIC}, {"electronic", CAT_MUSIC}, {"chillout", CAT_MUSIC},
    {"lofi", CAT_MUSIC}, {"dub", CAT_MUSIC}, {"techno", CAT_MUSIC}, {"folk", CAT_MUSIC}, {"world", CAT_MUSIC},
    {"blues", CAT_MUSIC}, {"soul", CAT_MUSIC}, {"reggae", CAT_MUSIC}, {"rock", CAT_MUSIC}, {"metal", CAT_MUSIC},
    {"hip hop", CAT_MUSIC}, {"latin", CAT_MUSIC}, {"oldies", CAT_MUSIC}, {"synthwave", CAT_MUSIC},
    {"chiptune", CAT_MUSIC}, {"gamelan", CAT_MUSIC}, {"choral", CAT_MUSIC}, {"minimal", CAT_MUSIC},
};
const int scan_genre_count = sizeof scan_genres / sizeof scan_genres[0];

static const char *const rb_servers[] = {
    "https://de1.api.radio-browser.info", "https://fi1.api.radio-browser.info",
    "https://de2.api.radio-browser.info", "https://all.api.radio-browser.info",
    "http://de1.api.radio-browser.info",
};

/* --- tiny JSON walker: just enough for radio-browser's flat station objects --- */
static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* parse a JSON string at p (pointing at '"'); writes ASCII-folded text into out */
static const char *json_str(const char *p, char *out, int n)
{
    int j = 0;
    p++;
    while (*p && *p != '"') {
        unsigned ch = (unsigned char)*p++;
        if (ch == '\\' && *p) {
            char e = *p++;
            switch (e) {
            case 'n': case 'r': case 't': ch = ' '; break;
            case 'u': {
                unsigned v = 0;
                for (int k = 0; k < 4 && isxdigit((unsigned char)*p); k++, p++)
                    v = v * 16 + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                ch = v < 128 ? v : '?';
                break;
            }
            default: ch = (unsigned char)e;
            }
        } else if (ch >= 0x80) {
            while ((unsigned char)*p >= 0x80 && ((unsigned char)*p & 0xC0) == 0x80) p++; /* skip UTF-8 tail */
            ch = '?';
        }
        if (j < n - 1 && ch >= 32) out[j++] = (char)ch;
    }
    if (out) out[j] = 0;
    return *p == '"' ? p + 1 : p;
}

static const char *json_skip(const char *p)
{
    p = skip_ws(p);
    if (*p == '"') { char dummy[2]; return json_str(p, dummy, 1); }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        do {
            if (*p == '"') { char dummy[2]; p = json_str(p, dummy, 1); continue; }
            if (*p == '{' || *p == '[') depth++;
            if (*p == '}' || *p == ']') depth--;
            p++;
        } while (*p && depth > 0);
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

static int parse_stations(const char *json, StationList *out)
{
    const char *p = skip_ws(json);
    if (*p != '[') return -1;
    p++;
    out->count = 0;
    while (*p) {
        p = skip_ws(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') return -1;
        p++;
        char name[96] = "", url[512] = "", url2[512] = "", codec[16] = "", tags[64] = "", cc[8] = "";
        int br = 0;
        while (*p) {
            p = skip_ws(p);
            if (*p == '}') { p++; break; }
            if (*p == ',') { p++; continue; }
            if (*p != '"') return -1;
            char key[32];
            p = json_str(p, key, sizeof key);
            p = skip_ws(p);
            if (*p == ':') p++;
            p = skip_ws(p);
            if (*p == '"' && !strcmp(key, "name")) p = json_str(p, name, sizeof name);
            else if (*p == '"' && !strcmp(key, "url_resolved")) p = json_str(p, url, sizeof url);
            else if (*p == '"' && !strcmp(key, "url")) p = json_str(p, url2, sizeof url2);
            else if (*p == '"' && !strcmp(key, "codec")) p = json_str(p, codec, sizeof codec);
            else if (*p == '"' && !strcmp(key, "tags")) p = json_str(p, tags, sizeof tags);
            else if (*p == '"' && !strcmp(key, "countrycode")) p = json_str(p, cc, sizeof cc);
            else if (!strcmp(key, "bitrate")) { br = atoi(p); p = json_skip(p); }
            else p = json_skip(p);
        }
        const char *u = url[0] ? url : url2;
        trim(name);
        if (!u[0] || !name[0] || out->count >= MAX_STATIONS) continue;
        if (codec[0] && strcasecmp(codec, "MP3")) continue;
        if (strncasecmp(u, "http://", 7) && strncasecmp(u, "https://", 8)) continue;
        if (strstr(u, ".m3u8")) continue;
        Station *s = &out->items[out->count++];
        memset(s, 0, sizeof *s);
        snprintf(s->name, sizeof s->name, "%s", name);
        snprintf(s->url, sizeof s->url, "%s", u);
        s->cat = category_from_text(tags);
        char *comma2 = strchr(tags, ',');
        if (comma2) *comma2 = 0;
        snprintf(s->info, sizeof s->info, "%s%s%s %dk", cc, cc[0] && tags[0] ? " " : "", tags, br);
    }
    return out->count;
}

int directory_search(int kind, const char *query, StationList *out, char *err, int errlen)
{
    char q[256], path[512];
    url_encode(query ? query : "", q, sizeof q);
    const char *common = "codec=MP3&hidebroken=true&limit=100";
    if (kind == 0)
        snprintf(path, sizeof path, "/json/stations/search?tag=%s&order=clickcount&reverse=true&%s", q, common);
    else if (kind == 1)
        snprintf(path, sizeof path, "/json/stations/search?name=%s&order=clickcount&reverse=true&%s", q, common);
    else
        snprintf(path, sizeof path, "/json/stations/search?order=random&%s", common);

    char e[160] = "";
    const char *custom = getenv("RS_DIRECTORY"); /* override the directory server (testing / mirrors) */
    int nserv = (int)(sizeof rb_servers / sizeof rb_servers[0]);
    for (int i = custom ? -1 : 0; i < nserv; i++) {
        char url[800];
        snprintf(url, sizeof url, "%s%s", i < 0 ? custom : rb_servers[i], path);
        char *body = NULL;
        long n = http_fetch(url, &body, 4 << 20, e, sizeof e);
        if (n > 0) {
            int c = parse_stations(body, out);
            free(body);
            if (c >= 0) return c;
            snprintf(e, sizeof e, "Bad directory response");
        }
        free(body);
    }
    snprintf(err, errlen, "Directory unreachable: %s", e);
    return -1;
}
