#include "cft_capnp.h"

#include <string.h>

#define HDR_BYTES 8 /* one segment: segment count - 1, segment size, padded to a word */

static inline void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static inline uint64_t get_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static inline uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* --- Builder --- */

static uint8_t *word_ptr(cft_cpb_t *b, uint32_t word)
{
    return b->buf + HDR_BYTES + (size_t)word * 8;
}

/* Allocates zeroed words; returns the index of the first one, or UINT32_MAX. */
static uint32_t alloc_words(cft_cpb_t *b, uint32_t n)
{
    if (b->overflow || HDR_BYTES + ((size_t)b->words + n) * 8 > b->cap) {
        b->overflow = true;
        return UINT32_MAX;
    }
    uint32_t at = b->words;
    memset(word_ptr(b, at), 0, (size_t)n * 8);
    b->words += n;
    return at;
}

static void write_struct_ptr(cft_cpb_t *b, uint32_t slot, uint32_t target, uint16_t dwords, uint16_t pwords)
{
    int32_t offset = (int32_t)target - (int32_t)slot - 1;
    uint64_t v = ((uint64_t)((uint32_t)offset << 2)) | ((uint64_t)dwords << 32) | ((uint64_t)pwords << 48);
    put_le64(word_ptr(b, slot), v);
}

static void write_list_ptr(cft_cpb_t *b, uint32_t slot, uint32_t target, uint8_t elem_size, uint32_t count)
{
    int32_t offset = (int32_t)target - (int32_t)slot - 1;
    uint64_t v = ((uint64_t)(((uint32_t)offset << 2) | 1)) | ((uint64_t)elem_size << 32) | ((uint64_t)count << 35);
    put_le64(word_ptr(b, slot), v);
}

static bool ptr_slot(cft_cpb_t *b, cft_cpb_struct_t s, unsigned ptr, uint32_t *slot)
{
    if (b->overflow || s.off == UINT32_MAX || ptr >= s.pwords) {
        b->overflow = true;
        return false;
    }
    *slot = s.off + s.dwords + ptr;
    return true;
}

void cft_cpb_init(cft_cpb_t *b, uint8_t *buf, size_t cap)
{
    b->buf = buf;
    b->cap = cap;
    b->words = 0;
    b->overflow = false;
    alloc_words(b, 1); /* root pointer */
}

cft_cpb_struct_t cft_cpb_root(cft_cpb_t *b, uint16_t dwords, uint16_t pwords)
{
    cft_cpb_struct_t s = {.off = alloc_words(b, (uint32_t)dwords + pwords), .dwords = dwords, .pwords = pwords};
    if (s.off != UINT32_MAX) {
        write_struct_ptr(b, 0, s.off, dwords, pwords);
    }
    return s;
}

cft_cpb_struct_t cft_cpb_new_struct(cft_cpb_t *b, cft_cpb_struct_t parent, unsigned ptr, uint16_t dwords,
                                    uint16_t pwords)
{
    cft_cpb_struct_t s = {.off = UINT32_MAX, .dwords = dwords, .pwords = pwords};
    uint32_t slot;
    if (!ptr_slot(b, parent, ptr, &slot)) {
        return s;
    }
    s.off = alloc_words(b, (uint32_t)dwords + pwords);
    if (s.off != UINT32_MAX) {
        write_struct_ptr(b, slot, s.off, dwords, pwords);
    }
    return s;
}

static uint8_t *data_field(cft_cpb_t *b, cft_cpb_struct_t s, unsigned bit, unsigned bits)
{
    if (b->overflow || s.off == UINT32_MAX || bit + bits > (unsigned)s.dwords * 64) {
        b->overflow = true;
        return NULL;
    }
    return word_ptr(b, s.off) + bit / 8;
}

void cft_cpb_set_u8(cft_cpb_t *b, cft_cpb_struct_t s, unsigned bit, uint8_t v)
{
    uint8_t *p = data_field(b, s, bit, 8);
    if (p) {
        p[0] = v;
    }
}

void cft_cpb_set_u16(cft_cpb_t *b, cft_cpb_struct_t s, unsigned bit, uint16_t v)
{
    uint8_t *p = data_field(b, s, bit, 16);
    if (p) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
    }
}

void cft_cpb_set_u32(cft_cpb_t *b, cft_cpb_struct_t s, unsigned bit, uint32_t v)
{
    uint8_t *p = data_field(b, s, bit, 32);
    if (p) {
        for (int i = 0; i < 4; i++) {
            p[i] = (uint8_t)(v >> (8 * i));
        }
    }
}

void cft_cpb_set_u64(cft_cpb_t *b, cft_cpb_struct_t s, unsigned bit, uint64_t v)
{
    uint8_t *p = data_field(b, s, bit, 64);
    if (p) {
        put_le64(p, v);
    }
}

void cft_cpb_set_bool(cft_cpb_t *b, cft_cpb_struct_t s, unsigned bit, bool v)
{
    uint8_t *p = data_field(b, s, bit, 1);
    if (p) {
        if (v) {
            *p |= (uint8_t)(1u << (bit % 8));
        } else {
            *p &= (uint8_t)~(1u << (bit % 8));
        }
    }
}

/* Byte list at `slot`; `extra` bytes of zero padding are counted as elements
 * (1 for the NUL of a Text). */
static void set_bytes_at(cft_cpb_t *b, uint32_t slot, const void *data, size_t len, size_t extra)
{
    size_t count = len + extra;
    uint32_t at = alloc_words(b, (uint32_t)((count + 7) / 8));
    if (at == UINT32_MAX) {
        return;
    }
    if (len) {
        memcpy(word_ptr(b, at), data, len);
    }
    write_list_ptr(b, slot, at, 2 /* byte */, (uint32_t)count);
}

void cft_cpb_set_data(cft_cpb_t *b, cft_cpb_struct_t s, unsigned ptr, const void *data, size_t len)
{
    uint32_t slot;
    if (ptr_slot(b, s, ptr, &slot)) {
        set_bytes_at(b, slot, data, len, 0);
    }
}

void cft_cpb_set_text(cft_cpb_t *b, cft_cpb_struct_t s, unsigned ptr, const char *text)
{
    uint32_t slot;
    if (ptr_slot(b, s, ptr, &slot)) {
        set_bytes_at(b, slot, text, strlen(text), 1);
    }
}

void cft_cpb_set_text_list(cft_cpb_t *b, cft_cpb_struct_t s, unsigned ptr, const char *const *texts, size_t n)
{
    uint32_t slot;
    if (!ptr_slot(b, s, ptr, &slot)) {
        return;
    }
    uint32_t at = alloc_words(b, (uint32_t)n);
    if (at == UINT32_MAX) {
        return;
    }
    write_list_ptr(b, slot, at, 6 /* pointer */, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        set_bytes_at(b, at + (uint32_t)i, texts[i], strlen(texts[i]), 1);
    }
}

size_t cft_cpb_finish(cft_cpb_t *b)
{
    if (b->overflow) {
        return 0;
    }
    memset(b->buf, 0, HDR_BYTES); /* segment count - 1 = 0 */
    b->buf[4] = (uint8_t)b->words;
    b->buf[5] = (uint8_t)(b->words >> 8);
    b->buf[6] = (uint8_t)(b->words >> 16);
    b->buf[7] = (uint8_t)(b->words >> 24);
    return HDR_BYTES + (size_t)b->words * 8;
}

/* --- Reader --- */

int cft_cpr_parse_frame(const uint8_t *buf, size_t len, cft_cpr_msg_t *msg)
{
    if (len < 4) {
        return 0;
    }
    uint32_t nseg = get_le32(buf) + 1;
    if (nseg == 0 || nseg > CFT_CP_MAX_SEGMENTS) {
        return -1;
    }
    size_t hdr = 4 + 4 * (size_t)nseg;
    hdr = (hdr + 7) & ~(size_t)7;
    if (len < hdr) {
        return 0;
    }
    size_t total = hdr;
    for (uint32_t i = 0; i < nseg; i++) {
        uint32_t w = get_le32(buf + 4 + 4 * i);
        if (w > (1u << 24)) {
            return -1;
        }
        msg->seg_words[i] = w;
        total += (size_t)w * 8;
    }
    if (total > INT32_MAX) {
        return -1;
    }
    if (len < total) {
        return 0;
    }
    size_t at = hdr;
    for (uint32_t i = 0; i < nseg; i++) {
        msg->seg[i] = buf + at;
        at += (size_t)msg->seg_words[i] * 8;
    }
    msg->nseg = nseg;
    return (int)total;
}

static bool read_word(const cft_cpr_msg_t *m, uint32_t seg, uint32_t idx, uint64_t *out)
{
    if (seg >= m->nseg || idx >= m->seg_words[seg]) {
        return false;
    }
    *out = get_le64(m->seg[seg] + (size_t)idx * 8);
    return true;
}

/*
 * Follows the pointer stored at (seg, idx), resolving far pointers. On success
 * returns the pointer word describing the target (with its offset field
 * meaningless) and the target position.
 */
static bool resolve_ptr(const cft_cpr_msg_t *m, uint32_t seg, uint32_t idx, uint64_t *tag, uint32_t *tseg,
                        uint32_t *toff)
{
    uint64_t p;
    if (!m || !read_word(m, seg, idx, &p) || p == 0) {
        return false;
    }
    if ((p & 3) == 2) { /* far pointer */
        bool dbl = (p >> 2) & 1;
        uint32_t pad = (uint32_t)(p >> 3) & 0x1fffffff;
        uint32_t pseg = (uint32_t)(p >> 32);
        uint64_t land;
        if (!read_word(m, pseg, pad, &land)) {
            return false;
        }
        if (!dbl) {
            if ((land & 3) == 2) {
                return false;
            }
            int32_t off = (int32_t)(uint32_t)land >> 2;
            *tag = land;
            *tseg = pseg;
            *toff = pad + 1 + (uint32_t)off;
            return true;
        }
        /* Double far: landing pad is a far pointer to the content, followed
         * by a tag word with offset 0. */
        if ((land & 7) != 2) {
            return false;
        }
        uint64_t t;
        if (!read_word(m, pseg, pad + 1, &t)) {
            return false;
        }
        *tag = t;
        *tseg = (uint32_t)(land >> 32);
        *toff = (uint32_t)(land >> 3) & 0x1fffffff;
        return true;
    }
    int32_t off = (int32_t)(uint32_t)p >> 2;
    *tag = p;
    *tseg = seg;
    *toff = idx + 1 + (uint32_t)off;
    return true;
}

static bool in_segment(const cft_cpr_msg_t *m, uint32_t seg, uint32_t off, uint64_t words)
{
    return seg < m->nseg && (uint64_t)off + words <= m->seg_words[seg];
}

cft_cpr_struct_t cft_cpr_root(const cft_cpr_msg_t *msg)
{
    cft_cpr_struct_t root = {.msg = msg, .seg = 0, .off = 0, .dwords = 0, .pwords = 1};
    return cft_cpr_struct(root, 0);
}

static const uint8_t *data_at(cft_cpr_struct_t s, unsigned bit, unsigned bits)
{
    if (!s.msg || bit + bits > (unsigned)s.dwords * 64) {
        return NULL;
    }
    return s.msg->seg[s.seg] + (size_t)s.off * 8 + bit / 8;
}

uint8_t cft_cpr_u8(cft_cpr_struct_t s, unsigned bit)
{
    const uint8_t *p = data_at(s, bit, 8);
    return p ? p[0] : 0;
}

uint16_t cft_cpr_u16(cft_cpr_struct_t s, unsigned bit)
{
    const uint8_t *p = data_at(s, bit, 16);
    return p ? (uint16_t)(p[0] | (p[1] << 8)) : 0;
}

uint32_t cft_cpr_u32(cft_cpr_struct_t s, unsigned bit)
{
    const uint8_t *p = data_at(s, bit, 32);
    return p ? get_le32(p) : 0;
}

uint64_t cft_cpr_u64(cft_cpr_struct_t s, unsigned bit)
{
    const uint8_t *p = data_at(s, bit, 64);
    return p ? get_le64(p) : 0;
}

bool cft_cpr_bool(cft_cpr_struct_t s, unsigned bit)
{
    const uint8_t *p = data_at(s, bit, 1);
    return p ? (*p >> (bit % 8)) & 1 : false;
}

cft_cpr_struct_t cft_cpr_struct(cft_cpr_struct_t s, unsigned ptr)
{
    cft_cpr_struct_t r = {0};
    uint64_t tag;
    uint32_t tseg, toff;
    if (!s.msg || ptr >= s.pwords || !resolve_ptr(s.msg, s.seg, s.off + s.dwords + ptr, &tag, &tseg, &toff)) {
        return r;
    }
    if ((tag & 3) != 0) {
        return r;
    }
    uint16_t dw = (uint16_t)(tag >> 32);
    uint16_t pw = (uint16_t)(tag >> 48);
    if (!in_segment(s.msg, tseg, toff, (uint64_t)dw + pw)) {
        return r;
    }
    r.msg = s.msg;
    r.seg = tseg;
    r.off = toff;
    r.dwords = dw;
    r.pwords = pw;
    return r;
}

static const unsigned elem_bits[8] = {0, 1, 8, 16, 32, 64, 64, 0};

cft_cpr_list_t cft_cpr_list(cft_cpr_struct_t s, unsigned ptr)
{
    cft_cpr_list_t l = {0};
    uint64_t tag;
    uint32_t tseg, toff;
    if (!s.msg || ptr >= s.pwords || !resolve_ptr(s.msg, s.seg, s.off + s.dwords + ptr, &tag, &tseg, &toff)) {
        return l;
    }
    if ((tag & 3) != 1) {
        return l;
    }
    uint8_t es = (uint8_t)((tag >> 32) & 7);
    uint32_t count = (uint32_t)(tag >> 35);
    if (es == 7) {
        /* count is the number of words after the tag word */
        uint64_t t;
        if (!in_segment(s.msg, tseg, toff, 1 + (uint64_t)count) || !read_word(s.msg, tseg, toff, &t)) {
            return l;
        }
        uint32_t n = (uint32_t)(t >> 2) & 0x3fffffff;
        uint16_t dw = (uint16_t)(t >> 32);
        uint16_t pw = (uint16_t)(t >> 48);
        if ((uint64_t)n * (dw + pw) > count) {
            return l;
        }
        l.off = toff + 1;
        l.count = n;
        l.dwords = dw;
        l.pwords = pw;
    } else {
        uint64_t words = ((uint64_t)count * elem_bits[es] + 63) / 64;
        if (!in_segment(s.msg, tseg, toff, words)) {
            return l;
        }
        l.off = toff;
        l.count = count;
    }
    l.msg = s.msg;
    l.seg = tseg;
    l.elem_size = es;
    return l;
}

cft_cpr_struct_t cft_cpr_list_struct(cft_cpr_list_t l, uint32_t i)
{
    cft_cpr_struct_t r = {0};
    if (!l.msg || l.elem_size != 7 || i >= l.count) {
        return r;
    }
    r.msg = l.msg;
    r.seg = l.seg;
    r.off = l.off + i * (uint32_t)(l.dwords + l.pwords);
    r.dwords = l.dwords;
    r.pwords = l.pwords;
    return r;
}

bool cft_cpr_blob(cft_cpr_struct_t s, unsigned ptr, bool is_text, const uint8_t **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    cft_cpr_list_t l = cft_cpr_list(s, ptr);
    if (!l.msg || l.elem_size != 2) {
        return false;
    }
    const uint8_t *p = l.msg->seg[l.seg] + (size_t)l.off * 8;
    size_t n = l.count;
    if (is_text) {
        if (n == 0 || p[n - 1] != 0) {
            return false;
        }
        n--;
    }
    *data = p;
    *len = n;
    return true;
}

int32_t cft_cpr_cap(cft_cpr_struct_t s, unsigned ptr)
{
    uint64_t p;
    if (!s.msg || ptr >= s.pwords || !read_word(s.msg, s.seg, s.off + s.dwords + ptr, &p)) {
        return -1;
    }
    if ((p & 3) != 3 || ((p >> 2) & 0x3fffffff) != 0) {
        return -1;
    }
    return (int32_t)(p >> 32);
}
