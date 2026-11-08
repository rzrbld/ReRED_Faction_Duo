/*
 * rered-import - builds rered_faction2/data from the user's own Red Faction II.
 *
 *   rered-import --source DIR --dest DIR [--innoextract PATH] [--7z PATH]
 *
 * Main path: the GOG.com installer (setup_red_faction2_*.exe, Inno Setup) in
 * source/, unpacked with PortMaster's innoextract (its "app" folder is the
 * installed game). The translated code in this port was built from that
 * release's rf2.exe (sha256 below), so the result is checked against it.
 *
 * Fallback: the two retail discs (.cue + .bin / .iso) plus an rf2.exe with
 * the same hash, loose or in a .zip/.rar/.7z (PortMaster's 7zzs).
 *
 * The game's display and detail settings (rf2_config.ini) are set to the
 * original release's defaults at 640x480 (GOG ships 1080p / maximum detail).
 * Nothing in source/ is modified.
 *
 * Disc 1 carries the InstallShield 6 cabinet set's header and first volumes
 * (data1.hdr, data1.cab, data2.cab) and the loose movies and music
 * (pc_media/Movies, pc_media/Music); disc 2 carries data3.cab. The cabinets
 * are copied to dest/.import-tmp, their files are inflated into dest/, and the
 * temporary copies are removed. One file (All_Levels.packfile, 661 MB) starts
 * in data2.cab and continues in data3.cab.
 *
 * InstallShield 6 format as read here (the same as tools/assets/isextract.py):
 *   data1.hdr: common header { "ISc(", version, volume info, cab descriptor
 *   offset, size }; the descriptor holds the file table (directory name
 *   offsets, then 0x57-byte file records: u16 flags, u64 expanded size, u64
 *   compressed size, u64 data offset, md5[16], u32 name offset, u16 directory,
 *   ..., u16 volume at +0x55).
 *   A file's data, in volume dataN.cab at its offset: chunks of
 *   { u16 size, raw deflate }, the raw stream optionally scrambled (flag 2):
 *   b' = ror8(b ^ 0xD5, 2) - (seed % 0x47), the seed counting every byte.
 *   A file that runs past its volume continues in the next one at that
 *   volume's first-file offset (volume header +0x24).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <zlib.h>

/* SHA-256 of rf2.exe (GOG release 2.0.0.6), the executable the translated
 * code was generated from */
#define SHA_RF2 "7f0d187a246c9c8bcc1693b6c0dce3e87dd1585c9bb6fa691b85cee2b2159bef"

static const char *g_src, *g_dst, *g_7z, *g_inno;
static char g_tmp[4096];

static void die(const char *fmt, const char *a) { fprintf(stderr, "[import] ERROR: "); fprintf(stderr, fmt, a); fprintf(stderr, "\n"); exit(1); }
static void info(const char *fmt, const char *a) { printf("[import] "); printf(fmt, a); printf("\n"); fflush(stdout); }

/* ------------------------------------------------------------- sha256 */
typedef struct { uint32_t h[8]; uint64_t n; uint8_t b[64]; size_t bl; } sha256;
static const uint32_t K[64] = {
 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
 0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
 0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
 0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(sha256 *s, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 | (uint32_t)p[i*4+2] << 8 | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha_init(sha256 *s) {
    static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(s->h, iv, sizeof iv); s->n = 0; s->bl = 0;
}
static void sha_update(sha256 *s, const uint8_t *p, size_t n) {
    s->n += n;
    while (n) {
        size_t k = 64 - s->bl < n ? 64 - s->bl : n;
        memcpy(s->b + s->bl, p, k); s->bl += k; p += k; n -= k;
        if (s->bl == 64) { sha_block(s, s->b); s->bl = 0; }
    }
}
static void sha_hex(sha256 *s, char out[65]) {
    uint64_t bits = s->n * 8; uint8_t pad = 0x80, z = 0;
    sha_update(s, &pad, 1);
    while (s->bl != 56) sha_update(s, &z, 1);
    uint8_t len[8]; for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, len, 8);
    for (int i = 0; i < 8; i++) sprintf(out + i * 8, "%08x", s->h[i]);
}
static int file_sha(const char *path, char out[65]) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    sha256 s; sha_init(&s); static uint8_t buf[1 << 16]; size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) sha_update(&s, buf, r);
    fclose(f); sha_hex(&s, out); return 0;
}

/* ---------------------------------------------------------- utilities */
static int ends_with(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcasecmp(s + a - b, suf); }
static void mkdirs(const char *path) {
    char p[4096]; snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}
static void mkparent(const char *file) {
    char p[4096]; snprintf(p, sizeof p, "%s", file);
    char *s = strrchr(p, '/'); if (s) { *s = 0; mkdirs(p); }
}
static void rmtree(const char *dir) {
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[4200]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st; if (!lstat(p, &st) && S_ISDIR(st.st_mode)) rmtree(p); else unlink(p);
    }
    closedir(d); rmdir(dir);
}

/* ------------------------------------------------------- disc images */
typedef struct { FILE *f; int raw; } disc;
static int disc_read(disc *d, uint32_t lba, uint8_t *out, uint32_t nsect) {
    for (uint32_t i = 0; i < nsect; i++) {
        long off = (long)(lba + i) * (d->raw ? 2352 : 2048) + (d->raw ? 16 : 0);
        if (fseek(d->f, off, SEEK_SET) || fread(out + (size_t)i * 2048, 1, 2048, d->f) != 2048) return -1;
    }
    return 0;
}
static int g_joliet;
static void name_of(const uint8_t *rec, char *out, size_t n) {
    int nl = rec[32]; const uint8_t *p = rec + 33; size_t k = 0;
    if (g_joliet) for (int i = 0; i + 1 < nl && k + 1 < n; i += 2) { uint16_t c = (uint16_t)(p[i] << 8 | p[i + 1]); out[k++] = c < 128 ? (char)c : '_'; }
    else for (int i = 0; i < nl && k + 1 < n; i++) out[k++] = (char)p[i];
    out[k] = 0;
    char *semi = strchr(out, ';'); if (semi) *semi = 0;
}
static int disc_open(disc *d, const char *image, uint32_t *root_lba, uint32_t *root_size) {
    d->f = fopen(image, "rb"); d->raw = 0;
    if (!d->f) return -1;
    uint8_t sync[12] = {0}, ref[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (fread(sync, 1, 12, d->f) == 12 && !memcmp(sync, ref, 12)) d->raw = 1;
    uint8_t vd[2048], root[34]; g_joliet = 0;
    if (disc_read(d, 16, vd, 1) || memcmp(vd + 1, "CD001", 5)) { fclose(d->f); return -1; }
    memcpy(root, vd + 156, 34);
    for (uint32_t l = 17; l < 32; l++) {           /* prefer Joliet: long names */
        if (disc_read(d, l, vd, 1) || vd[0] == 255) break;
        if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/') { memcpy(root, vd + 156, 34); g_joliet = 1; break; }
    }
    *root_lba = (uint32_t)root[2] | (uint32_t)root[3] << 8 | (uint32_t)root[4] << 16 | (uint32_t)root[5] << 24;
    *root_size = (uint32_t)root[10] | (uint32_t)root[11] << 8 | (uint32_t)root[12] << 16 | (uint32_t)root[13] << 24;
    return 0;
}
static int copy_extent(disc *d, uint32_t lba, uint32_t size, const char *dst, const char *label) {
    struct stat st;
    if (!stat(dst, &st) && (uint32_t)st.st_size == size) return 0;   /* resume */
    mkparent(dst);
    char tmp[4200]; snprintf(tmp, sizeof tmp, "%s.part", dst);
    FILE *o = fopen(tmp, "wb"); if (!o) return -1;
    printf("[import]   %s (%u MB)\n", label, size >> 20); fflush(stdout);
    static uint8_t chunk[2048 * 256];
    for (uint32_t left = size, l = lba; left; ) {
        uint32_t n = left > sizeof chunk ? (uint32_t)sizeof chunk : left, s = (n + 2047) / 2048;
        if (disc_read(d, l, chunk, s) || fwrite(chunk, 1, n, o) != n) { fclose(o); unlink(tmp); return -1; }
        left -= n; l += s;
    }
    if (fclose(o) || rename(tmp, dst)) return -1;
    return 0;
}
/* Walk a directory. Root-level files named in `want` go to g_tmp; everything
 * under pc_media/ goes to g_dst (the loose movies and music). */
static int g_found_hdr, g_found_cab3;
static int walk(disc *d, uint32_t lba, uint32_t size, const char *rel) {
    uint32_t ns = (size + 2047) / 2048;
    uint8_t *buf = malloc((size_t)ns * 2048);
    if (!buf || disc_read(d, lba, buf, ns)) { free(buf); return -1; }
    for (uint32_t pos = 0; pos < size; ) {
        uint8_t len = buf[pos];
        if (!len) { pos = (pos / 2048 + 1) * 2048; continue; }
        const uint8_t *r = buf + pos; pos += len;
        if (r[32] == 1 && (r[33] == 0 || r[33] == 1)) continue;
        char name[256]; name_of(r, name, sizeof name);
        uint32_t elba = (uint32_t)r[2] | (uint32_t)r[3] << 8 | (uint32_t)r[4] << 16 | (uint32_t)r[5] << 24;
        uint32_t esz  = (uint32_t)r[10] | (uint32_t)r[11] << 8 | (uint32_t)r[12] << 16 | (uint32_t)r[13] << 24;
        char sub[2048]; snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", name);
        if (r[25] & 2) {
            if (*rel || !strcasecmp(name, "pc_media"))
                if (walk(d, elba, esz, sub)) { free(buf); return -1; }
            continue;
        }
        char dst[4200];
        if (!*rel) {
            static const char *want[] = { "data1.hdr", "data1.cab", "data2.cab", "data3.cab" };
            for (int i = 0; i < 4; i++) if (!strcasecmp(name, want[i])) {
                snprintf(dst, sizeof dst, "%s/%s", g_tmp, want[i]);
                if (copy_extent(d, elba, esz, dst, want[i])) { free(buf); return -1; }
                if (i == 0) g_found_hdr = 1;
                if (i == 3) g_found_cab3 = 1;
            }
            continue;
        }
        snprintf(dst, sizeof dst, "%s/%s", g_dst, sub);
        if (copy_extent(d, elba, esz, dst, sub)) { free(buf); return -1; }
    }
    free(buf);
    return 0;
}

/* --------------------------------------------- InstallShield 6 cabinets */
static uint8_t *g_hdr; static size_t g_hdr_n;
static uint32_t rd32(size_t o) { return o + 4 <= g_hdr_n ? (uint32_t)g_hdr[o] | (uint32_t)g_hdr[o+1] << 8 | (uint32_t)g_hdr[o+2] << 16 | (uint32_t)g_hdr[o+3] << 24 : 0; }
static uint16_t rd16(size_t o) { return o + 2 <= g_hdr_n ? (uint16_t)(g_hdr[o] | g_hdr[o+1] << 8) : 0; }
static uint64_t rd64(size_t o) { return (uint64_t)rd32(o) | (uint64_t)rd32(o + 4) << 32; }
static const char *hdr_str(size_t o) { return o < g_hdr_n && memchr(g_hdr + o, 0, g_hdr_n - o) ? (const char *)g_hdr + o : ""; }

typedef struct { FILE *f; int vol; uint32_t seed; int obf; } cabread;
static FILE *open_volume(int vol) {
    char p[4200]; snprintf(p, sizeof p, "%s/data%d.cab", g_tmp, vol);
    return fopen(p, "rb");
}
static size_t cab_read(cabread *c, uint8_t *out, size_t n) {
    size_t got = 0;
    while (got < n) {
        size_t r = fread(out + got, 1, n - got, c->f);
        got += r;
        if (got < n) {                               /* continue in the next volume */
            FILE *nx = open_volume(c->vol + 1);
            if (!nx) break;
            uint8_t h[0x28];
            if (fread(h, 1, sizeof h, nx) != sizeof h) { fclose(nx); break; }
            uint32_t first = (uint32_t)h[0x24] | (uint32_t)h[0x25] << 8 | (uint32_t)h[0x26] << 16 | (uint32_t)h[0x27] << 24;
            fseek(nx, first, SEEK_SET);
            fclose(c->f); c->f = nx; c->vol++;
        }
    }
    if (c->obf) for (size_t i = 0; i < got; i++) {
        uint8_t x = out[i] ^ 0xD5;
        x = (uint8_t)((x >> 2) | (x << 6));
        out[i] = (uint8_t)(x - (c->seed % 0x47)); c->seed++;
    }
    return got;
}
static const char *k_skip[] = {        /* the installer's own files, not the game's */
    "iKernel.dll", "Setup.dll", "DotNetInstaller.exe", "iscript.dll", "ctor.dll", "iuser.dll", "IGDI.dll", "IsProBE.tlb",
    "objectps.dll", "isrt.dll", "_IsRes.dll", "setup.inx", "corecomp.ini", "vssver.scc", "II.exe", "setup.exe", "setup.ini",
    "setup.skin", "setup.boot", "setup.bmp", "layout.bin", "Autorun.exe", "AutoRun.exe", "Autorun.inf", "Autorun.ico", NULL };
static int skip_file(const char *dir, const char *name) {
    if (!strncasecmp(dir, "DirectX81", 9)) return 1;
    if (!strncasecmp(name, "InstallDialog", 13) || !strncasecmp(name, "StringTable-", 12)) return 1;
    for (int i = 0; k_skip[i]; i++) if (!strcasecmp(name, k_skip[i])) return 1;
    return 0;
}
static int extract_cabs(void) {
    char p[4200]; snprintf(p, sizeof p, "%s/data1.hdr", g_tmp);
    FILE *f = fopen(p, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); g_hdr_n = (size_t)ftell(f); rewind(f);
    g_hdr = malloc(g_hdr_n);
    if (fread(g_hdr, 1, g_hdr_n, f) != g_hdr_n) { fclose(f); return -1; }
    fclose(f);
    if (rd32(0) != 0x28635349u) die("%s is not an InstallShield header", "data1.hdr");
    uint32_t major = (rd32(4) >> 12) & 0xF;
    if (major < 6) die("unexpected InstallShield version (%s)", "older than 6");
    size_t cd = rd32(12);
    uint32_t ft_off = rd32(cd + 0x0C), dir_n = rd32(cd + 0x1C), file_n = rd32(cd + 0x28), ft_off2 = rd32(cd + 0x2C);
    size_t ft = cd + ft_off;
    printf("[import] cabinet: %u files in %u directories\n", file_n, dir_n); fflush(stdout);
    static uint8_t in[65536 + 4], out[1 << 17];
    uint32_t done = 0, total = 0;
    for (uint32_t i = 0; i < file_n; i++) { size_t fd = ft + ft_off2 + (size_t)i * 0x57; if (rd16(fd) & 4) total++; }
    for (uint32_t i = 0; i < file_n; i++) {
        size_t fd = ft + ft_off2 + (size_t)i * 0x57;
        uint16_t flags = rd16(fd);
        if ((flags & 8) || !(flags & 4)) continue;          /* invalid, or a loose file (taken from the disc) */
        uint64_t size = rd64(fd + 2), off = rd64(fd + 0x12);
        const char *name = hdr_str(ft + rd32(fd + 0x3A));
        uint16_t di = rd16(fd + 0x3E), vol = rd16(fd + 0x55);
        const char *dir = di < dir_n ? hdr_str(ft + rd32(ft + (size_t)di * 4)) : "";
        done++;
        if (!*name || skip_file(dir, name)) continue;
        char rel[2048]; snprintf(rel, sizeof rel, "%s%s%s", dir, *dir ? "/" : "", name);
        for (char *s = rel; *s; s++) if (*s == '\\') *s = '/';
        char dst[4200]; snprintf(dst, sizeof dst, "%s/%s", g_dst, rel);
        struct stat st;
        if (strcasecmp(name, "paths.ini") && !stat(dst, &st) && (uint64_t)st.st_size == size) continue;   /* resume */
        if (size >= (8u << 20) || done % 200 == 0) {
            printf("[import] %u/%u  %s (%llu MB)\n", done, total, rel, (unsigned long long)(size >> 20)); fflush(stdout);
        }
        cabread c = { open_volume(vol ? vol : 1), vol ? vol : 1, 0, (flags & 2) != 0 };
        if (!c.f) die("cannot open cabinet volume for %s", rel);
        fseeko(c.f, (off_t)off, SEEK_SET);
        mkparent(dst);
        char tmp[4300]; snprintf(tmp, sizeof tmp, "%s.part", dst);
        FILE *o = fopen(tmp, "wb"); if (!o) die("cannot write %s", dst);
        uint64_t written = 0;
        while (written < size) {
            uint8_t sz[2];
            if (cab_read(&c, sz, 2) != 2) die("unexpected end of cabinet in %s", rel);
            uint32_t n = (uint32_t)(sz[0] | sz[1] << 8);
            if (!n) break;
            if (cab_read(&c, in, n) != n) die("unexpected end of cabinet in %s", rel);
            z_stream z; memset(&z, 0, sizeof z);
            if (inflateInit2(&z, -15) != Z_OK) die("zlib: %s", "init");
            z.next_in = in; z.avail_in = n;
            int zr;
            do {
                z.next_out = out; z.avail_out = sizeof out;
                zr = inflate(&z, Z_SYNC_FLUSH);
                size_t produced = sizeof out - z.avail_out;
                if (produced && fwrite(out, 1, produced, o) != produced) die("disk full writing %s", rel);
                written += produced;
            } while (zr == Z_OK && z.avail_in);
            inflateEnd(&z);
            if (zr != Z_OK && zr != Z_STREAM_END && zr != Z_BUF_ERROR) die("corrupt data in %s", rel);
        }
        fclose(c.f);
        if (fclose(o) || written != size) die("short output for %s", rel);
        if (!strcasecmp(name, "paths.ini")) {
            /* three variants by install type (music/movies on CD or disk); the
             * import puts both on the card, so keep the "on_hd" one */
            FILE *pf = fopen(tmp, "rb"); char txt[512] = {0};
            if (pf) { size_t r = fread(txt, 1, sizeof txt - 1, pf); txt[r] = 0; fclose(pf); }
            if (!strstr(txt, "Music_on_hd")) { unlink(tmp); continue; }
        }
        if (rename(tmp, dst)) die("cannot finish %s", rel);
    }
    free(g_hdr);
    return 0;
}

/* ------------------------------------------- the unprotected rf2.exe */
#include <sys/wait.h>
static int find_exe(const char *dir, char *out, size_t n, int depth) {
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e; int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[4200]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st; if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 5) found = find_exe(p, out, n, depth + 1); continue; }
        if (strcasecmp(e->d_name, "rf2.exe")) continue;
        char sha[65];
        if (!file_sha(p, sha) && !strcmp(sha, SHA_RF2)) { snprintf(out, n, "%s", p); found = 1; }
        else info("ignoring %s (not the expected no-CD build)", p);
    }
    closedir(d); return found;
}
static void unpack_archives(const char *dir, const char *into, int depth) {
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[4200]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st; if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 3) unpack_archives(p, into, depth + 1); continue; }
        if (!(ends_with(e->d_name, ".zip") || ends_with(e->d_name, ".rar") || ends_with(e->d_name, ".7z"))) continue;
        if (!g_7z) { info("cannot open %s: no 7zzs given", e->d_name); continue; }
        info("looking inside %s", e->d_name);
        char o[4300]; snprintf(o, sizeof o, "-o%s", into);
        /* only rf2.exe: the release archives also hold the disc images */
        char *av[] = { (char *)g_7z, "x", "-y", "-r", "-bso0", "-bsp0", o, p, "rf2.exe", NULL };
        pid_t pid = fork();
        if (pid == 0) { execv(av[0], av); _exit(127); }
        int stt; if (pid > 0) waitpid(pid, &stt, 0);
    }
    closedir(d);
}

/* ------------------------------------------------- GOG installer path */
static int find_gog_setup(char *out, size_t n) {
    DIR *d = opendir(g_src); if (!d) return 0;
    struct dirent *e; int found = 0;
    while (!found && (e = readdir(d)))
        if (!strncasecmp(e->d_name, "setup_red_faction", 17) && ends_with(e->d_name, ".exe")) { snprintf(out, n, "%s/%s", g_src, e->d_name); found = 1; }
    closedir(d); return found;
}
/* move every entry of from/ into to/ (same filesystem), merging directories */
static int move_tree(const char *from, const char *to) {
    DIR *d = opendir(from); if (!d) return -1;
    mkdirs(to);
    struct dirent *e; int rc = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[4200], b[4200]; snprintf(a, sizeof a, "%s/%s", from, e->d_name); snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        struct stat st; if (lstat(a, &st)) continue;
        if (S_ISDIR(st.st_mode)) { struct stat sb; if (!stat(b, &sb) && S_ISDIR(sb.st_mode)) { if (move_tree(a, b)) rc = -1; continue; } }
        if (rename(a, b)) { fprintf(stderr, "[import] cannot move %s: %s\n", e->d_name, strerror(errno)); rc = -1; }
    }
    closedir(d); return rc;
}
static void import_gog(const char *setup) {
    if (!g_inno) die("%s", "the GOG installer needs PortMaster's innoextract");
    char out[4200]; snprintf(out, sizeof out, "%s/gog", g_tmp); mkdirs(out);
    info("unpacking %s (several minutes)", setup);
    char *av[] = { (char *)g_inno, "-s", "-I", "app", "-d", out, (char *)setup, NULL };
    pid_t pid = fork();
    if (pid == 0) { execv(av[0], av); _exit(127); }
    int st = 0; if (pid < 0 || waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) die("%s", "innoextract failed (see log.txt)");
    char app[4300]; snprintf(app, sizeof app, "%s/app", out);
    if (move_tree(app, g_dst)) die("%s", "could not move the unpacked files into data/");
}
/* rf2_config.ini: display and detail back to the original defaults */
static void set_handheld_config(void) {
    static const char *keys[][2] = {
        { "Screen_width", "640" }, { "Screen_height", "480" }, { "Shadow_resolution", "256" },
        { "Mesh_lod_scale", "1.000000" }, { "VFX_setting", "\"effects.tbl\"" }, { "Corona_ms", "200" },
        { "Corona_range", "50.000000" }, { "Shadow_softness", "4" } };
    char p[4200], t[4300]; snprintf(p, sizeof p, "%s/rf2_config.ini", g_dst); snprintf(t, sizeof t, "%s.part", p);
    FILE *in = fopen(p, "rb"); if (!in) return;
    FILE *o = fopen(t, "wb"); if (!o) { fclose(in); return; }
    char line[1024];
    while (fgets(line, sizeof line, in)) {
        int done = 0;
        for (size_t k = 0; k < sizeof keys / sizeof *keys && !done; k++) {
            size_t n = strlen(keys[k][0]);
            if (!strncmp(line, keys[k][0], n) && (line[n] == ' ' || line[n] == '=' || line[n] == '\t')) {
                const char *c = strchr(line, ';');
                fprintf(o, "%s = %s%s%s", keys[k][0], keys[k][1], c ? "\t\t; " : "\r\n", c ? c + 1 : "");
                done = 1;
            }
        }
        if (!done) fputs(line, o);
    }
    fclose(in);
    if (!fclose(o)) rename(t, p);
}

/* ------------------------------------------------------------ main */
int main(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--source")) g_src = argv[i + 1];
        if (!strcmp(argv[i], "--dest")) g_dst = argv[i + 1];
        if (!strcmp(argv[i], "--7z")) g_7z = argv[i + 1];
        if (!strcmp(argv[i], "--innoextract")) g_inno = argv[i + 1];
    }
    if (!g_src || !g_dst) { fprintf(stderr, "usage: rered-import --source DIR --dest DIR\n"); return 2; }
    mkdirs(g_dst);
    snprintf(g_tmp, sizeof g_tmp, "%s/.import-tmp", g_dst);
    mkdirs(g_tmp);
    struct statvfs vs;
    if (!statvfs(g_dst, &vs)) {
        unsigned long long mb = (unsigned long long)vs.f_bavail * vs.f_frsize >> 20;
        printf("[import] free space: %llu MB (about 1500 MB needed)\n", mb); fflush(stdout);
    }
    char setup[4200];
    if (find_gog_setup(setup, sizeof setup)) {          /* main path: the GOG installer */
        import_gog(setup);
        rmtree(g_tmp);
        set_handheld_config();
        char exe[4200], sha[65]; snprintf(exe, sizeof exe, "%s/rf2.exe", g_dst);
        if (file_sha(exe, sha)) die("%s", "rf2.exe missing after the import");
        if (strcmp(sha, SHA_RF2)) die("rf2.exe is a different version (sha256 %s): this port needs the GOG release 2.0.0.6", sha);
        info("%s", "done: game files imported, rf2.exe verified");
        return 0;
    }
    /* 0. the unprotected executable, before the long part */
    char exe_src[4200];
    if (!find_exe(g_src, exe_src, sizeof exe_src, 0)) {
        char ex[4200]; snprintf(ex, sizeof ex, "%s/exe", g_tmp); mkdirs(ex);
        unpack_archives(g_src, ex, 0);
        if (!find_exe(ex, exe_src, sizeof exe_src, 0))
            die("%s", "no GOG installer (setup_red_faction2_*.exe) found in source/ (see README)");
    }
    info("unprotected rf2.exe: %s", exe_src);
    /* 1. the discs */
    DIR *d = opendir(g_src); if (!d) die("no source directory %s", g_src);
    struct dirent *e; int images = 0;
    while ((e = readdir(d))) {
        if (!(ends_with(e->d_name, ".bin") || ends_with(e->d_name, ".iso") || ends_with(e->d_name, ".img"))) continue;
        char p[4200]; snprintf(p, sizeof p, "%s/%s", g_src, e->d_name);
        disc dd; uint32_t lba, sz;
        if (disc_open(&dd, p, &lba, &sz)) { info("skipping %s (not an ISO9660 disc image)", e->d_name); continue; }
        info("reading %s", e->d_name);
        if (walk(&dd, lba, sz, "")) { fclose(dd.f); die("read error in %s", e->d_name); }
        fclose(dd.f); images++;
    }
    closedir(d);
    if (!images) die("no disc images (.bin/.iso) in %s", g_src);
    if (!g_found_hdr) die("%s", "disc 1 not found (data1.hdr)");
    if (!g_found_cab3) die("%s", "disc 2 not found (data3.cab)");
    /* 2. the cabinets */
    info("%s", "unpacking the game files");
    if (extract_cabs()) die("%s", "cabinet extraction failed");
    /* 3. the executable: the user's unprotected build replaces the disc's */
    char exe[4200], tmpx[4300], sha[65]; snprintf(exe, sizeof exe, "%s/rf2.exe", g_dst); snprintf(tmpx, sizeof tmpx, "%s.part", exe);
    {
        FILE *in = fopen(exe_src, "rb"), *o = in ? fopen(tmpx, "wb") : NULL;
        static uint8_t b[1 << 16]; size_t r;
        if (!o) die("%s", "cannot copy rf2.exe");
        while ((r = fread(b, 1, sizeof b, in)) > 0) if (fwrite(b, 1, r, o) != r) die("%s", "disk full");
        fclose(in); if (fclose(o) || rename(tmpx, exe)) die("%s", "cannot copy rf2.exe");
    }
    rmtree(g_tmp);
    set_handheld_config();
    if (file_sha(exe, sha) || strcmp(sha, SHA_RF2)) die("%s", "rf2.exe check failed after copying");
    info("%s", "done: game files imported, rf2.exe verified");
    return 0;
}
