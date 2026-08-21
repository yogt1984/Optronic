/*
 * optronic_node - sensor node application
 *
 * grabs frames from the ISP, "encodes" them and prints statistics.
 * register access is emulated on the PC with a static array, on the
 * target g_regs is supposed to be the mmap'ed UIO region.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>

#include "regs.h"

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

#define MAX_CFG_LINE    256
#define RING_SIZE       8
#define DEFAULT_W       1920
#define DEFAULT_H       1080
#define DEFAULT_FPS     30
#define DEFAULT_GAIN    256

int   g_width    = DEFAULT_W;
int   g_height   = DEFAULT_H;
int   g_fps      = DEFAULT_FPS;
int   g_gain     = DEFAULT_GAIN;
int   g_verbose  = 0;
int   g_max_frames = 0;         /* 0 = run forever */
char  g_cfg_path[MAX_CFG_LINE] = "optronic.cfg";
char  g_log_prefix[32] = "[INFO]";

int   running = 1;              /* set to 0 by signal handler */
int   reload_gain = 0;          /* set by SIGUSR1 */

/* ------------------------------------------------------------------ */
/* register access                                                     */
/* ------------------------------------------------------------------ */

static uint32_t fake_regs[ISP_SIZE / 4];
volatile uint32_t* g_regs = fake_regs;

static inline uint32_t reg_rd(uint32_t off)
{
    return g_regs[off / 4];
}

static inline void reg_wr(uint32_t off, uint32_t val)
{
    g_regs[off / 4] = val;
}

static void isp_init(void)
{
    memset(fake_regs, 0, sizeof(fake_regs));
    reg_wr(ISP_ID, ISP_ID_MAGIC);
    reg_wr(ISP_VERSION, 0x00010000);
    reg_wr(ISP_FRAME_W, g_width);
    reg_wr(ISP_FRAME_H, g_height);
    reg_wr(ISP_GAIN, g_gain & GAIN_MASK);
    reg_wr(ISP_TEMP_MC, 41250);
    reg_wr(ISP_CTRL, CTRL_ENABLE);
    reg_wr(ISP_STATUS, STAT_RUNNING);
}

/* ------------------------------------------------------------------ */
/* frames and ring                                                     */
/* ------------------------------------------------------------------ */

struct Frame {
    uint8_t* data;
    int w;
    int h;
    int stride;
    uint64_t ts;        /* us since epoch */
    uint32_t seq;
};

Frame*          ring[RING_SIZE];
int             ring_head = 0;
int             ring_tail = 0;
int             ring_count = 0;
pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t        frames_dropped = 0;

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

static Frame* frame_alloc(int w, int h)
{
    Frame* f = new Frame;
    f->w = w;
    f->h = h;
    f->stride = w;
    f->data = new uint8_t[w * h];
    f->ts = 0;
    f->seq = 0;
    return f;
}

static void frame_free(Frame* f)
{
    if (f) {
        delete[] f->data;
        delete f;
    }
}

static int ring_push(Frame* f)
{
    pthread_mutex_lock(&ring_lock);
    if (ring_count == RING_SIZE) {
        /* full - drop the new one */
        frames_dropped++;
        pthread_mutex_unlock(&ring_lock);
        return -1;
    }
    ring[ring_head] = f;
    ring_head = (ring_head + 1) % RING_SIZE;
    ring_count++;
    pthread_mutex_unlock(&ring_lock);
    return 0;
}

static Frame* ring_pop(void)
{
    Frame* f = NULL;
    pthread_mutex_lock(&ring_lock);
    if (ring_count > 0) {
        f = ring[ring_tail];
        ring[ring_tail] = NULL;
        ring_tail = (ring_tail + 1) % RING_SIZE;
        ring_count--;
    }
    pthread_mutex_unlock(&ring_lock);
    return f;
}

/* ------------------------------------------------------------------ */
/* config file: key=value                                              */
/* ------------------------------------------------------------------ */

static int load_config(const char* path)
{
    FILE* fp = fopen(path, "r");
    char key[MAX_CFG_LINE];
    char val[MAX_CFG_LINE];
    int n = 0;

    if (!fp) {
        printf("%s no config file %s, using defaults\n", g_log_prefix, path);
        return 0;
    }

    while (fscanf(fp, "%255[^=]=%255s\n", key, val) == 2) {
        if (strcmp(key, "width") == 0) {
            g_width = atoi(val);
        } else if (strcmp(key, "height") == 0) {
            g_height = atoi(val);
        } else if (strcmp(key, "fps") == 0) {
            g_fps = atoi(val);
        } else if (strcmp(key, "gain") == 0) {
            g_gain = atoi(val);
        } else if (strcmp(key, "verbose") == 0) {
            g_verbose = atoi(val);
        } else {
            printf("[WARN] unknown key %s\n", key);
        }
        n++;
    }
    fclose(fp);
    printf("%s loaded %d keys from %s\n", g_log_prefix, n, path);
    return n;
}

/* ------------------------------------------------------------------ */
/* grab thread                                                         */
/* ------------------------------------------------------------------ */

static void fill_pattern(Frame* f, uint32_t seq)
{
    /* moving gradient so the checksum changes per frame */
    int y, x;
    for (y = 0; y < f->h; y++) {
        uint8_t* row = f->data + y * f->stride;
        for (x = 0; x < f->w; x++) {
            row[x] = (uint8_t)((x + y + seq) & 0xFF);
        }
    }
}

void* grab_thread(void* arg)
{
    uint32_t seq = 0;
    int period_us = 1000000 / g_fps;
    (void)arg;

    printf("%s grab thread started, %dx%d @ %d fps\n", g_log_prefix,
           g_width, g_height, g_fps);

    while (running) {
        Frame* f = frame_alloc(g_width, g_height);
        fill_pattern(f, seq);
        f->ts = now_us();
        f->seq = seq++;

        /* apply gain from register like the ISP would */
        uint32_t gain = reg_rd(ISP_GAIN) & GAIN_MASK;
        if (gain != 256) {
            int i;
            for (i = 0; i < f->w * f->h; i += 97) {
                f->data[i] = (uint8_t)((f->data[i] * gain) >> 8);
            }
        }

        reg_wr(ISP_FRAME_CNT, reg_rd(ISP_FRAME_CNT) + 1);
        reg_wr(ISP_STATUS, reg_rd(ISP_STATUS) | STAT_FRAME_DONE);

        if (ring_push(f) != 0) {
            if (g_verbose)
                printf("[WARN] ring full, frame %u dropped\n", f->seq);
            /* frame is lost here */
        }

        usleep(period_us);
    }

    printf("%s grab thread exit\n", g_log_prefix);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* "encoder"                                                           */
/* ------------------------------------------------------------------ */

static uint32_t encode_frame(Frame* f)
{
    /* placeholder for the real encoder: checksum over the frame */
    uint32_t sum = 0;
    int i;
    int n = f->w * f->h;
    for (i = 0; i < n; i++) {
        sum += f->data[i];
    }
    return sum;
}

/* ------------------------------------------------------------------ */
/* signals                                                             */
/* ------------------------------------------------------------------ */

static void on_sigint(int sig)
{
    (void)sig;
    printf("\n%s signal received, stopping\n", g_log_prefix);
    running = 0;
}

static void on_sigusr1(int sig)
{
    (void)sig;
    reload_gain = 1;
}

/* ------------------------------------------------------------------ */
/* args                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char* prog)
{
    printf("usage: %s [-c config] [-g gain] [-n frames] [-v]\n", prog);
}

static int parse_args(int argc, char** argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            strncpy(g_cfg_path, argv[++i], MAX_CFG_LINE - 1);
        } else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            g_gain = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            g_max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return -1;
        } else {
            printf("[ERR] unknown argument %s\n", argv[i]);
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char** argv)
{
    pthread_t tid;
    uint32_t processed = 0;
    uint64_t t_start;
    uint64_t t_last_stat;
    int rc;

    if (parse_args(argc, argv) != 0)
        return 1;

    load_config(g_cfg_path);

    if (g_fps <= 0 || g_fps > 120) {
        printf("[ERR] bad fps %d\n", g_fps);
        return 1;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGUSR1, on_sigusr1);

    isp_init();

    printf("%s optronic_node starting, isp id 0x%08X version 0x%08X\n",
           g_log_prefix, reg_rd(ISP_ID), reg_rd(ISP_VERSION));

    rc = pthread_create(&tid, NULL, grab_thread, NULL);
    if (rc != 0) {
        printf("[ERR] pthread_create failed: %s\n", strerror(rc));
        return 1;
    }

    t_start = now_us();
    t_last_stat = t_start;

    while (running) {
        Frame* f = ring_pop();
        if (f == NULL) {
            usleep(1000);
            continue;
        }

        uint32_t sum = encode_frame(f);
        uint64_t lat = now_us() - f->ts;
        processed++;

        if (g_verbose || (processed % 30) == 0) {
            printf("%s frame %u sum=%u lat=%lluus\n", g_log_prefix, f->seq,
                   sum, (unsigned long long)lat);
        }

        if (reload_gain) {
            reload_gain = 0;
            g_gain = (g_gain + 64) & GAIN_MASK;
            reg_wr(ISP_GAIN, g_gain);
            printf("%s gain set to %d\n", g_log_prefix, g_gain);
        }

        if (now_us() - t_last_stat > 5000000ULL) {
            t_last_stat = now_us();
            printf("%s stats: processed=%u dropped=%u frame_cnt=%u temp=%d\n",
                   g_log_prefix, processed, frames_dropped,
                   reg_rd(ISP_FRAME_CNT), (int)reg_rd(ISP_TEMP_MC));
        }

        frame_free(f);

        if (g_max_frames > 0 && processed >= (uint32_t)g_max_frames) {
            running = 0;
        }
    }

    pthread_join(tid, NULL);

    /* drain what is left */
    {
        Frame* f;
        while ((f = ring_pop()) != NULL)
            frame_free(f);
    }

    printf("%s done, %u frames in %.1f s\n", g_log_prefix, processed,
           (now_us() - t_start) / 1e6);
    return 0;
}
