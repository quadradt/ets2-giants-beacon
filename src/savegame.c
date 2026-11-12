/*
 * savegame.c – Spielstand von ETS2/ATS lesen und die Rundumleuchten des
 * aktuellen LKW bestimmen.
 *
 * Formate:
 *   ScsC  AES-256-CBC + zlib (fester, öffentlich bekannter Schlüssel), darin:
 *   BSII  binäres SII (Struktur-/Datenblöcke), Format nach DecryptTruck /
 *         ets2-decrypt (MIT) nachgebaut
 *   SiiN  Klartext-SII (g_save_format "2")
 *
 * Bewusst ohne neue glibc-Symbole (kein stat(), kein strtol unter C23,
 * dlopen mit alter Symbolversion), damit das Plugin auch in älteren
 * Steam-Runtimes lädt.
 */
#define _DEFAULT_SOURCE
#include "savegame.h"

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

__asm__(".symver dlopen,dlopen@GLIBC_2.2.5");
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");

#include "beacon_table.h"

/* ------------------------------------------------------------------ */
/* Hilfsfunktionen                                                     */
/* ------------------------------------------------------------------ */

/* stat() über den Syscall – das glibc-Symbol stat@GLIBC_2.33 wäre zu neu. */
static int file_mtime(const char *path, time_t *mt, long long *size)
{
	struct stat st;
	if (syscall(SYS_newfstatat, AT_FDCWD, path, &st, 0) != 0)
		return -1;
	if (mt)
		*mt = st.st_mtime;
	if (size)
		*size = st.st_size;
	return 0;
}

static uint64_t fnv(const void *p, size_t n, uint64_t h)
{
	const unsigned char *s = (const unsigned char *)p;
	for (size_t i = 0; i < n; i++)
		h = (h ^ s[i]) * 1099511628211ULL;
	return h;
}
#define FNV0 1469598103934665603ULL

static unsigned char *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	size_t cap = 1 << 20, n = 0;
	unsigned char *buf = (unsigned char *)malloc(cap);
	while (buf) {
		if (n == cap) {
			unsigned char *nb = (unsigned char *)realloc(buf, cap *= 2);
			if (!nb) { free(buf); buf = NULL; break; }
			buf = nb;
		}
		size_t r = fread(buf + n, 1, cap - n, f);
		if (r == 0)
			break;
		n += r;
	}
	fclose(f);
	*len = n;
	return buf;
}

/* ------------------------------------------------------------------ */
/* AES-256 (nur Entschlüsselung, CBC)                                  */
/* ------------------------------------------------------------------ */

static uint8_t sbox[256], rsbox[256];

static uint8_t rotl8(uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); }

static void aes_init_tables(void)
{
	if (sbox[0] == 0x63)
		return;
	uint8_t p = 1, q = 1;
	do {   /* Erzeugung über Generator 3 (siehe FIPS-197 / Wikipedia) */
		p = p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1B : 0);
		q ^= q << 1;
		q ^= q << 2;
		q ^= q << 4;
		if (q & 0x80)
			q ^= 0x09;
		uint8_t x = q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4);
		sbox[p] = x ^ 0x63;
	} while (p != 1);
	sbox[0] = 0x63;
	for (int i = 0; i < 256; i++)
		rsbox[sbox[i]] = (uint8_t)i;
}

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1B)); }

static uint8_t gmul(uint8_t a, uint8_t b)
{
	uint8_t r = 0;
	while (b) {
		if (b & 1)
			r ^= a;
		a = xtime(a);
		b >>= 1;
	}
	return r;
}

static void aes256_expand(const uint8_t key[32], uint8_t rk[240])
{
	memcpy(rk, key, 32);
	uint8_t rcon = 1;
	for (int i = 8; i < 60; i++) {
		uint8_t t[4];
		memcpy(t, rk + (i - 1) * 4, 4);
		if (i % 8 == 0) {
			uint8_t t0 = t[0];
			t[0] = sbox[t[1]] ^ rcon;
			t[1] = sbox[t[2]];
			t[2] = sbox[t[3]];
			t[3] = sbox[t0];
			rcon = xtime(rcon);
		} else if (i % 8 == 4) {
			for (int j = 0; j < 4; j++)
				t[j] = sbox[t[j]];
		}
		for (int j = 0; j < 4; j++)
			rk[i * 4 + j] = rk[(i - 8) * 4 + j] ^ t[j];
	}
}

static void aes256_decrypt_block(const uint8_t rk[240], const uint8_t in[16], uint8_t out[16])
{
	uint8_t s[16], t[16];
	for (int i = 0; i < 16; i++)
		s[i] = in[i] ^ rk[14 * 16 + i];
	for (int round = 13; round >= 0; round--) {
		/* InvShiftRows + InvSubBytes */
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				t[c * 4 + r] = rsbox[s[((c - r + 4) & 3) * 4 + r]];
		for (int i = 0; i < 16; i++)
			s[i] = t[i] ^ rk[round * 16 + i];
		if (round == 0)
			break;
		for (int c = 0; c < 4; c++) {   /* InvMixColumns */
			uint8_t *a = s + c * 4, a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
			a[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
			a[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
			a[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
			a[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
		}
	}
	memcpy(out, s, 16);
}

/* Der Schlüssel ist seit Jahren öffentlich (SII_Decrypt u. a.). */
static const uint8_t SII_KEY[32] = {
	0x2a, 0x5f, 0xcb, 0x17, 0x91, 0xd2, 0x2f, 0xb6, 0x02, 0x45, 0xb3, 0xd8, 0x36, 0x9e, 0xd0, 0xb2,
	0xc2, 0x73, 0x71, 0x56, 0x3f, 0xbf, 0x1f, 0x3c, 0x9e, 0xdf, 0x6b, 0x11, 0x82, 0x5a, 0x5d, 0x0a,
};

typedef int (*uncompress_fn)(unsigned char *, unsigned long *, const unsigned char *, unsigned long);

static uncompress_fn get_uncompress(void)
{
	static uncompress_fn fn = NULL;
	static int tried = 0;
	if (!tried) {
		tried = 1;
		void *h = dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL);
		if (h)
			fn = (uncompress_fn)dlsym(h, "uncompress");
	}
	return fn;
}

/* ScsC entschlüsseln und entpacken. Liefert neuen Puffer oder NULL. */
static unsigned char *scsc_decode(const unsigned char *d, size_t n, size_t *outlen, char *err, size_t errn)
{
	if (n < 56 + 16 || (n - 56) % 16) {
		snprintf(err, errn, "ScsC-Datei zu kurz");
		return NULL;
	}
	uncompress_fn unc = get_uncompress();
	if (!unc) {
		snprintf(err, errn, "libz.so.1 nicht gefunden");
		return NULL;
	}
	aes_init_tables();
	uint8_t rk[240];
	aes256_expand(SII_KEY, rk);
	size_t clen = n - 56;
	unsigned char *plain = (unsigned char *)malloc(clen);
	if (!plain)
		return NULL;
	const uint8_t *prev = d + 36;
	for (size_t off = 0; off < clen; off += 16) {
		uint8_t blk[16];
		aes256_decrypt_block(rk, d + 56 + off, blk);
		for (int i = 0; i < 16; i++)
			plain[off + i] = blk[i] ^ prev[i];
		prev = d + 56 + off;
	}
	unsigned pad = plain[clen - 1];
	if (pad == 0 || pad > 16) {
		free(plain);
		snprintf(err, errn, "Entschluesselung fehlgeschlagen");
		return NULL;
	}
	unsigned long dsize = (unsigned long)d[52] | (unsigned long)d[53] << 8 |
	                      (unsigned long)d[54] << 16 | (unsigned long)d[55] << 24;
	unsigned char *out = (unsigned char *)malloc(dsize + 1);
	if (!out) {
		free(plain);
		return NULL;
	}
	unsigned long olen = dsize;
	int rc = unc(out, &olen, plain, clen - pad);
	free(plain);
	if (rc != 0) {
		free(out);
		snprintf(err, errn, "zlib-Fehler %d", rc);
		return NULL;
	}
	out[olen] = 0;
	*outlen = olen;
	return out;
}

/* ------------------------------------------------------------------ */
/* Einheitlicher Zugriff auf Units (BSII und Text)                     */
/* ------------------------------------------------------------------ */

typedef struct {
	uint64_t id;          /* Hash der Unit-ID */
	const char *type;     /* Unit-Typ, z. B. "vehicle" */
	uint32_t typelen;
	uint32_t sid;         /* BSII: Struktur-ID */
	size_t off, end;      /* Datenbereich */
} unit_t;

typedef struct {
	uint32_t type;
	char *name;
} field_t;

typedef struct {
	char *name;
	uint32_t nfields;
	field_t *fields;
} struct_t;

typedef struct {
	const unsigned char *d;
	size_t n;
	int text;             /* 1 = SiiN */
	uint32_t version;     /* BSII-Version */
	struct_t *structs;    /* nach Struktur-ID indiziert */
	uint32_t nstructs;
	unit_t *units;
	size_t nunits, capunits;
	uint32_t *hash;       /* ID -> Index+1, offene Adressierung */
	size_t hmask;
} save_t;

/* Rückruf je Feldwert: name, Text (oder NULL), Länge, ID-Hash (0 = null/keine ID) */
typedef void (*field_cb)(void *ctx, const char *name, const char *txt, size_t len, uint64_t id);

static int add_unit(save_t *s, unit_t u)
{
	if (s->nunits == s->capunits) {
		size_t nc = s->capunits ? s->capunits * 2 : 4096;
		unit_t *nu = (unit_t *)realloc(s->units, nc * sizeof *nu);
		if (!nu)
			return -1;
		s->units = nu;
		s->capunits = nc;
	}
	s->units[s->nunits++] = u;
	return 0;
}

/* ---- BSII ---- */

typedef struct {
	const unsigned char *d;
	size_t n, pos;
	int err;
} cur_t;

static int need(cur_t *c, size_t k)
{
	if (c->err || c->n - c->pos < k) {
		c->err = 1;
		return 0;
	}
	return 1;
}
static uint8_t r8(cur_t *c) { return need(c, 1) ? c->d[c->pos++] : 0; }
static uint32_t r32(cur_t *c)
{
	if (!need(c, 4))
		return 0;
	const unsigned char *p = c->d + c->pos;
	c->pos += 4;
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void skip(cur_t *c, size_t k)
{
	if (need(c, k))
		c->pos += k;
}

/* Liest eine ID und liefert ihren Hash (0 für "null"). */
static uint64_t rid(cur_t *c)
{
	uint8_t parts = r8(c);
	if (parts == 0)
		return 0;
	size_t len = parts == 0xFF ? 8 : (size_t)parts * 8;
	if (!need(c, len))
		return 0;
	uint64_t h = fnv(&parts, 1, FNV0);
	h = fnv(c->d + c->pos, len, h);
	c->pos += len;
	return h ? h : 1;
}

static size_t fixed_size(uint32_t t, uint32_t ver)
{
	switch (t) {
	case 5: case 37: case 39: case 47: case 55: return 4;
	case 7: case 49: case 51: case 65: return 8;
	case 9: case 17: return 12;
	case 23: return 16;
	case 25: return ver == 1 ? 28 : 32;
	case 41: case 43: return 2;
	case 53: return 1;
	case 3: return 8;
	}
	return 0;
}

static size_t array_elem_size(uint32_t t, uint32_t ver)
{
	switch (t) {
	case 6: case 38: case 40: return 4;
	case 8: case 50: case 52: case 4: return 8;
	case 10: case 18: return 12;
	case 24: return 16;
	case 26: return ver == 1 ? 28 : 32;
	case 42: case 44: return 2;
	case 54: return 1;
	}
	return 0;
}

/* Liest einen Feldwert; ruft cb nur für Strings und IDs auf. */
static void bsii_value(save_t *s, cur_t *c, const field_t *f, field_cb cb, void *ctx)
{
	uint32_t t = f->type;
	size_t k;
	switch (t) {
	case 1: {
		uint32_t len = r32(c);
		if (need(c, len)) {
			if (cb)
				cb(ctx, f->name, (const char *)c->d + c->pos, len, 0);
			c->pos += len;
		}
		return;
	}
	case 2: {
		uint32_t cnt = r32(c);
		for (uint32_t i = 0; i < cnt && !c->err; i++) {
			uint32_t len = r32(c);
			if (need(c, len)) {
				if (cb)
					cb(ctx, f->name, (const char *)c->d + c->pos, len, 0);
				c->pos += len;
			}
		}
		return;
	}
	case 57: case 59: case 61: {
		uint64_t id = rid(c);
		if (cb)
			cb(ctx, f->name, NULL, 0, id);
		return;
	}
	case 58: case 60: case 62: {
		uint32_t cnt = r32(c);
		for (uint32_t i = 0; i < cnt && !c->err; i++) {
			uint64_t id = rid(c);
			if (cb)
				cb(ctx, f->name, NULL, 0, id);
		}
		return;
	}
	}
	if ((k = fixed_size(t, s->version))) {
		skip(c, k);
		return;
	}
	if ((k = array_elem_size(t, s->version))) {
		uint32_t cnt = r32(c);
		skip(c, (size_t)cnt * k);
		return;
	}
	c->err = 1;   /* unbekannter Typ */
}

static int bsii_index(save_t *s)
{
	cur_t c = {s->d, s->n, 4, 0};
	s->version = r32(&c);
	if (s->version < 1 || s->version > 3)
		return -1;
	int data_phase = 0;
	while (c.pos < c.n && !c.err) {
		uint32_t bt = r32(&c);
		if (bt == 0) {
			if (!r8(&c)) {
				if (data_phase)
					break;
				continue;
			}
			uint32_t sid = r32(&c);
			uint32_t nl = r32(&c);
			if (!need(&c, nl) || sid > 100000)
				return -1;
			if (sid >= s->nstructs) {
				uint32_t nn = sid + 64;
				struct_t *ns = (struct_t *)realloc(s->structs, nn * sizeof *ns);
				if (!ns)
					return -1;
				memset(ns + s->nstructs, 0, (nn - s->nstructs) * sizeof *ns);
				s->structs = ns;
				s->nstructs = nn;
			}
			struct_t *st = &s->structs[sid];
			if (st->name)   /* doppelte Definition: erste gewinnt */
				st = NULL;
			char *name = strndup((const char *)c.d + c.pos, nl);
			c.pos += nl;
			field_t *fields = NULL;
			uint32_t nf = 0;
			for (;;) {
				uint32_t ft = r32(&c);
				if (ft == 0 || c.err)
					break;
				uint32_t fl = r32(&c);
				if (!need(&c, fl))
					break;
				field_t *nfp = (field_t *)realloc(fields, (nf + 1) * sizeof *nfp);
				if (!nfp)
					return -1;
				fields = nfp;
				fields[nf].type = ft;
				fields[nf].name = strndup((const char *)c.d + c.pos, fl);
				nf++;
				c.pos += fl;
				if (ft == 55) {   /* Ordinal-Stringtabelle überspringen */
					uint32_t cnt = r32(&c);
					for (uint32_t i = 0; i < cnt && !c.err; i++) {
						r32(&c);
						skip(&c, r32(&c));
					}
				}
			}
			if (st) {
				st->name = name;
				st->fields = fields;
				st->nfields = nf;
			} else {
				free(name);
				for (uint32_t i = 0; i < nf; i++)
					free(fields[i].name);
				free(fields);
			}
			continue;
		}
		data_phase = 1;
		if (bt >= s->nstructs || !s->structs[bt].name)
			return -1;
		struct_t *st = &s->structs[bt];
		unit_t u;
		u.id = rid(&c);
		u.type = st->name;
		u.typelen = (uint32_t)strlen(st->name);
		u.sid = bt;
		u.off = c.pos;
		for (uint32_t i = 0; i < st->nfields && !c.err; i++)
			bsii_value(s, &c, &st->fields[i], NULL, NULL);
		u.end = c.pos;
		if (add_unit(s, u))
			return -1;
	}
	return c.err ? -1 : 0;
}

/* ---- Text (SiiN) ---- */

static void trim(const char **p, const char **e)
{
	while (*p < *e && isspace((unsigned char)**p))
		(*p)++;
	while (*e > *p && isspace((unsigned char)(*e)[-1]))
		(*e)--;
}

static uint64_t text_id(const char *p, size_t n)
{
	if (n == 4 && !memcmp(p, "null", 4))
		return 0;
	uint64_t h = fnv(p, n, FNV0);
	return h ? h : 1;
}

static int text_index(save_t *s)
{
	const char *d = (const char *)s->d, *end = d + s->n;
	const char *line = d;
	unit_t cur;
	int in_unit = 0;
	while (line < end) {
		const char *nl = (const char *)memchr(line, '\n', (size_t)(end - line));
		const char *le = nl ? nl : end;
		const char *p = line, *e = le;
		trim(&p, &e);
		if (!in_unit) {
			const char *colon = (const char *)memchr(p, ':', (size_t)(e - p));
			if (e > p && e[-1] == '{' && colon) {
				const char *tp = p, *te = colon, *ip = colon + 1, *ie = e - 1;
				trim(&tp, &te);
				trim(&ip, &ie);
				cur.type = tp;
				cur.typelen = (uint32_t)(te - tp);
				cur.id = text_id(ip, (size_t)(ie - ip));
				cur.sid = 0;
				cur.off = (size_t)((nl ? nl + 1 : end) - d);
				in_unit = 1;
			}
		} else if (e - p == 1 && *p == '}') {
			cur.end = (size_t)(line - d);
			if (add_unit(s, cur))
				return -1;
			in_unit = 0;
		}
		line = nl ? nl + 1 : end;
	}
	return 0;
}

/* Alle Feldwerte einer Unit durchlaufen. */
static void unit_fields(save_t *s, const unit_t *u, field_cb cb, void *ctx)
{
	if (!s->text) {
		cur_t c = {s->d, u->end, u->off, 0};
		struct_t *st = &s->structs[u->sid];
		for (uint32_t i = 0; i < st->nfields && !c.err; i++)
			bsii_value(s, &c, &st->fields[i], cb, ctx);
		return;
	}
	const char *d = (const char *)s->d;
	const char *line = d + u->off, *end = d + u->end;
	char name[64];
	while (line < end) {
		const char *nl = (const char *)memchr(line, '\n', (size_t)(end - line));
		const char *le = nl ? nl : end;
		const char *p = line, *e = le;
		trim(&p, &e);
		const char *colon = (const char *)memchr(p, ':', (size_t)(e - p));
		if (colon) {
			const char *ne = colon, *br = (const char *)memchr(p, '[', (size_t)(colon - p));
			int is_elem = br != NULL;
			if (br)
				ne = br;
			size_t nlen = (size_t)(ne - p);
			const char *vp = colon + 1, *ve = e;
			trim(&vp, &ve);
			if (ve - vp >= 2 && *vp == '"' && ve[-1] == '"') {
				vp++;
				ve--;
			}
			/* "accessories: 12" ist nur die Anzahl – das Element "accessories[0]" zählt */
			int is_count = !is_elem && ve > vp && isdigit((unsigned char)*vp) &&
			               strspn(vp, "0123456789") == (size_t)(ve - vp);
			if (nlen < sizeof name && !is_count) {
				memcpy(name, p, nlen);
				name[nlen] = 0;
				cb(ctx, name, vp, (size_t)(ve - vp), text_id(vp, (size_t)(ve - vp)));
			}
		}
		line = nl ? nl + 1 : end;
	}
}

static int build_hash(save_t *s)
{
	size_t cap = 1024;
	while (cap < s->nunits * 2)
		cap *= 2;
	s->hash = (uint32_t *)calloc(cap, sizeof *s->hash);
	if (!s->hash)
		return -1;
	s->hmask = cap - 1;
	for (size_t i = 0; i < s->nunits; i++) {
		size_t h = (size_t)s->units[i].id & s->hmask;
		while (s->hash[h])
			h = (h + 1) & s->hmask;
		s->hash[h] = (uint32_t)(i + 1);
	}
	return 0;
}

static const unit_t *find_unit(save_t *s, uint64_t id)
{
	if (!id || !s->hash)
		return NULL;
	size_t h = (size_t)id & s->hmask;
	while (s->hash[h]) {
		const unit_t *u = &s->units[s->hash[h] - 1];
		if (u->id == id)
			return u;
		h = (h + 1) & s->hmask;
	}
	return NULL;
}

static int unit_is(const unit_t *u, const char *type)
{
	size_t n = strlen(type);
	return u->typelen == n && !memcmp(u->type, type, n);
}

/* ---- einzelne Feldwerte holen ---- */

typedef struct {
	const char *want;
	uint64_t id;
	char str[256];
	int found;
} get_ctx;

static void get_cb(void *vctx, const char *name, const char *txt, size_t len, uint64_t id)
{
	get_ctx *g = (get_ctx *)vctx;
	if (g->found || strcmp(name, g->want))
		return;
	g->found = 1;
	g->id = id;
	if (txt) {
		if (len >= sizeof g->str)
			len = sizeof g->str - 1;
		memcpy(g->str, txt, len);
		g->str[len] = 0;
	}
}

static uint64_t get_id(save_t *s, const unit_t *u, const char *field)
{
	get_ctx g;
	memset(&g, 0, sizeof g);
	g.want = field;
	if (u)
		unit_fields(s, u, get_cb, &g);
	return g.id;
}

static void get_str(save_t *s, const unit_t *u, const char *field, char *out, size_t n)
{
	get_ctx g;
	memset(&g, 0, sizeof g);
	g.want = field;
	if (u)
		unit_fields(s, u, get_cb, &g);
	snprintf(out, n, "%s", g.str);
}

typedef struct {
	const char *want;
	uint64_t ids[512];
	int n;
} list_ctx;

static void list_cb(void *vctx, const char *name, const char *txt, size_t len, uint64_t id)
{
	(void)txt;
	(void)len;
	list_ctx *l = (list_ctx *)vctx;
	if (!strcmp(name, l->want) && l->n < 512 && id)
		l->ids[l->n++] = id;
}

/* ------------------------------------------------------------------ */
/* Tabelle und Klassifizierung                                          */
/* ------------------------------------------------------------------ */

typedef struct {
	char *key;
	char blink;
} extra_t;
static extra_t *extra = NULL;
static size_t nextra = 0;

void save_load_extra_table(const char *path)
{
	/* Plugin kann mehrfach initialisiert werden: alte Tabelle verwerfen */
	for (size_t i = 0; i < nextra; i++)
		free(extra[i].key);
	free(extra);
	extra = NULL;
	nextra = 0;
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		char *tab = strchr(line, '\t');
		if (line[0] == '#' || !tab)
			continue;
		*tab = 0;
		extra_t *ne = (extra_t *)realloc(extra, (nextra + 1) * sizeof *ne);
		if (!ne)
			break;
		extra = ne;
		extra[nextra].key = strdup(line);
		extra[nextra].blink = strncmp(tab + 1, "blink", 5) == 0;
		nextra++;
	}
	fclose(f);
}

static int lookup(const char *key)
{
	for (size_t i = 0; i < nextra; i++)   /* eigene Tabelle zuerst */
		if (!strcmp(extra[i].key, key))
			return extra[i].blink ? BEACON_BLINK : BEACON_ROUND;
	size_t lo = 0, hi = sizeof BEACON_TABLE / sizeof BEACON_TABLE[0];
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		int c = strcmp(BEACON_TABLE[mid].key, key);
		if (!c)
			return BEACON_TABLE[mid].blink ? BEACON_BLINK : BEACON_ROUND;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return BEACON_UNKNOWN;
}

typedef struct {
	int roof_round, roof_blink, roof_unknown;   /* Dachplatz "beacon" */
	int other_round, other_blink;               /* Hookups, Grill-Blitzer usw. */
	char model[96];
} tally_t;

static void classify_key(tally_t *t, const char *key, int is_path)
{
	int roof = is_path && strstr(key, "/accessory/beacon/") != NULL;
	int m = lookup(key);
	if (m == BEACON_UNKNOWN && roof) {
		/* Unbekannte Mod-Leuchte im Dachplatz: am Dateinamen raten */
		const char *base = strrchr(key, '/');
		base = base ? base + 1 : key;
		if (strstr(base, "strobe") || strstr(base, "flash") || strstr(base, "blink") || strstr(base, "blitz"))
			m = BEACON_BLINK;
		else if (strstr(base, "rot") || strstr(base, "classic") || strstr(base, "dreh"))
			m = BEACON_ROUND;
		else {
			t->roof_unknown++;
			return;
		}
	}
	if (m == BEACON_UNKNOWN)
		return;
	if (roof) {
		if (m == BEACON_BLINK)
			t->roof_blink++;
		else
			t->roof_round++;
	} else if (m == BEACON_BLINK)
		t->other_blink++;
	else
		t->other_round++;
}

typedef struct {
	tally_t *t;
} acc_ctx;

static void acc_cb(void *vctx, const char *name, const char *txt, size_t len, uint64_t id)
{
	(void)id;
	acc_ctx *a = (acc_ctx *)vctx;
	if (!txt || len == 0 || len > 250)
		return;
	char buf[256];
	memcpy(buf, txt, len);
	buf[len] = 0;
	if (!strcmp(name, "data_path")) {
		classify_key(a->t, buf, 1);
		const char *p = strstr(buf, "/def/vehicle/truck/");
		if (p && !a->t->model[0]) {
			p += 19;
			const char *e = strchr(p, '/');
			if (e && (size_t)(e - p) < sizeof a->t->model) {
				memcpy(a->t->model, p, (size_t)(e - p));
				a->t->model[e - p] = 0;
			}
		}
	} else if (!strcmp(name, "slot_hookup")) {
		classify_key(a->t, buf, 0);
	}
}

static void tally_vehicle(save_t *s, const unit_t *veh, tally_t *t)
{
	list_ctx l;
	memset(&l, 0, sizeof l);
	l.want = "accessories";
	unit_fields(s, veh, list_cb, &l);
	acc_ctx a = {t};
	for (int i = 0; i < l.n; i++) {
		const unit_t *acc = find_unit(s, l.ids[i]);
		if (acc)
			unit_fields(s, acc, acc_cb, &a);
	}
}

static int tally_mode(const tally_t *t, int *nr, int *nb)
{
	*nr = t->roof_round + t->other_round;
	*nb = t->roof_blink + t->other_blink;
	if (t->roof_blink || t->roof_round)          /* Dachleuchte hat Vorrang */
		return t->roof_blink >= t->roof_round ? BEACON_BLINK : BEACON_ROUND;
	if (t->roof_unknown)
		return BEACON_UNKNOWN;
	if (t->other_blink || t->other_round)
		return t->other_blink ? BEACON_BLINK : BEACON_ROUND;
	return BEACON_UNKNOWN;
}

/* Modell aus dem Spielstand ("scania.r_2016") gegen die Telemetrie-ID prüfen */
static int model_match(const char *model, const char *tel)
{
	if (!model[0] || !tel[0])
		return 0;
	return strstr(tel, model) != NULL || strstr(model, tel) != NULL;
}

/* Kennzeichen vergleichbar machen: Formatierung <...>, Länderteil |xx, Leerzeichen weg */
static void norm_plate(const char *in, char *out, size_t n)
{
	size_t o = 0;
	int tag = 0;
	for (; *in && *in != '|' && o + 1 < n; in++) {
		if (*in == '<')
			tag = 1;
		else if (*in == '>')
			tag = 0;
		else if (!tag && isalnum((unsigned char)*in))
			out[o++] = (char)toupper((unsigned char)*in);
	}
	out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* Öffentliche Funktionen                                               */
/* ------------------------------------------------------------------ */

static void free_save(save_t *s)
{
	for (uint32_t i = 0; i < s->nstructs; i++) {
		free(s->structs[i].name);
		for (uint32_t j = 0; j < s->structs[i].nfields; j++)
			free(s->structs[i].fields[j].name);
		free(s->structs[i].fields);
	}
	free(s->structs);
	free(s->units);
	free(s->hash);
}

int save_detect(const char *path, const truck_hint_t *hint, beacon_info_t *out)
{
	memset(out, 0, sizeof *out);
	out->mode = BEACON_UNKNOWN;
	size_t n = 0;
	unsigned char *raw = read_file(path, &n), *dec = NULL;
	if (!raw) {
		snprintf(out->reason, sizeof out->reason, "Spielstand nicht lesbar");
		return -1;
	}
	const unsigned char *d = raw;
	if (n >= 4 && !memcmp(raw, "ScsC", 4)) {
		char err[80] = "";
		dec = scsc_decode(raw, n, &n, err, sizeof err);
		free(raw);
		raw = NULL;
		if (!dec) {
			snprintf(out->reason, sizeof out->reason, "%s", err);
			return -1;
		}
		d = dec;
	}
	save_t s;
	memset(&s, 0, sizeof s);
	s.d = d;
	s.n = n;
	int rc;
	if (n >= 4 && !memcmp(d, "BSII", 4))
		rc = bsii_index(&s);
	else if (n >= 8 && (!memcmp(d, "SiiNunit", 8) || !memcmp(d, "\xEF\xBB\xBFSiiNunit", 11))) {
		s.text = 1;
		rc = text_index(&s);
	} else
		rc = -1;
	if (rc || build_hash(&s)) {
		snprintf(out->reason, sizeof out->reason, "Spielstand-Format nicht erkannt");
		rc = -1;
		goto done;
	}

	const unit_t *player = NULL;
	for (size_t i = 0; i < s.nunits && !player; i++)
		if (unit_is(&s.units[i], "player"))
			player = &s.units[i];
	if (!player) {
		snprintf(out->reason, sizeof out->reason, "kein Spieler im Spielstand");
		rc = -1;
		goto done;
	}

	/* 1.5x: player.assigned_vehicles -> player_vehicles.vehicle/.trailer, davor assigned_truck */
	uint64_t truck = 0, trailer = 0;
	const unit_t *pv = find_unit(&s, get_id(&s, player, "assigned_vehicles"));
	if (pv) {
		truck = get_id(&s, pv, "vehicle");
		trailer = get_id(&s, pv, "trailer");
	}
	if (!truck)
		truck = get_id(&s, player, "assigned_truck");
	if (!trailer)
		trailer = get_id(&s, player, "assigned_trailer");
	if (!truck)
		truck = get_id(&s, player, "my_truck");

	char want_plate[64] = "";
	if (hint && hint->license_plate)
		norm_plate(hint->license_plate, want_plate, sizeof want_plate);
	const char *want_model = hint && hint->truck_id ? hint->truck_id : "";

	tally_t t;
	memset(&t, 0, sizeof t);
	const unit_t *veh = find_unit(&s, truck);
	char plate[64] = "", np[64];
	if (veh) {
		tally_vehicle(&s, veh, &t);
		get_str(&s, veh, "license_plate", plate, sizeof plate);
	}
	norm_plate(plate, np, sizeof np);
	/* Kennzeichen ist der verlässliche Schlüssel; das Modell nur, wenn kein Kennzeichen da ist */
	int mismatch = want_plate[0] ? strcmp(np, want_plate) != 0
	                             : (want_model[0] && t.model[0] && !model_match(t.model, want_model));
	if (!veh || mismatch) {
		/* Spielstand evtl. älter als der LKW-Wechsel: passenden LKW per Kennzeichen suchen */
		const unit_t *best = NULL;
		tally_t bt;
		int best_score = 0;
		if (want_plate[0]) {
			for (size_t i = 0; i < s.nunits; i++) {
				if (!unit_is(&s.units[i], "vehicle"))
					continue;
				char p2[64], n2[64];
				get_str(&s, &s.units[i], "license_plate", p2, sizeof p2);
				norm_plate(p2, n2, sizeof n2);
				if (strcmp(n2, want_plate))
					continue;
				tally_t tt;
				memset(&tt, 0, sizeof tt);
				tally_vehicle(&s, &s.units[i], &tt);
				int score = 1 + (want_model[0] && model_match(tt.model, want_model));
				if (score > best_score) {
					best = &s.units[i];
					bt = tt;
					best_score = score;
					snprintf(plate, sizeof plate, "%s", p2);
				}
			}
		}
		if (best) {
			t = bt;
			veh = best;
			trailer = 0;   /* Anhänger gehört nicht sicher dazu */
		} else if (mismatch) {
			snprintf(out->reason, sizeof out->reason,
			         "aktueller LKW (%s) steht noch nicht im Spielstand", want_plate[0] ? want_plate : want_model);
			memset(&t, 0, sizeof t);
			veh = NULL;
		}
	}
	if (!veh) {
		if (!out->reason[0])
			snprintf(out->reason, sizeof out->reason, "kein eigener LKW im Spielstand (Schnellauftrag?)");
		rc = 0;
		goto done;
	}

	snprintf(out->truck, sizeof out->truck, "%s", t.model);
	snprintf(out->plate, sizeof out->plate, "%s", plate);
	out->mode = tally_mode(&t, &out->n_round, &out->n_blink);

	/* Ohne Leuchten am LKW: Anhänger (inkl. Doppel-Anhänger) ansehen */
	if (out->mode == BEACON_UNKNOWN && !t.roof_unknown) {
		tally_t tt;
		memset(&tt, 0, sizeof tt);
		for (int hop = 0; hop < 4 && trailer; hop++) {
			const unit_t *tr = find_unit(&s, trailer);
			if (!tr)
				break;
			tally_vehicle(&s, tr, &tt);
			trailer = get_id(&s, tr, "slave_trailer");
		}
		out->mode = tally_mode(&tt, &out->n_round, &out->n_blink);
		if (out->mode != BEACON_UNKNOWN) {
			snprintf(out->reason, sizeof out->reason, "Leuchten am Anhaenger");
			rc = 0;
			goto done;
		}
	}
	if (out->mode == BEACON_UNKNOWN)
		snprintf(out->reason, sizeof out->reason,
		         t.roof_unknown ? "unbekannte (Mod-)Rundumleuchte" : "keine Rundumleuchte am LKW gefunden");
	else
		snprintf(out->reason, sizeof out->reason, "%d drehend, %d blinkend", out->n_round, out->n_blink);
	rc = 0;
done:
	free_save(&s);
	free(raw);
	free(dec);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Neuesten Spielstand finden                                           */
/* ------------------------------------------------------------------ */

typedef struct {
	char best[1024];
	time_t mt;
} latest_t;

/* profiles_dir/<profil>/save/<slot>/game.sii */
static void scan_profiles(const char *profiles_dir, latest_t *l)
{
	DIR *pd = opendir(profiles_dir);
	if (!pd)
		return;
	struct dirent *pe;
	while ((pe = readdir(pd))) {
		if (pe->d_name[0] == '.')
			continue;
		char sd[1024];
		snprintf(sd, sizeof sd, "%s/%s/save", profiles_dir, pe->d_name);
		DIR *sv = opendir(sd);
		if (!sv)
			continue;
		struct dirent *se;
		while ((se = readdir(sv))) {
			if (se->d_name[0] == '.')
				continue;
			char gp[1024];
			time_t mt;
			snprintf(gp, sizeof gp, "%s/%s/game.sii", sd, se->d_name);
			if (file_mtime(gp, &mt, NULL) == 0 && mt > l->mt) {
				l->mt = mt;
				snprintf(l->best, sizeof l->best, "%s", gp);
			}
		}
		closedir(sv);
	}
	closedir(pd);
}

int save_find_latest(const char *game_id, char *path, size_t n, time_t *mtime)
{
	const char *home = getenv("HOME");
	if (!home)
		return -1;
	int ats = game_id && !strcmp(game_id, "ats");
	const char *dir = ats ? "American Truck Simulator" : "Euro Truck Simulator 2";
	const char *appid = ats ? "270880" : "227300";
	latest_t l;
	memset(&l, 0, sizeof l);
	char p[1024];

	/* lokale Profile (auch Flatpak-Steam) */
	const char *data_roots[] = {"%s/.local/share/%s", "%s/.var/app/com.valvesoftware.Steam/.local/share/%s",
	                            "%s/.var/app/com.valvesoftware.Steam/data/%s"};
	for (size_t i = 0; i < sizeof data_roots / sizeof *data_roots; i++) {
		char base[900];
		snprintf(base, sizeof base, data_roots[i], home, dir);
		snprintf(p, sizeof p, "%s/profiles", base);
		scan_profiles(p, &l);
		snprintf(p, sizeof p, "%s/steam_profiles", base);
		scan_profiles(p, &l);
	}
	/* Steam-Cloud-Profile: <steam>/userdata/<user>/<appid>/remote/profiles */
	const char *steam_roots[] = {"%s/.local/share/Steam", "%s/.steam/steam",
	                             "%s/.var/app/com.valvesoftware.Steam/.local/share/Steam",
	                             "%s/.var/app/com.valvesoftware.Steam/data/Steam"};
	for (size_t i = 0; i < sizeof steam_roots / sizeof *steam_roots; i++) {
		char ud[900];
		snprintf(ud, sizeof ud, steam_roots[i], home);
		strncat(ud, "/userdata", sizeof ud - strlen(ud) - 1);
		DIR *d = opendir(ud);
		if (!d)
			continue;
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.')
				continue;
			snprintf(p, sizeof p, "%s/%s/%s/remote/profiles", ud, e->d_name, appid);
			scan_profiles(p, &l);
		}
		closedir(d);
	}
	if (!l.best[0])
		return -1;
	snprintf(path, n, "%s", l.best);
	if (mtime)
		*mtime = l.mt;
	return 0;
}
