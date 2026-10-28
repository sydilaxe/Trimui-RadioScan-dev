/* Station list (stations.txt: "Name|URL" per line; a URL can also be a local file:// .wav) and the online directory scanner
 * (radio-browser.info, a free community database of internet radio stations). */
#ifndef STATIONS_H
#define STATIONS_H

#define MAX_STATIONS 512

typedef struct {
    char name[96];
    char url[512];
    char info[64]; /* genre / bitrate / country, shown in scan results */
    int cat;       /* CAT_ANY / CAT_VOICE / CAT_MUSIC / CAT_SOUND (see dsp.h) */
} Station;

typedef struct {
    Station items[MAX_STATIONS];
    int count;
} StationList;

int stations_load(StationList *l, const char *path);   /* returns count, -1 if file missing */
int stations_save(const StationList *l, const char *path);
int stations_add(StationList *l, const char *name, const char *url, int cat); /* index, or -1 */
int category_from_text(const char *s);     /* "voice"/"music"/"sound" (or genre tags) -> CAT_* */
void stations_remove(StationList *l, int idx);
int stations_find_url(const StationList *l, const char *url);
/* a name from a URL: the host, or a file's name without its extension */
void station_default_name(const char *url, char *out, int n);
/* add every .wav in `dir` not already listed, as file://<url_prefix>/<name>.wav; returns how many were added */
int stations_add_wav_folder(StationList *l, const char *dir, const char *url_prefix);

/* Scanner menu: genre tags grouped by category (VOICES / SOUND / MUSIC). */
typedef struct { const char *tag; int cat; } ScanGenre;
extern const ScanGenre scan_genres[];
extern const int scan_genre_count;

/* Blocking directory query. kind: 0 = by tag, 1 = by name, 2 = random. Fills `out` with MP3 stations. */
int directory_search(int kind, const char *query, StationList *out, char *err, int errlen);

#endif
