// dq38 - bare-metal RAM data-line test (UEFI application).
//
// Claims all free RAM while boot services are still active, writes patterns, reads them back
// several times and records every flipped bit by *data line*: which bit of the 64-bit data word
// (DQ0..DQ63 = byte lane 0..7, bit 0..7) and which beat of the cache-line burst it arrived in.
// After a mismatch the word is re-read from the cache and, after CLFLUSH, from DRAM, to tell a bad
// transfer ("transient") from a wrong value that is actually stored in DRAM ("persistent").
//
// Configuration: \dq38\run.cfg on the volume the program was loaded from (KEY=value lines).
// Log: appended to \dq38\dq38.log on the same volume. See README.md.
#include <efi.h>
#include <stdarg.h>

#define VERSION "dq38 1.0.0"
#define MAXREG 64
#define LOGCAP (1u << 20)
#define CFGCAP 4096
#define MAX_HIT_LINES 400
#define NONE (~0ULL)

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_RUNTIME_SERVICES *RT;
static EFI_HANDLE IMG;
static EFI_GUID gLI = LOADED_IMAGE_PROTOCOL;
static EFI_GUID gFS = SIMPLE_FILE_SYSTEM_PROTOCOL;
static EFI_GUID gMP = EFI_MP_SERVICES_PROTOCOL_GUID;
static EFI_GUID gSMBIOS = {0xeb9d2d31, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
static EFI_GUID gSMBIOS3 = {0xf2fd1544, 0x9794, 0x4a2c, {0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94}};

static char *logbuf;
static UINTN loglen, logflushed;
static struct { UINT64 base, bytes; } reg[MAXREG];
static int nreg;
static UINT64 tsc_hz, tsc0;
static UINT64 hist_dq[64], hist_beat[8], hist_abit[64];
static UINT64 hits_total, hits_transient, hits_persistent, hit_lines;
static int is_intel;

// --- text UI state ---
#define CELLS 256
#define CELLCOLS 64
static int quiet;                    // 1 during the positive control: count, but do not log
static int tui;                      // 1 while the fixed-layout screen is active
static UINT8 cell_state[CELLS];      // 0 untested, 1 written, 2 verified
static UINT32 cell_hits[CELLS];
static UINT64 cell_bytes, total_bytes;
static int cur_cell, ncells;
static char lasthit[5][80];
static int ui_pass, ui_read, ui_reads; static const char *ui_phase = "", *ui_pat = "";
static UINT64 ui_run_s;
static char board[64], cpuname[52];
static int ndimm;

void *memset(void *d, int c, UINTN n) {
  void *r = d;
  __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
  return r;
}
void *memcpy(void *d, const void *s, UINTN n) {
  void *r = d;
  __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
  return r;
}

static UINT64 rdtsc(void) { UINT32 a, d; __asm__ volatile("rdtsc" : "=a"(a), "=d"(d)); return ((UINT64)d << 32) | a; }
static UINT64 rdmsr(UINT32 m) { UINT32 a, d; __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(m)); return ((UINT64)d << 32) | a; }
static void cpuid(UINT32 l, UINT32 *a, UINT32 *b, UINT32 *c, UINT32 *d) {
  __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(l), "c"(0));
}
static UINT64 secs(void) { return (rdtsc() - tsc0) / tsc_hz; }

// --- idle modes: let the memory controller put the DIMMs into power-down / self-refresh ---
// The CPU sleeps in MWAIT (deep C-state hint) or HLT and is woken by the firmware timer tick, so the
// next read is the "first read after wake-up". With PARK=1 the other cores sleep in MWAIT for the
// whole run, because the package (and with it the memory) only sleeps when every core does.
static UINT64 idle_s, idle_ms, idle_alt, idle_on = 1, mwait_hint = 0x60, idle_wakes;
static int have_mwait;
static volatile UINT64 ap_mon, ap_parked;
static void EFIAPI ap_park(void *arg) {
  (void)arg; __sync_fetch_and_add(&ap_parked, 1);
  for (;;) {
    __asm__ volatile("monitor" : : "a"(&ap_mon), "c"(0), "d"(0));
    __asm__ volatile("mwait" : : "a"((UINT32)mwait_hint), "c"(0));
  }
}
static EFI_STATUS park_aps(void) {
  EFI_MP_SERVICES_PROTOCOL *mp; EFI_EVENT ev;
  EFI_STATUS st = BS->LocateProtocol(&gMP, NULL, (void **)&mp);
  if (EFI_ERROR(st)) return st;
  st = BS->CreateEvent(0, 0, NULL, NULL, &ev);
  if (EFI_ERROR(st)) return st;
  st = mp->StartupAllAPs(mp, ap_park, FALSE, ev, 0, NULL, NULL);
  BS->Stall(100000);
  return st;
}
static void idle_usec(UINT64 us) {
  UINT64 end = rdtsc() + tsc_hz / 1000000 * us;
  static volatile UINT64 mon;
  while (rdtsc() < end) {
    if (have_mwait) {
      __asm__ volatile("monitor" : : "a"(&mon), "c"(0), "d"(0));
      __asm__ volatile("mwait" : : "a"((UINT32)mwait_hint), "c"(1));
    } else __asm__ volatile("hlt");
    idle_wakes++;
  }
}

// CPU temperature from Intel's digital thermal sensor; 0 if unavailable (other vendors, QEMU).
static int cpu_temp(void) {
  UINT32 a, b, c, d;
  if (!is_intel) return 0;
  cpuid(0, &a, &b, &c, &d);
  if (a < 6) return 0;
  cpuid(6, &a, &b, &c, &d);
  if (!(a & 1)) return 0;
  int t = (int)((rdmsr(0x1A2) >> 16) & 0xff) - (int)((rdmsr(0x19C) >> 16) & 0x7f);
  return t < 0 ? 0 : t;
}

static void cpu_identify(void) {
  UINT32 r[12], a, b, c, d;
  cpuid(0, &a, &b, &c, &d);
  is_intel = b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e;   // "GenuineIntel"
  cpuid(0x80000000, &a, &b, &c, &d);
  if (a < 0x80000004) { memcpy(cpuname, "unknown CPU", 12); return; }
  for (UINT32 i = 0; i < 3; i++) cpuid(0x80000002 + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
  const char *s = (const char *)r; int n = 0, sp = 1;
  for (int i = 0; i < 48 && s[i]; i++) {                 // collapse the padding spaces vendors put in
    if (s[i] == ' ') { if (!sp) cpuname[n++] = ' '; sp = 1; } else { cpuname[n++] = s[i]; sp = 0; }
  }
  while (n && cpuname[n - 1] == ' ') n--;
  cpuname[n] = 0;
}

// Minimal formatter: %s %d (UINT64 decimal) %x (UINT64 hex) %c, optional zero-padded width.
static UINTN fmt(char *o, UINTN cap, const char *f, va_list ap) {
  UINTN n = 0;
#define PUT(ch) do { if (n + 1 < cap) o[n++] = (ch); } while (0)
  for (; *f; f++) {
    if (*f != '%') { PUT(*f); continue; }
    f++;
    int width = 0;
    while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    if (*f == 's') { const char *s = va_arg(ap, const char *); while (*s) PUT(*s++); }
    else if (*f == 'c') { PUT((char)va_arg(ap, int)); }
    else if (*f == 'd' || *f == 'x') {
      UINT64 v = va_arg(ap, UINT64); UINT64 base = *f == 'd' ? 10 : 16;
      char t[24]; int k = 0;
      do { UINT64 q = v % base; t[k++] = (char)(q < 10 ? '0' + q : 'a' + q - 10); v /= base; } while (v);
      while (k < width) t[k++] = '0';
      while (k) PUT(t[--k]);
    }
  }
  o[n] = 0;
  return n;
#undef PUT
}

static UINTN sfmt(char *o, UINTN cap, const char *f, ...) {
  va_list ap; va_start(ap, f); UINTN n = fmt(o, cap, f, ap); va_end(ap); return n;
}

// Print to the console (unless the fixed-layout screen is up); with tolog also append to the log.
static void emit(int tolog, const char *f, ...) {
  static char line[512];
  static CHAR16 w[1100];
  va_list ap; va_start(ap, f);
  UINTN n = fmt(line, sizeof line, f, ap);
  va_end(ap);
  UINTN k = 0;
  for (UINTN i = 0; i < n; i++) { if (line[i] == '\n') w[k++] = L'\r'; w[k++] = (CHAR16)line[i]; }
  w[k] = 0;
  if (!tui) ST->ConOut->OutputString(ST->ConOut, w);
  if (tolog && logbuf && loglen + n < LOGCAP) { memcpy(logbuf + loglen, line, n); loglen += n; }
}

// --- SMBIOS: board name and the populated memory slots, so the log says what was tested ---
static const char *smb_str(const UINT8 *s, int idx, const UINT8 *end) {
  if (!idx) return "";
  const char *p = (const char *)s + s[1];
  while (--idx > 0 && (const UINT8 *)p < end) { while ((const UINT8 *)p < end && *p) p++; p++; }
  return (const UINT8 *)p < end ? p : "";
}
static void copy_trim(char *dst, UINTN cap, const char *src) {
  UINTN n = 0;
  while (*src == ' ') src++;
  while (*src && n + 1 < cap) dst[n++] = *src++;
  while (n && dst[n - 1] == ' ') n--;
  dst[n] = 0;
}
static int guid_eq(EFI_GUID *a, EFI_GUID *b) {
  const UINT8 *x = (const UINT8 *)a, *y = (const UINT8 *)b;
  for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
  return 1;
}
static void smbios_scan(void) {
  const UINT8 *tab = NULL; UINT64 len = 0;
  for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
    EFI_CONFIGURATION_TABLE *ct = &ST->ConfigurationTable[i];
    const UINT8 *ep = (const UINT8 *)ct->VendorTable;
    if (guid_eq(&ct->VendorGuid, &gSMBIOS3) && ep[0] == '_' && ep[1] == 'S' && ep[2] == 'M' && ep[3] == '3') {
      tab = (const UINT8 *)(UINTN)*(const UINT64 *)(ep + 0x10); len = *(const UINT32 *)(ep + 0x0C); break;
    }
    if (guid_eq(&ct->VendorGuid, &gSMBIOS) && ep[0] == '_' && ep[1] == 'S' && ep[2] == 'M' && ep[3] == '_') {
      tab = (const UINT8 *)(UINTN)*(const UINT32 *)(ep + 0x18); len = *(const UINT16 *)(ep + 0x16);
    }
  }
  if (!tab) { emit(1, "smbios: not found\n"); return; }
  const UINT8 *p = tab, *end = tab + len;
  static char sys[64], base[64];
  while (p + 4 <= end && p[1] >= 4) {
    UINT8 type = p[0], hl = p[1];
    const UINT8 *nx = p + hl;                                   // structure ends with a double NUL
    while (nx + 1 < end && (nx[0] || nx[1])) nx++;
    nx += 2;
    if (type == 127) break;
    if ((type == 1 || type == 2) && hl >= 6) {
      char m[32], n[32]; copy_trim(m, sizeof m, smb_str(p, p[4], end)); copy_trim(n, sizeof n, smb_str(p, p[5], end));
      char *dst = type == 1 ? sys : base;
      if (!dst[0]) sfmt(dst, 64, "%s %s", m, n);
    }
    if (type == 17 && hl >= 0x15) {
      UINT64 mb = *(const UINT16 *)(p + 0x0C);
      if (mb == 0x7FFF && hl >= 0x20) mb = *(const UINT32 *)(p + 0x1C);
      else if (mb != 0xFFFF && (mb & 0x8000)) mb = (mb & 0x7FFF) >> 10;   // size given in KiB
      char loc[24], bank[24], man[24], part[24], sn[24];
      copy_trim(loc, sizeof loc, smb_str(p, p[0x10], end));
      copy_trim(bank, sizeof bank, smb_str(p, p[0x11], end));
      if (mb && mb != 0xFFFF) {
        ndimm++;
        UINT64 speed = hl >= 0x17 ? *(const UINT16 *)(p + 0x15) : 0;
        copy_trim(man, sizeof man, hl >= 0x18 ? smb_str(p, p[0x17], end) : "");
        copy_trim(sn, sizeof sn, hl >= 0x19 ? smb_str(p, p[0x18], end) : "");
        copy_trim(part, sizeof part, hl >= 0x1B ? smb_str(p, p[0x1A], end) : "");
        emit(1, "dimm: %s / %s: %d MiB %d MT/s %s %s sn %s\n", loc, bank, mb, speed, man, part, sn);
      } else emit(1, "dimm: %s / %s: empty\n", loc, bank);
    }
    p = nx;
  }
  copy_trim(board, sizeof board, base[0] ? base : sys);
  emit(1, "system: %s\nboard: %s\n", sys, base);
}

// --- screen ---
static void at(int col, int row, UINTN attr, const char *f, ...) {
  static char line[200]; static CHAR16 w[200];
  va_list ap; va_start(ap, f);
  UINTN n = fmt(line, sizeof line, f, ap);
  va_end(ap);
  for (UINTN i = 0; i <= n; i++) w[i] = (CHAR16)(UINT8)line[i];
  ST->ConOut->SetAttribute(ST->ConOut, attr);
  ST->ConOut->SetCursorPosition(ST->ConOut, (UINTN)col, (UINTN)row);
  ST->ConOut->OutputString(ST->ConOut, w);
}
#define C_DIM EFI_LIGHTGRAY
#define C_TXT EFI_LIGHTGRAY
#define C_HEAD EFI_LIGHTCYAN
#define C_OK EFI_LIGHTGREEN
#define C_WARN EFI_YELLOW
#define C_BAD EFI_LIGHTRED
#define MAP_ROW 10

static void draw_cell(int c) {
  CHAR16 g[2] = {0, 0}; UINTN attr;
  if (cell_hits[c]) { g[0] = cell_hits[c] > 9 ? L'X' : (CHAR16)(L'0' + cell_hits[c]); attr = EFI_WHITE | EFI_BACKGROUND_RED; }
  else if (cell_state[c] == 2) { g[0] = 0x2588; attr = C_OK; }      // full block
  else if (cell_state[c] == 1) { g[0] = 0x2592; attr = EFI_CYAN; }  // medium shade
  else { g[0] = 0x2591; attr = C_DIM; }                             // light shade
  ST->ConOut->SetAttribute(ST->ConOut, attr);
  ST->ConOut->SetCursorPosition(ST->ConOut, (UINTN)(8 + c % CELLCOLS), (UINTN)(MAP_ROW + c / CELLCOLS));
  ST->ConOut->OutputString(ST->ConOut, g);
}

static void draw_status(void) {
  UINT64 t = secs(); int temp = cpu_temp();
  at(2, 6, C_TXT, "PASS %3d   pattern %s        ", (UINT64)ui_pass, ui_pat);
  at(34, 6, C_WARN, "%s %d/%d   ", ui_phase, (UINT64)ui_read, (UINT64)ui_reads);
  at(52, 6, C_TXT, "t=%4d/%ds  ", t, ui_run_s);
  if (temp) at(68, 6, C_HEAD, "CPU %dC  ", (UINT64)temp);
  char bar[61]; UINT64 f = ui_run_s ? t * 60 / ui_run_s : 0; if (f > 60) f = 60;
  for (int i = 0; i < 60; i++) bar[i] = (UINT64)i < f ? '=' : (UINT64)i == f ? '>' : '.';
  bar[60] = 0;
  at(2, 7, C_DIM, "time [");
  at(8, 7, C_HEAD, "%s", bar);
  at(68, 7, C_DIM, "]");
}

// Data lines that saw hits, most frequent first: "DQ38:22  DQ32:2  ..."
static UINTN dq_ranking(char *o, UINTN cap) {
  UINT8 done[64] = {0}; UINTN n = 0;
  for (;;) {
    int best = -1;
    for (int b = 0; b < 64; b++) if (hist_dq[b] && !done[b] && (best < 0 || hist_dq[b] > hist_dq[best])) best = b;
    if (best < 0 || n + 16 >= cap) break;
    done[best] = 1;
    n += sfmt(o + n, cap - n, "DQ%d:%d  ", (UINT64)best, hist_dq[best]);
  }
  o[n] = 0;
  return n;
}

static void draw_hits(void) {
  at(2, 15, C_TXT, "HITS ");
  at(7, 15, hits_total ? C_BAD : C_OK, "total %d   transient %d   persistent %d      ", hits_total, hits_transient, hits_persistent);
  char l[68]; UINTN n = dq_ranking(l, sizeof l);
  at(2, 16, C_DIM, "lines     ");
  at(12, 16, C_WARN, "%s", n ? l : "-");
  at(2, 17, C_DIM, "beat      ");
  at(12, 17, C_WARN, "0:%d 1:%d 2:%d 3:%d 4:%d 5:%d 6:%d 7:%d   ", hist_beat[0], hist_beat[1], hist_beat[2], hist_beat[3],
     hist_beat[4], hist_beat[5], hist_beat[6], hist_beat[7]);
  for (int i = 0; i < 5; i++) at(2, 19 + i, i == 0 ? C_BAD : C_DIM, "%s", lasthit[i]);
}

static void draw_frame(const char *started, const char *cfg) {
  ST->ConOut->EnableCursor(ST->ConOut, FALSE);
  ST->ConOut->SetAttribute(ST->ConOut, C_TXT);
  ST->ConOut->ClearScreen(ST->ConOut);
  at(1, 0, C_HEAD, " ____   ___  _____  ___");
  at(1, 1, C_HEAD, "|  _ \\ / _ \\|___ / ( _ )");
  at(1, 2, C_HEAD, "| | | | | | | |_ \\ / _ \\");
  at(1, 3, C_HEAD, "| |_| | |_| |___) | (_) |");
  at(1, 4, C_HEAD, "|____/ \\__\\_\\____/ \\___/");
  at(30, 0, C_TXT, VERSION "  bare-metal RAM data-line test");
  at(30, 1, C_DIM, "%s", board[0] ? board : "unknown board");
  at(30, 2, C_DIM, "%s", cpuname);
  at(30, 3, C_DIM, "start %s   DIMMs %d", started, (UINT64)ndimm);
  at(30, 4, C_DIM, "%s", cfg);
  at(30, 5, C_OK, "positive control ok");
  at(2, 9, C_TXT, "MEMORY %d MiB in %d regions, 1 cell = %d MiB", total_bytes >> 20, (UINT64)nreg, cell_bytes >> 20);
  at(2, MAP_ROW + 4, C_DIM, "legend:");
  ST->ConOut->SetAttribute(ST->ConOut, C_DIM); ST->ConOut->OutputString(ST->ConOut, L" \x2591 untested ");
  ST->ConOut->SetAttribute(ST->ConOut, EFI_CYAN); ST->ConOut->OutputString(ST->ConOut, L" \x2592 written ");
  ST->ConOut->SetAttribute(ST->ConOut, C_OK); ST->ConOut->OutputString(ST->ConOut, L" \x2588 verified ");
  ST->ConOut->SetAttribute(ST->ConOut, EFI_WHITE | EFI_BACKGROUND_RED); ST->ConOut->OutputString(ST->ConOut, L" n ");
  ST->ConOut->SetAttribute(ST->ConOut, C_DIM); ST->ConOut->OutputString(ST->ConOut, L" hits in cell   any key = stop");
  for (int c = 0; c < ncells; c++) draw_cell(c);
  draw_status(); draw_hits();
}

// --- files on the boot volume ---
static EFI_FILE_HANDLE open_root(void) {
  EFI_LOADED_IMAGE *li; EFI_FILE_IO_INTERFACE *fs; EFI_FILE_HANDLE root;
  if (EFI_ERROR(BS->HandleProtocol(IMG, &gLI, (void **)&li))) return NULL;
  if (EFI_ERROR(BS->HandleProtocol(li->DeviceHandle, &gFS, (void **)&fs))) return NULL;
  if (EFI_ERROR(fs->OpenVolume(fs, &root))) return NULL;
  return root;
}

// Append everything not yet written to \dq38\dq38.log (creating \dq38 if needed).
static void flush_log(void) {
  if (loglen == logflushed) return;
  EFI_FILE_HANDLE root = open_root(), f, dir;
  if (!root) { emit(0, "LOG: no file system\n"); return; }
  if (!EFI_ERROR(root->Open(root, &dir, L"\\dq38", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                            EFI_FILE_DIRECTORY)))
    dir->Close(dir);
  EFI_STATUS s = root->Open(root, &f, L"\\dq38\\dq38.log",
                            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR(s)) { emit(0, "LOG: open failed %x (read-only medium?)\n", (UINT64)s); root->Close(root); return; }
  f->SetPosition(f, 0xFFFFFFFFFFFFFFFFULL);
  UINTN n = loglen - logflushed;
  s = f->Write(f, &n, logbuf + logflushed);
  if (EFI_ERROR(s)) emit(0, "LOG: write failed %x\n", (UINT64)s); else logflushed = loglen;
  f->Close(f); root->Close(root);
}

static UINTN read_cfg(char *buf, UINTN cap) {
  EFI_FILE_HANDLE root = open_root(), f; UINTN n = 0;
  if (!root) return 0;
  if (!EFI_ERROR(root->Open(root, &f, L"\\dq38\\run.cfg", EFI_FILE_MODE_READ, 0))) {
    n = cap - 1; if (EFI_ERROR(f->Read(f, &n, buf))) n = 0; f->Close(f);
  }
  root->Close(root); buf[n] = 0; return n;
}
// Value of a "KEY=123" or "KEY=0x7b" line; def if the key is absent. Other lines (comments) are ignored.
static UINT64 cfg_num(const char *cfg, const char *key, UINT64 def) {
  for (const char *p = cfg; *p; p++) {
    const char *a = p, *k = key;
    while (*k && *a == *k) { a++; k++; }
    if (!*k && *a == '=' && (p == cfg || p[-1] == '\n')) {
      UINT64 v = 0; a++;
      if (a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) {
        for (a += 2;; a++) {
          int d = *a >= '0' && *a <= '9' ? *a - '0' : *a >= 'a' && *a <= 'f' ? *a - 'a' + 10 : *a >= 'A' && *a <= 'F' ? *a - 'A' + 10 : -1;
          if (d < 0) break;
          v = v * 16 + (UINT64)d;
        }
      } else while (*a >= '0' && *a <= '9') v = v * 10 + (UINT64)(*a++ - '0');
      return v;
    }
  }
  return def;
}

// --- the test ---
// Expected value of the 64-bit word at addr for a pattern.
// 0: all zero, 1: all one, 2: 8 bytes 00 / 8 bytes FF alternating (every line toggles each beat), 3: inverse of 2.
static inline UINT64 pat(int p, UINT64 addr) {
  switch (p) {
  case 0: return 0;
  case 1: return ~0ULL;
  case 2: return (addr >> 3) & 1 ? ~0ULL : 0;
  default: return (addr >> 3) & 1 ? 0 : ~0ULL;
  }
}
static const char *patname[] = {"zero", "ones", "alt", "alt-inv"};

static void fill(UINT64 base, UINT64 bytes, int p) {
  volatile UINT64 *q = (volatile UINT64 *)base, *e = (volatile UINT64 *)(base + bytes);
  for (; q < e; q++) *q = pat(p, (UINT64)q);
}

// Positive control: one injected flip per byte lane, each on a different bit and in a different beat.
#define PC_N 8
static UINT64 pc_addr[PC_N], pc_diff[PC_N]; static int pc_seen[PC_N], pc_armed;

static void record(int pass, int rd, volatile UINT64 *q, UINT64 exp, UINT64 act) {
  UINT64 cached = *q;                       // second read, normally served from the cache
  __asm__ volatile("clflush (%0)\n\tmfence" : : "r"(q) : "memory");
  UINT64 dram = *q;                         // forced re-read from DRAM
  UINT64 diff = act ^ exp, a = (UINT64)q;
  int beat = (int)((a >> 3) & 7);
  hits_total++;
  if (dram == exp) hits_transient++; else hits_persistent++;
  for (int b = 0; b < 64; b++) if (diff >> b & 1) hist_dq[b]++;
  for (int b = 0; b < 64; b++) if (a >> b & 1) hist_abit[b]++;
  hist_beat[beat]++;
  if (pc_armed) for (int i = 0; i < PC_N; i++) if (a == pc_addr[i] && diff == pc_diff[i]) pc_seen[i] = 1;
  int first = 0; while (!(diff >> first & 1)) first++;
  if (!quiet && hit_lines < MAX_HIT_LINES) {
    hit_lines++;
    emit(1, "HIT pass=%d read=%d t=%ds phys=0x%x exp=%16x act=%16x cached=%16x dram=%16x dq=%d lane=%d bit=%d beat=%d %s\n",
         (UINT64)pass, (UINT64)rd, secs(), a, exp, act, cached, dram, (UINT64)first, (UINT64)(first / 8),
         (UINT64)(first % 8), (UINT64)beat, dram == exp ? "transient" : "PERSISTENT");
  }
  if (tui) {
    cell_hits[cur_cell]++;
    for (int i = 4; i > 0; i--) memcpy(lasthit[i], lasthit[i - 1], 80);
    sfmt(lasthit[0], 80, "0x%10x exp %16x act %16x DQ%d %s   ", a, exp, act, (UINT64)first,
         dram == exp ? "transient" : "PERSIST");
    draw_hits();
  }
  if (dram != exp) *q = exp;                // repair so a stuck value is not counted again every read
}

static UINT64 check(UINT64 base, UINT64 bytes, int p, int pass, int rd) {
  volatile UINT64 *q = (volatile UINT64 *)base, *e = (volatile UINT64 *)(base + bytes);
  UINT64 before = hits_total;
  for (; q < e; q++) { UINT64 v = *q, x = pat(p, (UINT64)q); if (v != x) record(pass, rd, q, x, v); }
  return hits_total - before;
}

// Walk all regions cell by cell: mode 0 writes the pattern, mode 1 verifies it.
static void sweep(int mode, int p, int pass, int rd) {
  cur_cell = 0; UINT64 left = cell_bytes;
  for (int i = 0; i < nreg; i++) {
    UINT64 a = reg[i].base, rem = reg[i].bytes;
    while (rem) {
      UINT64 n = rem < left ? rem : left;
      if (mode && idle_on && idle_ms && left == cell_bytes) idle_usec(idle_ms * 1000);
      if (mode) check(a, n, p, pass, rd); else fill(a, n, p);
      a += n; rem -= n; left -= n;
      if (!left) {
        cell_state[cur_cell] = mode ? 2 : 1;
        if (tui) draw_cell(cur_cell);
        cur_cell++; left = cell_bytes;
        if (tui && !(cur_cell & 15)) draw_status();
      }
    }
  }
  if (left != cell_bytes && cur_cell < CELLS) { cell_state[cur_cell] = mode ? 2 : 1; if (tui) draw_cell(cur_cell); }
}

// The test must find deliberately flipped bits at the right address and attribute them to the right
// data line and beat: lane k gets bit k flipped (DQ0, DQ9, ... DQ63) in beat k of its own cache line.
static UINT64 pc_n[2], pc_extra;
static int positive_control(void) {
  EFI_PHYSICAL_ADDRESS a = 0; UINT64 bytes = 16u << 20;
  if (EFI_ERROR(BS->AllocatePages(AllocateAnyPages, EfiLoaderData, bytes >> 12, &a))) return 0;
  int ok = 1; quiet = 1; pc_extra = 0;
  for (int p = 0; p < 2; p++) {
    fill(a, bytes, p);
    for (int k = 0; k < PC_N; k++) {
      pc_addr[k] = a + ((UINT64)(k + 1) << 20) + 0x4300 + (UINT64)k * 8;   // cache-line aligned base + beat k
      pc_diff[k] = 1ULL << (k * 8 + k);
      pc_seen[k] = 0;
      *(volatile UINT8 *)(pc_addr[k] + (UINT64)k) ^= (UINT8)(1u << k);
    }
    UINT64 beat_before[8]; memcpy(beat_before, hist_beat, sizeof beat_before);
    pc_armed = 1;
    UINT64 n = check(a, bytes, p, 0, 0);
    pc_armed = 0;
    pc_n[p] = n;
    int found = 0;
    for (int k = 0; k < PC_N; k++) if (pc_seen[k] && hist_beat[k] > beat_before[k]) found++;
    if (found != PC_N) ok = 0;                    // every injected flip must be found ...
    pc_extra += n - (UINT64)found;                // ... real faults on top are reported, not fatal
  }
  BS->FreePages(a, bytes >> 12); quiet = 0;
  // control hits must not leak into the statistics of the real run
  hits_total = hits_transient = hits_persistent = hit_lines = 0;
  memset(hist_dq, 0, sizeof hist_dq); memset(hist_beat, 0, sizeof hist_beat); memset(hist_abit, 0, sizeof hist_abit);
  return ok;
}

static void claim_memory(void) {
  UINTN msz = 0, key, dsz; UINT32 dver; EFI_MEMORY_DESCRIPTOR *map = NULL;
  BS->GetMemoryMap(&msz, NULL, &key, &dsz, &dver);
  msz += 8192;
  if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, msz, (void **)&map))) return;
  if (EFI_ERROR(BS->GetMemoryMap(&msz, map, &key, &dsz, &dver))) return;
  UINTN n = msz / dsz; UINT64 biggest = 0;
  for (UINTN i = 0; i < n; i++) {
    EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * dsz);
    if (d->Type == EfiConventionalMemory && d->NumberOfPages > biggest) biggest = d->NumberOfPages;
  }
  for (UINTN i = 0; i < n && nreg < MAXREG; i++) {
    EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * dsz);
    if (d->Type != EfiConventionalMemory || d->PhysicalStart < 0x100000) continue;
    UINT64 pages = d->NumberOfPages;
    if (pages == biggest) { pages -= 32768; biggest = 0; }   // leave 128 MiB to the firmware
    if (pages < 4096) continue;                              // ignore fragments below 16 MiB
    EFI_PHYSICAL_ADDRESS a = d->PhysicalStart;
    if (EFI_ERROR(BS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &a))) continue;
    reg[nreg].base = a; reg[nreg].bytes = pages << 12; nreg++;
  }
}

static void summary(int pass) {
  emit(1, "SUMMARY passes=%d seconds=%d hits=%d transient=%d persistent=%d temp=%dC\n", (UINT64)pass, secs(),
       hits_total, hits_transient, hits_persistent, (UINT64)cpu_temp());
  for (int b = 0; b < 64; b++)
    if (hist_dq[b]) emit(1, "  DQ%d (byte lane %d, bit %d): %d\n", (UINT64)b, (UINT64)(b / 8), (UINT64)(b % 8), hist_dq[b]);
  for (int w = 0; w < 8; w++) if (hist_beat[w]) emit(1, "  beat %d: %d\n", (UINT64)w, hist_beat[w]);
  if (hits_total) {
    // How often each physical address bit was set among the hits. A bit that is always 0 or always 1
    // (where chance would give about half) points at one channel, rank, bank or column.
    emit(1, "  address bits set (of %d hits):", hits_total);
    for (int b = 3; b < 40; b++) emit(1, " %d:%d", (UINT64)b, hist_abit[b]);
    emit(1, "\n");
  }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  ST = st; BS = st->BootServices; RT = st->RuntimeServices; IMG = image;
  BS->SetWatchdogTimer(0, 0, 0, NULL);       // the firmware would reset us after 5 minutes
  ST->ConOut->ClearScreen(ST->ConOut);
  tsc0 = rdtsc(); BS->Stall(200000); tsc_hz = (rdtsc() - tsc0) * 5; tsc0 = rdtsc();
  BS->AllocatePool(EfiLoaderData, LOGCAP, (void **)&logbuf);
  cpu_identify();

  static char cfg[CFGCAP];
  read_cfg(cfg, sizeof cfg);
  UINT64 run_s = cfg_num(cfg, "SECONDS", 600), reads = cfg_num(cfg, "READS", 3);
  UINT64 patmask = cfg_num(cfg, "PATTERNS", 15), do_reset = cfg_num(cfg, "RESET", 0);
  UINT64 inject = cfg_num(cfg, "INJECT", NONE);
  idle_s = cfg_num(cfg, "IDLE", 0); idle_ms = cfg_num(cfg, "IDLE_MS", 0);
  mwait_hint = cfg_num(cfg, "MWAIT", 0x60);
  idle_alt = cfg_num(cfg, "ALT", 0); UINT64 park = cfg_num(cfg, "PARK", 1);
  { UINT32 a, b, c, d; cpuid(1, &a, &b, &c, &d); have_mwait = (c >> 3) & 1; if (cfg_num(cfg, "HLT", 0)) have_mwait = 0; }
  if (!reads) reads = 1;
  if (!(patmask & 15)) patmask = 15;

  EFI_TIME t; RT->GetTime(&t, NULL);
  emit(1, "\n=== " VERSION " start %d-%2d-%2dT%2d:%2d:%2d temp=%dC\n",
       (UINT64)t.Year, (UINT64)t.Month, (UINT64)t.Day, (UINT64)t.Hour, (UINT64)t.Minute, (UINT64)t.Second, (UINT64)cpu_temp());
  static char cfgline[120];
  sfmt(cfgline, sizeof cfgline, "SECONDS=%d READS=%d PATTERNS=%d RESET=%d", run_s, reads, patmask, do_reset);
  emit(1, "cpu: %s\ncfg: %s IDLE=%d IDLE_MS=%d ALT=%d PARK=%d MWAIT=0x%x HLT=%d", cpuname, cfgline, idle_s, idle_ms,
       idle_alt, park, mwait_hint, (UINT64)!have_mwait);
  if (inject != NONE) emit(1, " INJECT=%d", inject);
  emit(1, "\n");
  smbios_scan();
  if ((idle_s || idle_ms) && have_mwait && park) {
    EFI_STATUS pst = park_aps();
    emit(1, "idle: other cores parked=%d (status %x)\n", ap_parked, (UINT64)pst);
  }

  int pc_ok = positive_control();
  emit(1, "control: zero=%d ones=%d hits (8 injected each), extra real hits %d\n", pc_n[0], pc_n[1], pc_extra);
  if (!pc_ok) {
    emit(1, "POSITIVE CONTROL FAILED - the test cannot be trusted on this machine, not running\n");
    flush_log(); BS->Stall(10000000); goto out;
  }
  emit(1, "positive control ok\n");

  claim_memory();
  UINT64 total = 0;
  for (int i = 0; i < nreg; i++) { total += reg[i].bytes; emit(1, "region %d: 0x%x + %d MiB\n", (UINT64)i, reg[i].base, reg[i].bytes >> 20); }
  emit(1, "testing %d MiB in %d regions\n", total >> 20, (UINT64)nreg);
  if (!nreg) { emit(1, "no memory could be claimed, not running\n"); flush_log(); BS->Stall(10000000); goto out; }
  flush_log();                               // "started" is on disk before the long part
  total_bytes = total; ui_run_s = run_s; ui_reads = (int)reads;
  cell_bytes = ((total / CELLS) + (1u << 20)) & ~((1ULL << 20) - 1);
  ncells = (int)((total + cell_bytes - 1) / cell_bytes); if (ncells > CELLS) ncells = CELLS;
  static char started[40];
  sfmt(started, sizeof started, "%d-%2d-%2d %2d:%2d:%2d", (UINT64)t.Year, (UINT64)t.Month, (UINT64)t.Day, (UINT64)t.Hour, (UINT64)t.Minute, (UINT64)t.Second);
  tui = 1; draw_frame(started, cfgline);

  int pass = 0, stop = 0;
  while (!stop && secs() < run_s) {
    for (int p = 0; p < 4 && !stop; p++) {
      if (!(patmask >> p & 1)) continue;
      pass++;
      UINT64 h0 = hits_total, tr0 = hits_transient;
      ui_pass = pass; ui_pat = patname[p]; ui_phase = "WRITE "; ui_read = 0;
      for (int c = 0; c < ncells; c++) { cell_state[c] = 0; draw_cell(c); }
      draw_status();
      sweep(0, p, pass, 0);
      if (inject < 64 && pass == 1)            // self-test: fake one stored fault on the given data line
        *(volatile UINT8 *)(reg[0].base + 0x200000 + inject / 8) ^= (UINT8)(1u << (inject % 8));
      idle_on = !idle_alt || (pass & 1);        // ALT=1: odd passes with idle, even passes without
      UINT64 w0 = idle_wakes;
      for (int r = 1; r <= (int)reads; r++) {
        if (idle_s && idle_on) { ui_phase = "IDLE  "; ui_read = r; draw_status(); idle_usec(idle_s * 1000000); }
        ui_phase = "VERIFY"; ui_read = r; draw_status(); sweep(1, p, pass, r);
      }
      if (idle_s || idle_ms) emit(1, "  idle: on=%d wakes=%d\n", (UINT64)idle_on, idle_wakes - w0);
      emit(1, "PASS %d pattern=%s t=%ds temp=%dC hits=%d (transient %d) total=%d\n", (UINT64)pass, patname[p], secs(),
           (UINT64)cpu_temp(), hits_total - h0, hits_transient - tr0, hits_total);
      EFI_INPUT_KEY k;
      if (!EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &k))) stop = 1;   // any key ends the run
      if (secs() >= run_s) stop = 1;
    }
  }

  ui_phase = "DONE  "; draw_status();
  summary(pass);
  emit(1, "=== end\n");
  for (int i = 0; i < nreg; i++) BS->FreePages(reg[i].base, reg[i].bytes >> 12);
  flush_log();
  if (!do_reset) {
    at(2, 24, C_OK, "finished - log written, press any key to return to the firmware");
    EFI_INPUT_KEY k; UINTN idx;
    while (!EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &k))) {}   // drop the key that stopped the run
    BS->WaitForEvent(1, &ST->ConIn->WaitForKey, &idx);
    ST->ConOut->SetAttribute(ST->ConOut, C_TXT); ST->ConOut->ClearScreen(ST->ConOut);
    return EFI_SUCCESS;
  }
  at(2, 24, C_OK, "finished - log written, rebooting in 5 s");
out:
  BS->Stall(5000000);
  if (do_reset) RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
  return EFI_SUCCESS;
}
