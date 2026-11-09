/* Self-update: ask GitHub for the latest release, and if it's newer than this build, download its package and swap
 * the new files into the app folder. stations.txt and settings.cfg stay as they are.
 *
 * The package is RadioScan-mustardos.muxapp, the MustardOS application archive. One build serves the Brick, Brick Hammer and Brick Pro. */
#ifndef UPDATE_H
#define UPDATE_H

#include <stddef.h>

/* the version for untagged builds; a release build gets its version from the release tag
 * (make VERSION=x.y.z, which passes -DRS_VERSION). The app offers any newer release. */
#ifndef RS_VERSION
#define RS_VERSION "0.5.0"
#endif
#define RS_REPO "sydilaxe/Trimui-RadioScan-dev"
/* the release file, and the folder its zip holds the app in */
#define RS_ASSET "RadioScan-mustardos.muxapp"
#define RS_ZIP_DIR "RadioScan/" /* MustardOS unpacks a .muxapp into its application folder */

/* exit status that tells launch.sh to start the app again (the new version) */
#define RELAUNCH_STATUS 75

enum {
    UP_IDLE,       /* not checked yet */
    UP_CHECKING,
    UP_CURRENT,    /* this is the latest release */
    UP_AVAILABLE,  /* a newer release is out: latest, url */
    UP_CHECK_FAILED,
    UP_INSTALLING, /* progress 0..100 */
    UP_INSTALLED,  /* the new files are in place: relaunch */
    UP_FAILED      /* install failed: err (the old version is untouched) */
};

typedef struct {
    volatile int state;
    volatile int progress;
    char latest[32]; /* "0.8.2" */
    char url[1024];  /* the release's update_asset() */
    char err[160];
} UpdateStatus;

const UpdateStatus *update_status(void);
const char *update_asset(void); /* the release file this copy updates from */
void update_check_start(void);                 /* in the background; state goes CHECKING -> CURRENT / AVAILABLE / CHECK_FAILED */
void update_install_start(const char *app_dir); /* from AVAILABLE; state goes INSTALLING -> INSTALLED / FAILED */

/* blocking versions, for the command line and tests; 0 on success */
int update_check(void);
int update_install(const char *app_dir);

/* true if dotted version a ("v0.8.2" or "0.8.2") is newer than b */
int version_newer(const char *a, const char *b);
/* unpack the app folder of a zip (Apps/RadioScan/, or RadioScan/ for MustardOS) into dir, keeping the user's files; 0 on success */
int update_unzip(const unsigned char *zip, long len, const char *dir, char *err, size_t errlen);

#endif
