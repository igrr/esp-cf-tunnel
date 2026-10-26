/*
 * Minimal Cap'n Proto encoding: a single-segment builder and a bounds-checked
 * multi-segment reader. Only the pieces needed for the tunnel registration RPC
 * are implemented (structs, byte lists, pointer lists, composite lists,
 * capability pointers). Field offsets come from tools/capnp_layout.py.
 *
 * Offsets of data fields are given in bits from the start of the data section,
 * pointer fields by their index in the pointer section.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Builder --- */

typedef struct {
    uint8_t *buf;    /* 8-byte stream header followed by the segment */
    size_t cap;      /* bytes */
    uint32_t words;  /* used words in the segment */
    bool overflow;
} cft_cpb_t;

/* Struct being built: word index of its data section in the segment. */
typedef struct {
    uint32_t off;
    uint16_t dwords;
    uint16_t pwords;
} cft_cpb_struct_t;

void cft_cpb_init(cft_cpb_t *b, uint8_t *buf, size_t cap);
cft_cpb_struct_t cft_cpb_root(cft_cpb_t *b, uint16_t dwords, uint16_t pwords);
cft_cpb_struct_t cft_cpb_new_struct(cft_cpb_t *b, cft_cpb_struct_t parent, unsigned ptr, uint16_t dwords,
                                    uint16_t pwords);
void cft_cpb_set_u8(cft_cpb_t *b, cft_cpb_struct_t s, unsigned bit, uint8_t v);
void cft_cpb_set_u16(cft_cpb_t *b, cft_cpb_struct_t s, unsigned bit, uint16_t v);
void cft_cpb_set_u32(cft_cpb_t *b, cft_cpb_struct_t s, unsigned bit, uint32_t v);
void cft_cpb_set_u64(cft_cpb_t *b, cft_cpb_struct_t s, unsigned bit, uint64_t v);
void cft_cpb_set_bool(cft_cpb_t *b, cft_cpb_struct_t s, unsigned bit, bool v);
void cft_cpb_set_data(cft_cpb_t *b, cft_cpb_struct_t s, unsigned ptr, const void *data, size_t len);
void cft_cpb_set_text(cft_cpb_t *b, cft_cpb_struct_t s, unsigned ptr, const char *text);
/* List(Text) with n elements. */
void cft_cpb_set_text_list(cft_cpb_t *b, cft_cpb_struct_t s, unsigned ptr, const char *const *texts, size_t n);

/* Writes the stream framing header. Returns the total frame length in bytes,
 * or 0 if the buffer overflowed. The frame starts at b->buf. */
size_t cft_cpb_finish(cft_cpb_t *b);

/* --- Reader --- */

#define CFT_CP_MAX_SEGMENTS 8

typedef struct {
    const uint8_t *seg[CFT_CP_MAX_SEGMENTS];
    uint32_t seg_words[CFT_CP_MAX_SEGMENTS];
    uint32_t nseg;
} cft_cpr_msg_t;

/* A struct being read. msg == NULL means "null pointer": all fields read as
 * zero / empty, as Cap'n Proto prescribes. */
typedef struct {
    const cft_cpr_msg_t *msg;
    uint32_t seg;
    uint32_t off;
    uint16_t dwords;
    uint16_t pwords;
} cft_cpr_struct_t;

typedef struct {
    const cft_cpr_msg_t *msg;
    uint32_t seg;
    uint32_t off;         /* word index of the first element (after the tag for composite lists) */
    uint32_t count;
    uint8_t elem_size;    /* Cap'n Proto element size code, 7 = composite */
    uint16_t dwords;      /* composite lists only */
    uint16_t pwords;
} cft_cpr_list_t;

/*
 * Checks whether buf holds a complete framed message. Returns:
 *   > 0  frame length; msg is filled in and points into buf
 *   0    more data needed
 *   < 0  malformed frame
 */
int cft_cpr_parse_frame(const uint8_t *buf, size_t len, cft_cpr_msg_t *msg);

cft_cpr_struct_t cft_cpr_root(const cft_cpr_msg_t *msg);
uint8_t cft_cpr_u8(cft_cpr_struct_t s, unsigned bit);
uint16_t cft_cpr_u16(cft_cpr_struct_t s, unsigned bit);
uint32_t cft_cpr_u32(cft_cpr_struct_t s, unsigned bit);
uint64_t cft_cpr_u64(cft_cpr_struct_t s, unsigned bit);
bool cft_cpr_bool(cft_cpr_struct_t s, unsigned bit);
cft_cpr_struct_t cft_cpr_struct(cft_cpr_struct_t s, unsigned ptr);
/* Text or Data. For Text, *len excludes the NUL terminator. Returns false
 * (with *data = NULL, *len = 0) for a null pointer or a type mismatch. */
bool cft_cpr_blob(cft_cpr_struct_t s, unsigned ptr, bool is_text, const uint8_t **data, size_t *len);
cft_cpr_list_t cft_cpr_list(cft_cpr_struct_t s, unsigned ptr);
cft_cpr_struct_t cft_cpr_list_struct(cft_cpr_list_t l, uint32_t i);
/* Capability pointer: returns the cap table index, or -1. */
int32_t cft_cpr_cap(cft_cpr_struct_t s, unsigned ptr);
