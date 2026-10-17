// ds5-input — live DualSense input mapper over raw Bluetooth HID.
//
// Decodes the controller's state report 0x31 directly from hidraw and
// prints every control with three identities: the raw report byte/bit,
// the Linux kernel event it maps to (hid-playstation), and the Steam
// Input action origin it surfaces as (Steamworks SDK 1.65, SteamInput007).
// Press buttons while it runs to identify them interactively.
//
// Audio-variant 0x31 frames (bit 1 of byte 1 set — Opus mic data, see
// tools/ds5-mic) are counted and skipped, never parsed as controls.
//
// Report 0x31 layout below transcribed from the kernel's hid-playstation.c
// struct dualsense_input_report; origin indices parsed from the SDK's
// isteaminput.h (PS5 block starts at k_EInputActionOrigin index 258).

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define VID_SONY      0x054C
#define PID_DUALSENSE 0x0CE6
#define READ_MAX      1024

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int _) { (void)_; g_stop = 1; }

struct btn {
  const char *name;      // Sony/hardware name
  int byte, bit;         // buttons[] offset from raw[9]
  const char *kern;      // kernel input event
  int origin;            // Steam Input EInputActionOrigin index
  const char *origin_name;
};

static const struct btn k_buttons[] = {
  {"cross    (✕)",      0, 5, "BTN_SOUTH",  258, "PS5_X"},
  {"circle   (○)",      0, 6, "BTN_EAST",   259, "PS5_Circle"},
  {"triangle (△)",      0, 7, "BTN_NORTH",  260, "PS5_Triangle"},
  {"square   (□)",      0, 4, "BTN_WEST",   261, "PS5_Square"},
  {"L1 bumper",         1, 0, "BTN_TL",     262, "PS5_LeftBumper"},
  {"R1 bumper",         1, 1, "BTN_TR",     263, "PS5_RightBumper"},
  {"L2 click",          1, 2, "BTN_TL2",    289, "PS5_LeftTrigger_Click"},
  {"R2 click",          1, 3, "BTN_TR2",    291, "PS5_RightTrigger_Click"},
  {"create   (select)", 1, 4, "BTN_SELECT", 265, "PS5_Create"},
  {"options  (start)",  1, 5, "BTN_START",  264, "PS5_Option"},
  {"L3 stick click",    1, 6, "BTN_THUMBL", 293, "PS5_LeftStick_Click"},
  {"R3 stick click",    1, 7, "BTN_THUMBR", 299, "PS5_RightStick_Click"},
  {"PS home",           2, 0, "BTN_MODE",   -1,  "system-reserved (no PS5 origin)"},
  {"touchpad click",    2, 1, "touchpad",   276, "PS5_RightPad_Click"},
  {"mic mute",          2, 2, "mute LED",   266, "PS5_Mute"},
};
static const int k_nbuttons = sizeof k_buttons / sizeof k_buttons[0];

static const char *dpad_name(int v) {
  switch (v) {
    case 0: return "north"; case 1: return "north-east";
    case 2: return "east";  case 3: return "south-east";
    case 4: return "south"; case 5: return "south-west";
    case 6: return "west";  case 7: return "north-west";
    case 8: return "released";
    default: return "invalid";
  }
}

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

static void legend(void) {
  puts("DualSense Bluetooth report 0x31 — full control map");
  puts("raw offsets: sticks raw[2..5], triggers raw[6..7], buttons raw[9..12]");
  puts("             gyro raw[17..22], accel raw[23..28], touch raw[34..41],");
  puts("             battery raw[54] (capacity = low nibble x10%)");
  puts("");
  printf("%-18s %-12s %-12s %-6s %s\n", "control", "report", "kernel", "origin", "Steam Input origin");
  printf("%-18s %-12s %-12s %-6s %s\n", "-------", "------", "------", "------", "------------------");
  printf("%-18s %-12s %-12s %-6s %s\n", "left stick", "raw[2],[3]", "ABS_X/Y", "292", "PS5_LeftStick_Move");
  printf("%-18s %-12s %-12s %-6s %s\n", "right stick", "raw[4],[5]", "ABS_RX/RY", "298", "PS5_RightStick_Move");
  printf("%-18s %-12s %-12s %-6s %s\n", "L2 analog", "raw[6]", "ABS_Z", "288", "PS5_LeftTrigger_Pull");
  printf("%-18s %-12s %-12s %-6s %s\n", "R2 analog", "raw[7]", "ABS_RZ", "290", "PS5_RightTrigger_Pull");
  printf("%-18s %-12s %-12s %-6s %s\n", "d-pad", "raw[9] 0:3", "ABS_HAT0X/Y", "304-7", "PS5_DPad_N/S/W/E");
  for (int i = 0; i < k_nbuttons; i++) {
    const struct btn *b = &k_buttons[i];
    char rep[16], orig[16];
    snprintf(rep, sizeof rep, "raw[9+%d].%d", b->byte, b->bit);
    if (b->origin >= 0) snprintf(orig, sizeof orig, "%d", b->origin);
    else snprintf(orig, sizeof orig, "-");
    printf("%-18s %-12s %-12s %-6s %s\n", b->name, rep, b->kern, orig, b->origin_name);
  }
  puts("");
  puts("sources: linux/drivers/hid/hid-playstation.c (report layout),");
  puts("         Steamworks SDK 1.65 isteaminput.h (origin indices)");
}

int main(int argc, char **argv) {
  bool json = false, raw_hex = false, show_legend = false;
  double secs = 0;
  for (int i = 1; i < argc; i++) {
    const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (!strcmp(argv[i], "--json")) json = true;
    else if (!strcmp(argv[i], "--raw")) raw_hex = true;
    else if (!strcmp(argv[i], "--legend")) show_legend = true;
    else if (!strcmp(argv[i], "--secs") && val) { secs = atof(val); i++; }
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      fprintf(stderr, "usage: ds5-input [--legend] [--json] [--raw] [--secs N]\n");
      return 0;
    }
  }
  if (show_legend) { legend(); return 0; }

  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);
  setvbuf(stdout, NULL, _IOLBF, 0);

  char *dev = find_bt_dualsense();
  if (!dev) { fprintf(stderr, "no Bluetooth DualSense found\n"); return 2; }
  fprintf(stderr, "device: %s — press buttons to identify them (Ctrl-C stops)\n", dev);
  int fd = open(dev, O_RDONLY | O_NONBLOCK);
  if (fd < 0) { fprintf(stderr, "open %s: %s\n", dev, strerror(errno)); return 2; }
  free(dev);

  bool last_btn[16] = {false};
  int last_hat = 8, last_lx = -1, last_ly = -1, last_rx = -1, last_ry = -1;
  int last_l2 = -1, last_r2 = -1, last_batt = -1;
  unsigned long state_pkts = 0, audio_pkts = 0;
  struct timespec t0, tnow;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  while (!g_stop) {
    struct pollfd pf = { .fd = fd, .events = POLLIN };
    int pr = poll(&pf, 1, 100);
    if (pr <= 0) {
      if (secs > 0) {
        clock_gettime(CLOCK_MONOTONIC, &tnow);
        if ((double)(tnow.tv_sec - t0.tv_sec) >= secs) break;
      }
      continue;
    }
    uint8_t b[READ_MAX];
    ssize_t n = read(fd, b, sizeof b);
    if (n < 78 || b[0] != 0x31) continue;
    if ((b[1] >> 1) & 1) { audio_pkts++; continue; }   // Opus mic frame, never a control
    state_pkts++;

    if (raw_hex) {
      printf("state:");
      for (int i = 0; i < 78; i++) printf(" %02x", b[i]);
      printf("\n");
      continue;
    }

    int hat = b[9] & 0x0F;
    int lx = b[2], ly = b[3], rx = b[4], ry = b[5], l2 = b[6], r2 = b[7];
    int batt = b[54] & 0x0F;

    for (int i = 0; i < k_nbuttons; i++) {
      const struct btn *bt = &k_buttons[i];
      bool down = (b[9 + bt->byte] >> bt->bit) & 1;
      if (down != last_btn[i]) {
        last_btn[i] = down;
        if (json)
          printf("{\"event\":\"button\",\"control\":\"%s\",\"state\":%s,\"report\":\"raw[9+%d].bit%d\",\"kernel\":\"%s\",\"origin\":%d,\"origin_name\":\"%s\"}\n",
                 bt->name, down ? "true" : "false", bt->byte, bt->bit,
                 bt->kern, bt->origin, bt->origin_name);
        else
          printf("%-18s %-4s  raw[9+%d].bit%-2d kernel %-11s origin %s\n",
                 bt->name, down ? "DOWN" : "up", bt->byte, bt->bit,
                 bt->kern, bt->origin_name);
      }
    }
    if (hat != last_hat) {
      last_hat = hat;
      if (hat != 8)
        printf("d-pad %-11s  raw[9].0:3=%d   kernel ABS_HAT0   origin PS5_DPad_N/S/W/E (304-307)\n",
               dpad_name(hat), hat);
      else
        printf("d-pad released\n");
    }
    if (abs(lx - last_lx) > 3 || abs(ly - last_ly) > 3) {
      last_lx = lx; last_ly = ly;
      printf("left stick  %3d,%3d   raw[2],[3]  kernel ABS_X/Y   origin PS5_LeftStick_Move (292)\n", lx, ly);
    }
    if (abs(rx - last_rx) > 3 || abs(ry - last_ry) > 3) {
      last_rx = rx; last_ry = ry;
      printf("right stick %3d,%3d   raw[4],[5]  kernel ABS_RX/RY  origin PS5_RightStick_Move (298)\n", rx, ry);
    }
    if (abs(l2 - last_l2) > 3 || abs(r2 - last_r2) > 3) {
      last_l2 = l2; last_r2 = r2;
      printf("triggers    L2=%3d R2=%3d  raw[6],[7]  kernel ABS_Z/RZ  origins PS5_{Left,Right}Trigger_Pull (288/290)\n", l2, r2);
    }
    if (batt != last_batt) {
      last_batt = batt;
      printf("battery     ~%d%%   raw[54] low nibble  charging bits 4:5\n", batt * 10 < 100 ? batt * 10 + 5 : 100);
    }

    if (secs > 0) {
      clock_gettime(CLOCK_MONOTONIC, &tnow);
      if ((double)(tnow.tv_sec - t0.tv_sec) >= secs) break;
    }
  }

  fprintf(stderr, "state packets: %lu, audio-variant packets skipped: %lu\n",
          state_pkts, audio_pkts);
  close(fd);
  return 0;
}
