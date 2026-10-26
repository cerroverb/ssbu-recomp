/* UPnP-IGD port mapping for online play on Linux / macOS, through miniupnpc (BSD).
 * POSIX port of net_upnp.c (Windows COM API: IUPnPNAT). Same three calls, same results:
 *
 *   upnp_map_udp(port, lan_ip, desc, ext_ip, n, why, n)   external UDP port -> lan_ip:port; 1 ok, 0 failed (why)
 *   upnp_unmap_udp(port)                                   removes that mapping (call at exit)
 *   upnp_probe(why, n)                                     READ ONLY: 1 if an IGD answers (why: what it is)
 *
 * #included by net_relay.c, so everything is static. Compile the standalone test with
 *   clang -DUPNP_POSIX_MAIN net_upnp_posix.c -o upnp_probe $(pkg-config --cflags --libs miniupnpc)
 *
 * Every operation runs on a short-lived worker thread and the caller waits at most UPNP_WAIT_MS, so a silent router can
 * never freeze the game. A worker that outlives the wait keeps running (SSDP has no cancellation): the job is reference
 * counted, so it frees itself when that worker ends. */
#ifndef NET_UPNP_POSIX_C
#define NET_UPNP_POSIX_C

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

#define UPNP_WAIT_MS 6000
#define UPNP_UNMAP_WAIT_MS 4000

enum { UPNP_OP_MAP = 1, UPNP_OP_UNMAP, UPNP_OP_PROBE };

typedef struct upnp_job {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int refs;                      /* waiter + worker; the last one frees */
    int done;                      /* the worker has written its result */
    int op;
    uint16_t port;
    char lan[16];
    char desc[64];
    int ok;
    char ext[64];
    char why[96];
} upnp_job;

static void upnp_job_init(upnp_job *j) {
    pthread_mutex_init(&j->mutex, NULL);
    pthread_cond_init(&j->cond, NULL);
    j->refs = 0;
    j->done = 0;
    j->ok = 0;
    j->ext[0] = 0;
    j->why[0] = 0;
}

static void upnp_job_release(upnp_job *j) {
    pthread_mutex_lock(&j->mutex);
    int last = --j->refs == 0;
    pthread_mutex_unlock(&j->mutex);
    if (last) {
        pthread_mutex_destroy(&j->mutex);
        pthread_cond_destroy(&j->cond);
        free(j);
    }
}

/* the worker's result is visible to the waiter: it writes everything before taking the mutex to set done */
static void upnp_job_signal(upnp_job *j) {
    pthread_mutex_lock(&j->mutex);
    j->done = 1;
    pthread_cond_signal(&j->cond);
    pthread_mutex_unlock(&j->mutex);
}

static int upnp_job_wait(upnp_job *j, int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&j->mutex);
    while (!j->done) {
        if (pthread_cond_timedwait(&j->cond, &j->mutex, &ts) == ETIMEDOUT) break;
    }
    int done = j->done;
    pthread_mutex_unlock(&j->mutex);
    return done;
}

static void upnp_fail(upnp_job *j, const char *why) {
    j->ok = 0;
    snprintf(j->why, sizeof j->why, "%s", why);
}

static const char *upnp_service(const struct IGDdatas *data) {
    /* first = WANIPConnection / WANPPPConnection of the IGD; CIF is only a fallback for odd routers */
    if (data->first.servicetype[0]) return data->first.servicetype;
    if (data->CIF.servicetype[0]) return data->CIF.servicetype;
    return "";
}

static void *upnp_worker(void *arg) {
    upnp_job *j = (upnp_job *)arg;
    struct UPNPDev *devlist = NULL;
    struct UPNPUrls urls;
    struct IGDdatas data;
    char lanaddr[64] = "", wanaddr[64] = "";
    char portstr[8];
    int ret;

    snprintf(portstr, sizeof portstr, "%u", (unsigned)j->port);

    devlist = upnpDiscover(2000, NULL, NULL, 0, 0, 2, &ret);
    if (!devlist) { upnp_fail(j, "no UPnP device found"); upnp_job_signal(j); upnp_job_release(j); return NULL; }

    memset(&urls, 0, sizeof urls);
    memset(&data, 0, sizeof data);
    ret = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, (int)sizeof lanaddr, wanaddr, (int)sizeof wanaddr);
    freeUPNPDevlist(devlist);
    if (ret == UPNP_NO_IGD || !urls.controlURL || !upnp_service(&data)[0]) {
        upnp_fail(j, "no UPnP router answered");
        if (urls.controlURL) FreeUPNPUrls(&urls);
        upnp_job_signal(j); upnp_job_release(j);
        return NULL;
    }

    if (j->op == UPNP_OP_PROBE) {
        char status[64] = "", lasterror[64] = "";
        unsigned int uptime = 0;
        UPNP_GetStatusInfo(urls.controlURL, upnp_service(&data), status, &uptime, lasterror);
        j->ok = 1;                              /* any valid IGD that answers is enough */
        snprintf(j->why, sizeof j->why, "UPnP router answered%s%s%s", status[0] ? " (" : "", status, status[0] ? ")" : "");
    } else if (j->op == UPNP_OP_UNMAP) {
        ret = UPNP_DeletePortMapping(urls.controlURL, upnp_service(&data), portstr, "UDP", "");
        if (ret == UPNPCOMMAND_SUCCESS) { j->ok = 1; snprintf(j->why, sizeof j->why, "mapping removed"); }
        else upnp_fail(j, strupnperror(ret));
    } else {
        char ext[64] = "";
        /* a stale mapping of ours from a crashed run would make Add fail: drop it first (ignore the result) */
        UPNP_DeletePortMapping(urls.controlURL, upnp_service(&data), portstr, "UDP", "");
        ret = UPNP_AddPortMapping(urls.controlURL, upnp_service(&data), portstr, portstr, j->lan,
                                  j->desc, "UDP", NULL, "0");
        if (ret == UPNPCOMMAND_SUCCESS) {
            j->ok = 1;
            snprintf(j->why, sizeof j->why, "mapped");
            if (UPNP_GetExternalIPAddress(urls.controlURL, upnp_service(&data), ext) == UPNPCOMMAND_SUCCESS)
                snprintf(j->ext, sizeof j->ext, "%s", ext);
        } else {
            upnp_fail(j, strupnperror(ret));
        }
    }

    FreeUPNPUrls(&urls);
    upnp_job_signal(j);
    upnp_job_release(j);
    return NULL;
}

/* runs one operation; returns the finished job (the waiter owns one reference), NULL on timeout */
static upnp_job *upnp_run(upnp_job *j, int timeout_ms) {
    pthread_t th;
    j->refs = 2;                        /* the waiter and the worker */
    if (pthread_create(&th, NULL, upnp_worker, j) != 0) {
        j->refs = 1;                    /* only the waiter's reference is left */
        upnp_fail(j, "could not start the UPnP thread");
        j->done = 1;
        return j;
    }
    pthread_detach(th);
    if (!upnp_job_wait(j, timeout_ms)) {
        upnp_job_release(j);            /* abandoned: the worker frees it when (if) it ends */
        return NULL;
    }
    return j;
}

static void upnp_copy(char *dst, size_t n, const char *src) {
    if (dst && n) snprintf(dst, n, "%s", src);
}

static int upnp_finish(upnp_job *j, char *ext_ip, size_t ext_n, char *why, size_t why_n) {
    if (!j) { upnp_copy(why, why_n, "no UPnP router answered (timeout)"); return 0; }
    int ok = j->ok;
    upnp_copy(why, why_n, j->why);
    if (ok && ext_ip) upnp_copy(ext_ip, ext_n, j->ext);
    upnp_job_release(j);
    return ok;
}

static int upnp_map_udp(uint16_t port, uint32_t lan_ip_host_order, const char *desc,
                        char *ext_ip, size_t ext_n, char *why, size_t why_n) {
    upnp_job *j = (upnp_job *)calloc(1, sizeof *j);
    if (ext_ip && ext_n) ext_ip[0] = 0;
    if (!j) { upnp_copy(why, why_n, "out of memory"); return 0; }
    upnp_job_init(j);
    j->op = UPNP_OP_MAP;
    j->port = port;
    snprintf(j->lan, sizeof j->lan, "%u.%u.%u.%u",
             (unsigned)(lan_ip_host_order >> 24) & 255,
             (unsigned)(lan_ip_host_order >> 16) & 255,
             (unsigned)(lan_ip_host_order >> 8) & 255,
             (unsigned)lan_ip_host_order & 255);
    snprintf(j->desc, sizeof j->desc, "%s", desc ? desc : "SSBU online");
    return upnp_finish(upnp_run(j, UPNP_WAIT_MS), ext_ip, ext_n, why, why_n);
}

static void upnp_unmap_udp(uint16_t port) {
    upnp_job *j = (upnp_job *)calloc(1, sizeof *j);
    if (!j) return;
    upnp_job_init(j);
    j->op = UPNP_OP_UNMAP;
    j->port = port;
    upnp_finish(upnp_run(j, UPNP_UNMAP_WAIT_MS), NULL, 0, NULL, 0);
}

static int upnp_probe(char *why, size_t why_n) {
    upnp_job *j = (upnp_job *)calloc(1, sizeof *j);
    if (!j) { upnp_copy(why, why_n, "out of memory"); return 0; }
    upnp_job_init(j);
    j->op = UPNP_OP_PROBE;
    return upnp_finish(upnp_run(j, UPNP_WAIT_MS), NULL, 0, why, why_n);
}

#ifdef UPNP_POSIX_MAIN
/* test: the probe is read only. The mapping calls are compiled but only run with the flag --really-map, which the
 * test run does NOT use (it changes the router configuration and needs the owner's permission). */
int main(int argc, char **argv) {
    char why[128] = "", ext[64] = "";
    int ok;
    if (argc > 3 && !strcmp(argv[1], "--really-map")) {
        ok = upnp_map_udp((uint16_t)atoi(argv[2]), 0, argv[3], ext, sizeof ext, why, sizeof why);
        printf("map: %d ext=%s why=%s\n", ok, ext, why);
        upnp_unmap_udp((uint16_t)atoi(argv[2]));
        return !ok;
    }
    ok = upnp_probe(why, sizeof why);
    printf("probe: %d (%s)\n", ok, why);
    return 0;
}
#endif
#endif /* NET_UPNP_POSIX_C */
