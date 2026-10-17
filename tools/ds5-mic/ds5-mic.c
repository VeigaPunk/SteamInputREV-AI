// ds5-mic — DualSense built-in microphone as a Linux audio input, over
// Bluetooth, software-only.
//
// The DualSense does not expose a standard Bluetooth audio profile. The mic
// is Opus-encoded (48 kHz mono, 480-sample frames) and embedded in the
// controller's normal HID input report 0x31: packets with bit 1 of byte 1
// set carry audio (Opus frame at byte 3), everything else is controller
// state. Enabling the mic takes a 142-byte output report 0x32 guarded by a
// CRC-32 seeded with 0xA2 (the same CRC the kernel's hid-playstation driver
// uses for Bluetooth output reports).
//
// Protocol discovery credit: tomarai85/dualsense-bridge (GPL-3.0, macOS)
// documented the wire format, cross-verified against awalol/DS5Dongle and
// the Linux hid-playstation.c driver. This implementation is original code
// written from those protocol facts; nothing is copied.
//
// Write discipline: the enable report doubles as a keepalive and is sent
// from a dedicated thread at 500 ms intervals — never firehosed, never from
// the read path ( SDL/macOS discipline; harmless and good manners on
// Linux hidraw, where the fd is opened non-blocking anyway).
//
// Outputs:
//   default : PipeWire virtual source "DualSense Mic" (select it in apps)
//   --wav F : also/instead write decoded PCM to a WAV file (testing)
//
// Build: make ds5-mic   (needs libopus and libpipewire-0.3)

#define _GNU_SOURCE
#include <opus/opus.h>
#include <pulse/error.h>
#include <pulse/simple.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define VID_SONY      0x054C
#define PID_DUALSENSE 0x0CE6

#define OPUS_RATE     48000
#define OPUS_CH       1
#define OPUS_FRAME    480          // 10 ms
#define HID_READ_MAX  1024

#define ENABLE_LEN    142          // report id + 141 payload
#define ENABLE_PERIOD_MS 500

static volatile sig_atomic_t g_stop = 0;
static bool g_force = false;
static bool g_debug_frames = false;
static void on_signal(int _) { (void)_; g_stop = 1; }

// ---------------------------------------------------------------------------
// Standard CRC-32 (zlib flavor: reflected poly 0xEDB88320, init and final
// complement 0xFFFFFFFF). The DualSense firmware expects it computed over
// the seed byte 0xA2 prepended to the report body — the Bluetooth HID
// output-report guard. Table generated at runtime.
// ---------------------------------------------------------------------------
static uint32_t g_crc_table[256];
static void crc_init(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    g_crc_table[i] = c;
  }
}
static uint32_t crc32_hid(const uint8_t *p, size_t len) {
  uint32_t c = 0xFFFFFFFF;
  c = g_crc_table[(c ^ 0xA2) & 0xFF] ^ (c >> 8);   // seed byte prepended
  for (size_t i = 0; i < len; i++)
    c = g_crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFF;
}

// Mic-enable report 0x32, two accepted shapes (block tag 0x91 = block-valid):
//   mode A (DS5Dongle firmware, "final tested"): block length 1,
//     byte4 = 0b11 mic-on / 0b10 mic-off — no other bits ever set
//   mode B (macOS bridge variant): block length 7, byte4 as in A,
//     audio buffer length 64 in bytes [5:10]
// Sending 0xFF in byte4 (as one published implementation does) latches the
// controller's hardware mic-mute — never set bits above bit 1.
static size_t build_enable(uint8_t *r, uint8_t seq, uint8_t counter, bool on, int mode) {
  memset(r, 0, ENABLE_LEN);
  r[0] = 0x32;
  r[1] = (uint8_t)(seq << 4);
  r[2] = 0x91;
  if (mode == 0) {
    r[3] = 1;
    r[4] = on ? 0b00000011 : 0b00000010;
  } else {
    r[3] = 7;
    r[4] = on ? 0b00000011 : 0b00000010;
    for (int i = 5; i < 10; i++) r[i] = 64;
    r[10] = counter;
  }
  uint32_t crc = crc32_hid(r, ENABLE_LEN - 4);
  r[ENABLE_LEN - 4] = (uint8_t)crc;
  r[ENABLE_LEN - 3] = (uint8_t)(crc >> 8);
  r[ENABLE_LEN - 2] = (uint8_t)(crc >> 16);
  r[ENABLE_LEN - 1] = (uint8_t)(crc >> 24);
  return ENABLE_LEN;
}

// ---------------------------------------------------------------------------
// Device discovery: /sys/class/hidraw/*/device/uevent, Bluetooth bus only
// (bus 0005) — over USB the pad already is a standard USB Audio mic.
// ---------------------------------------------------------------------------
static char *find_bt_dualsense(void) {
  for (int i = 0; i < 64; i++) {
    char path[128], buf[256];
    snprintf(path, sizeof path, "/sys/class/hidraw/hidraw%d/device/uevent", i);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    bool match = false;
    while (fgets(buf, sizeof buf, f)) {
      unsigned bus, vid, pid;
      if (sscanf(buf, "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3 &&
          bus == 5 && vid == VID_SONY && pid == PID_DUALSENSE)
        match = true;
    }
    fclose(f);
    if (match) {
      char *dev = malloc(32);
      snprintf(dev, 32, "/dev/hidraw%d", i);
      return dev;
    }
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// WAV sink
// ---------------------------------------------------------------------------
struct wav {
  FILE *f;
  uint32_t bytes;
};
static void wav_open(struct wav *w, const char *path) {
  w->f = fopen(path, "wb");
  w->bytes = 0;
  if (!w->f) { fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno)); exit(1); }
  fwrite("RIFF\377\377\377\377WAVEfmt ", 1, 16, w->f);  // sizes patched at close
  uint32_t fmtsize = 16;
  uint16_t pcm16[2] = {1 /*PCM*/, OPUS_CH};
  uint32_t rate32[2] = {OPUS_RATE, OPUS_RATE * OPUS_CH * 2};
  uint16_t tail16[2] = {OPUS_CH * 2 /*block align*/, 16 /*bits*/};
  fwrite(&fmtsize, 4, 1, w->f);
  fwrite(pcm16, 2, 2, w->f);
  fwrite(rate32, 4, 2, w->f);
  fwrite(tail16, 2, 2, w->f);
  fwrite("data\377\377\377\377", 1, 8, w->f);
}
static void wav_write(struct wav *w, const int16_t *pcm, int n) {
  if (!w->f) return;
  fwrite(pcm, 2, n, w->f);
  w->bytes += n * 2;
}
static void wav_close(struct wav *w) {
  if (!w->f) return;
  uint32_t riff = 36 + w->bytes;
  fseek(w->f, 4, SEEK_SET);  fwrite(&riff, 4, 1, w->f);
  fseek(w->f, 40, SEEK_SET); fwrite(&w->bytes, 4, 1, w->f);
  fclose(w->f);
  fprintf(stderr, "wav: %u bytes PCM (%.1f s)\n", w->bytes, (double)w->bytes / (OPUS_RATE * 2));
}

// ---------------------------------------------------------------------------
// Virtual microphone via null sink + monitor: the daemon plays decoded mic
// audio into a pactl-created null sink; the sink's monitor port is a normal
// recordable source every app can select. libpulse playback against
// pipewire-pulse handles all routing and pacing.
// ---------------------------------------------------------------------------
#define PW_SINK_NAME "ds5-mic-vsink"

struct pa_ctx {
  pa_simple *s;
  char module_id[32];
};

static void pa_ctx_init(struct pa_ctx *c) {
  memset(c, 0, sizeof *c);
  // create the null sink in the server via the pulse interface; pactl
  // prints the module index on stdout (needed to remove it on exit)
  FILE *p = popen(
      "pactl load-module module-null-sink sink_name=" PW_SINK_NAME
      " sink_properties=device.description=DualSense-Mic-Virtual",
      "r");
  if (p) {
    char out[128];
    if (fgets(out, sizeof out, p)) {
      unsigned long id = 0;
      if (sscanf(out, "%lu", &id) == 1)
        snprintf(c->module_id, sizeof c->module_id, "%lu", id);
    }
    pclose(p);
  }
  if (!c->module_id[0])
    fprintf(stderr, "warn: null-sink creation not confirmed; continuing\n");
  usleep(300000);

  // stereo: a mono channel map silences this PipeWire's null-sink monitor
  pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = OPUS_RATE, .channels = 2 };
  int err = 0;
  c->s = pa_simple_new(NULL, "ds5-mic", PA_STREAM_PLAYBACK, PW_SINK_NAME,
                       "DualSense microphone (Bluetooth)", &ss, NULL, NULL, &err);
  if (!c->s) {
    fprintf(stderr, "pulse: playback open failed: %s\n", pa_strerror(err));
    exit(1);
  }
}

static void pa_push(struct pa_ctx *c, const int16_t *pcm, int n) {
  int16_t stereo[OPUS_FRAME * 2];
  for (int i = 0; i < n; i++) {
    stereo[i * 2] = pcm[i];
    stereo[i * 2 + 1] = pcm[i];
  }
  int err = 0;
  if (pa_simple_write(c->s, stereo, (size_t)n * 4, &err) < 0)
    fprintf(stderr, "pulse: write: %s\n", pa_strerror(err));
}

static void pa_ctx_fini(struct pa_ctx *c) {
  int err = 0;
  if (c->s) {
    pa_simple_drain(c->s, &err);
    pa_simple_free(c->s);
  }
  if (c->module_id[0]) {
    char cmd[96];
    snprintf(cmd, sizeof cmd, "pactl unload-module %s >/dev/null 2>&1", c->module_id);
    system(cmd);
  }
}

// ---------------------------------------------------------------------------
// Kernel-driver management. While mic audio streams, report 0x31 carries
// Opus frames that hid-playstation (and anything else parsing the pad's
// input reports as controls) misreads as phantom button/gyro activity.
// Unbinding the driver for the duration removes the kernel-side phantom
// input; rebinding restores normal controller behavior on exit.
// Sysfs bind/unbind needs root: the tool tries, prints the exact sudo
// command if denied, and refuses to stream without it unless --force.
// ---------------------------------------------------------------------------
static char g_hid_devname[64];   // e.g. 0005:054C:0CE6.000E
static bool g_was_unbound = false;

static bool sysfs_write(const char *path, const char *val) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return false;
  ssize_t r = write(fd, val, strlen(val));
  close(fd);
  return r >= 0;
}

// map /dev/hidrawN -> HID device sysfs name, or NULL
static char *hid_device_name(const char *dev) {
  const char *base = strrchr(dev, '/');
  base = base ? base + 1 : dev;
  char link[128], path[96];
  snprintf(path, sizeof path, "/sys/class/hidraw/%s", base);
  ssize_t n = readlink(path, link, sizeof link - 1);
  if (n < 0) return NULL;
  link[n] = 0;
  // link is like ../../../devices/.../0005:054C:0CE6.000E/hidraw/hidraw12
  char *slash = strrchr(link, '/');
  if (!slash) return NULL;
  *slash = 0;
  char *name = strrchr(link, '/');
  if (!name) return NULL;
  name++;
  if (strstr(name, ":") == NULL) return NULL;
  return strdup(name);
}

static bool driver_swap(const char *hid_name, const char *from, const char *to) {
  char path[128];
  snprintf(path, sizeof path, "/sys/bus/hid/drivers/%s/unbind", from);
  bool unbound = sysfs_write(path, hid_name);
  if (!unbound && errno != ENODEV) return false;
  snprintf(path, sizeof path, "/sys/bus/hid/drivers/%s/bind", to);
  if (!sysfs_write(path, hid_name)) return false;
  return true;
}

// swap hid-playstation (which parses report 0x31 as controller state and
// therefore misreads Opus frames as phantom input) for hid-generic, which
// leaves the vendor reports alone but keeps a hidraw node alive. The
// controller works as a gamepad again only after swap-back on exit.
static bool driver_to_generic(const char *dev, bool quiet) {
  char *name = hid_device_name(dev);
  if (!name) return false;
  snprintf(g_hid_devname, sizeof g_hid_devname, "%s", name);

  char drvlink[192];
  const char *base = strrchr(dev, '/');
  base = base ? base + 1 : dev;
  snprintf(drvlink, sizeof drvlink, "/sys/class/hidraw/%s/device/driver", base);
  char cur[64] = "?";
  ssize_t n = readlink(drvlink, cur, sizeof cur - 1);
  if (n > 0) { cur[n] = 0; char *s = strrchr(cur, '/'); if (s) memmove(cur, s + 1, strlen(s)); }

  if (!strcmp(cur, "hid-generic")) {
    if (!quiet) fprintf(stderr, "already on hid-generic\n");
    free(name);
    return true;
  }
  if (!strcmp(cur, "playstation")) {
    if (driver_swap(name, "playstation", "hid-generic")) {
      g_was_unbound = true;
      if (!quiet)
        fprintf(stderr, "driver: playstation -> hid-generic (%s) — no phantom input\n", name);
      free(name);
      return true;
    }
  }
  free(name);
  return false;
}

static void driver_restore(void) {
  if (!g_was_unbound) return;
  if (!driver_swap(g_hid_devname, "hid-generic", "playstation"))
    fprintf(stderr,
            "warn: could not restore hid-playstation — run:\n"
            "  echo '%s' | sudo tee /sys/bus/hid/drivers/hid-generic/unbind\n"
            "  echo '%s' | sudo tee /sys/bus/hid/drivers/playstation/bind\n",
            g_hid_devname, g_hid_devname);
  else
    fprintf(stderr, "driver: hid-generic -> playstation — controller input restored\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
struct shared {
  int fd;
  bool use_pa;
  struct pa_ctx pa;
  struct wav wav;
  float gain;
  volatile int enable_mode;    // 0 = DS5Dongle shape, 1 = bridge shape
  volatile unsigned long frames_seen;
};

static void *writer_thread(void *arg) {
  struct shared *s = arg;
  uint8_t seq = 0, counter = 0;
  uint8_t rep[ENABLE_LEN];
  int mode = s->enable_mode;
  unsigned long frames_at_switch = 0;
  int mode_deadline = 5;   // re-evaluate after ~5 half-second cycles
  while (!g_stop) {
    // if the current enable shape produced no audio frames after a few
    // keepalives, try the other shape once
    if (--mode_deadline <= 0) {
      if (s->frames_seen == frames_at_switch && mode == 0) {
        mode = 1;
        fprintf(stderr, "enable: no audio with firmware shape, trying bridge shape\n");
      }
      frames_at_switch = s->frames_seen;
      mode_deadline = 6;
    }
    s->enable_mode = mode;
    size_t n = build_enable(rep, seq++, counter++, true, mode);
    if (write(s->fd, rep, n) < 0 && errno != EAGAIN)
      fprintf(stderr, "writer: %s\n", strerror(errno));
    seq &= 0x0F;
    for (int ms = 0; ms < ENABLE_PERIOD_MS && !g_stop; ms += 20) usleep(20000);
  }
  size_t n = build_enable(rep, seq, counter, false, mode);  // mic off on exit
  write(s->fd, rep, n);
  return NULL;
}

int main(int argc, char **argv) {
  const char *wav_path = NULL, *dev = NULL;
  double secs = 0, gain = 6.0;
  bool wav_only = false;
  for (int i = 1; i < argc; i++) {
    const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (!strcmp(argv[i], "--wav") && val) { wav_path = val; i++; }
    else if (!strcmp(argv[i], "--wav-only")) wav_only = true;
    else if (!strcmp(argv[i], "--device") && val) { dev = val; i++; }
    else if (!strcmp(argv[i], "--secs") && val) { secs = atof(val); i++; }
    else if (!strcmp(argv[i], "--gain") && val) { gain = atof(val); i++; }
    else if (!strcmp(argv[i], "--force")) g_force = true;
    else if (!strcmp(argv[i], "--debug-frames")) g_debug_frames = true;
    else if (!strcmp(argv[i], "-h")) {
      fprintf(stderr,
              "usage: ds5-mic [--device /dev/hidrawN] [--wav F] [--wav-only] [--secs S]\n"
              "               [--gain X] [--force]\n"
              "\n"
              "Runs as a normal user when hid-playstation can be unbound with sudo;\n"
              "--force streams anyway (expect phantom controller input while live).\n");
      return 0;
    }
  }

  crc_init();
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  char *found = NULL;
  if (!dev) { found = find_bt_dualsense(); dev = found; }
  if (!dev) { fprintf(stderr, "no Bluetooth DualSense found (hidraw scan)\n"); return 2; }
  fprintf(stderr, "device: %s\n", dev);

  // phantom-input protection: swap hid-playstation (whose parser misreads
  // Opus frames as buttons) for hid-generic BEFORE opening — the swap
  // recreates the hidraw node, possibly renumbered
  if (!driver_to_generic(dev, false)) {
    char *nm = hid_device_name(dev);
    fprintf(stderr,
            "cannot swap hid-playstation -> hid-generic as this user. Run:\n"
            "  echo '%s' | sudo tee /sys/bus/hid/drivers/playstation/unbind\n"
            "  echo '%s' | sudo tee /sys/bus/hid/drivers/hid-generic/bind\n"
            "then rerun (the tool restores playstation on exit) — or pass\n"
            "--force to stream anyway (Opus frames will appear as phantom\n"
            "controller input to everything parsing the pad).\n",
            nm ? nm : "0005:054C:0CE6.XXXX", nm ? nm : "0005:054C:0CE6.XXXX");
    free(nm);
    if (!g_force) { free(found); return 2; }
    fprintf(stderr, "--force: streaming without the driver swap\n");
  } else {
    // re-resolve: the node was recreated
    free(found);
    found = NULL;
    if (g_was_unbound) {
      found = find_bt_dualsense();
      dev = found;
      if (!dev) { fprintf(stderr, "hidraw vanished after driver swap\n"); driver_restore(); return 2; }
      fprintf(stderr, "device: %s (renumbered after swap)\n", dev);
    }
  }

  int fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) { fprintf(stderr, "open %s: %s\n", dev, strerror(errno)); driver_restore(); return 2; }
  free(found);

  // extended report mode: read feature report 0x05 (safe; usually already set
  // once anything has asked the pad for its state)
  uint8_t feat[41] = {0x05};
  if (ioctl(fd, HIDIOCGFEATURE(41), feat) < 0)
    fprintf(stderr, "warn: feature 0x05 read: %s (continuing)\n", strerror(errno));

  int err;
  OpusDecoder *dec = opus_decoder_create(OPUS_RATE, OPUS_CH, &err);
  if (!dec) { fprintf(stderr, "opus decoder: %d\n", err); return 2; }

  struct shared s = { .fd = fd, .gain = (float)gain };
  // --wav records alongside the sink unless --wav-only is given
  if (wav_path) wav_open(&s.wav, wav_path);
  if (!wav_only) { s.use_pa = true; pa_ctx_init(&s.pa); }

  pthread_t wt;
  pthread_create(&wt, NULL, writer_thread, &s);

  fprintf(stderr, "mic live (%s%s%s) — Ctrl-C to stop\n",
          s.use_pa ? "select '" PW_SINK_NAME ".monitor' as your input device" : "",
          s.use_pa && wav_path ? " + " : "", wav_path ? wav_path : "");
  struct timespec t0, tnow;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  unsigned long frames = 0, state_pkts = 0;
  int16_t pcm[OPUS_FRAME];

  while (!g_stop) {
    struct pollfd pf = { .fd = fd, .events = POLLIN };
    int pr = poll(&pf, 1, 100);
    if (pr <= 0) continue;
    uint8_t buf[HID_READ_MAX];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) continue;
    if (buf[0] == 0x31 && n >= 75 && ((buf[1] >> 1) & 1)) {
      frames++;
      s.frames_seen = frames;
      if (g_debug_frames && frames < 3) {
        fprintf(stderr, "audio frame %lu raw[0:24]:", frames);
        for (int i = 0; i < 24; i++) fprintf(stderr, " %02x", buf[i]);
        fprintf(stderr, "\n");
      }
      int ns = opus_decode(dec, buf + 3, 71, pcm, OPUS_FRAME, 0);
      if (ns > 0) {
        if (s.gain != 1.0f)
          for (int i = 0; i < ns; i++) {
            long v = (long)(pcm[i] * s.gain);
            pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
          }
        if (s.use_pa) pa_push(&s.pa, pcm, ns);
        wav_write(&s.wav, pcm, ns);
      }
    } else {
      state_pkts++;
    }
    if (secs > 0) {
      clock_gettime(CLOCK_MONOTONIC, &tnow);
      if ((double)(tnow.tv_sec - t0.tv_sec) + 1e-9 * (tnow.tv_nsec - t0.tv_nsec) >= secs)
        break;
    }
  }

  g_stop = 1;
  pthread_join(wt, NULL);
  fprintf(stderr, "audio frames: %lu (%.1f s), state packets: %lu\n",
          frames, frames / 100.0, state_pkts);
  driver_restore();
  if (s.use_pa) pa_ctx_fini(&s.pa);
  wav_close(&s.wav);
  opus_decoder_destroy(dec);
  close(fd);
  return frames > 0 ? 0 : 4;
}
