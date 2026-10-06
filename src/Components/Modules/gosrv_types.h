/* C types of the gosrv C API (go-dht-serverlist, pkg/capi).
 *
 * This is a vendored copy of pkg/capi/sltypes.h from
 * https://github.com/icedream/go-dht-serverlist. Keep it in sync with
 * that file; the layout of SlConfig, SlServer and SlServerConfig is
 * part of the ABI between this DLL and the 32-bit gosrv.dll that the
 * IW4x launcher ships next to the game.
 *
 * The function prototypes are declared in Gosrv.hpp (const-correct;
 * cgo does not encode const in the exported symbols, so this is
 * ABI-compatible with the generated header).
 */

#ifndef IW4X_GOSRV_TYPES_H
#define IW4X_GOSRV_TYPES_H

typedef enum {
    SL_OK = 0,
    SL_BAD_ARG = 1,
    SL_NOT_BOOTSTRAPPED = 2,
    SL_TIMEOUT = 3,
    SL_TRUNCATED = 4,
    SL_INTERNAL = 5,
    SL_IN_USE = 6
} SlResult;

typedef enum {
    SL_META_STRING = 0,
    SL_META_UINT = 1,
    SL_META_BOOL = 2,
    SL_META_BYTES = 3,
    SL_META_ARRAY = 4,
    SL_META_MAP = 5
} SlMetaType;

typedef enum {
    SL_LOG_INFO = 0,
    SL_LOG_WARN = 1,
    SL_LOG_ERROR = 2
} SlLogLevel;

typedef struct {
    const char *prefix; /* "gosrv/<project>/v1/" */
    int deployment; /* 0 = own DHT; 1 = unsupported */
    int join_mode; /* 0 = direct (bootstrap_addrs), 1 = resolve via anchor DHT */
    const char **bootstrap_addrs; /* join_mode 0: NULL-terminated own-DHT multiaddrs */
    int fallback_count; /* join_mode 1: fallback list length */
    const char **fallback_addrs; /* join_mode 1: hardcoded last resort (may be NULL) */
    const unsigned char *operator_pubkey; /* join_mode 1: 32-byte ed25519 public key */
    const char **public_addrs; /* join_mode 1: NULL-terminated anchor-DHT multiaddrs */
    const char *public_prefix; /* join_mode 1: anchor-DHT keyspace */
    int startup_timeout_ms; /* join_mode 1: resolution budget (0 = 10s) */
    int browse_timeout_ms; /* default per-browse deadline (0 = 30s) */
    int max_servers; /* default cap (0 = 256) */
} SlConfig;

typedef struct {
    char name[64];
    char host[256];
    unsigned short port;
    char proto[8];
    char region[32];
    char mode[16];
    unsigned short players;
    unsigned short max_players;
    char version[64];
    char desc[208];
    unsigned char pubkey[32];
} SlServer;

typedef struct {
    const char *key_file; /* ed25519 PEM file (created if missing) */
    const char *host; /* public address clients can reach */
    unsigned short port; /* game port (1-65535) */
    const char *version; /* exact game version to advertise */
    const char *name; /* display name (NULL = "unnamed server") */
    const char *proto; /* "udp" or "tcp" (NULL = "udp") */
    const char **meta_keys; /* NULL-terminated (may be NULL) */
    const char **meta_vals; /* parallel to meta_keys (may be NULL) */
    int refresh_ms; /* announce refresh interval (0 = 60s) */
    int startup_timeout_ms; /* first-announce budget (0 = 30s) */
} SlServerConfig;

#endif // IW4X_GOSRV_TYPES_H
