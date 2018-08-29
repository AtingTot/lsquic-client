/* Copyright (c) 2017 - 2018 LiteSpeed Technologies Inc.  See LICENSE. */
/*
 * lsquic_parse_id12.c -- Parsing functions specific to Internet Draft 12
 *                          version of IETF QUIC
 */

#include <assert.h>
#include <inttypes.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#ifndef WIN32
#include <sys/types.h>
#else
#include <vc_compat.h>
#endif

#include "lsquic_types.h"
#include "lsquic_alarmset.h"
#include "lsquic_packet_common.h"
#include "lsquic_packet_in.h"
#include "lsquic_packet_out.h"
#include "lsquic_parse.h"
#include "lsquic_parse_common.h"
#include "lsquic_rechist.h"
#include "lsquic_sfcw.h"
#include "lsquic_stream.h"
#include "lsquic_mm.h"
#include "lsquic_malo.h"
#include "lsquic_version.h"
#include "lsquic.h"
#include "lsquic_byteswap.h"
#include "lsquic_varint.h"
#include "lsquic_conn.h"
#include "lsquic_enc_sess.h"

#define LSQUIC_LOGGER_MODULE LSQLM_PARSE
#include "lsquic_logger.h"

#define CHECK_SPACE(need, pstart, pend)  \
    do { if ((intptr_t) (need) > ((pend) - (pstart))) { return -1; } } while (0)


/* read 16 bits(2 bytes) time, unit: us */
static uint64_t
id12_read_float_time16 (const void *mem)
{
    assert(0);  /* Not implemented for ID-11 yet */
    uint16_t val;
    READ_UINT(val, 16, mem, 2);
    uint64_t temp = val;
    uint16_t exp = (temp >> 11) & 0x1F;
    if (0 == exp)
        return temp;
    else
    {
        --exp;
        temp &= 0x7FF;
        temp |= 0x800;
        return temp << exp;
    }
}


static void
id12_write_float_time16 (lsquic_time_t time_us, void *mem)
{
    assert(0);  /* Not implemented for ID-11 yet */
    uint16_t ret = 0;
    uint16_t high, i;

    if (time_us < ((uint64_t)1 << 11))
        ret = time_us;
    else if(time_us > 0x3FFC0000000)
        ret = 0xFFFF;
    else
    {
        high = 0;
        for (i = 16; i > 0; i /= 2)
        {
            if (time_us >= (uint64_t)1 << (11 + i))
            {
                high |= i;
                time_us >>= i;
            }
        }
        ret = time_us + (high << 11);
    }
#if __BYTE_ORDER == __LITTLE_ENDIAN
    ret = bswap_16(ret);
#endif
    memcpy(mem, (void *)&ret, 2);
}


static void
id12_parse_packet_in_finish (lsquic_packet_in_t *packet_in,
                                            struct packin_parse_state *state)
{
    /* We parsed everything out in the beginning */
}


static size_t
id12_packout_header_size_long (const struct lsquic_conn *lconn,
                                                enum packet_out_flags flags)
{
    size_t sz;
    enum lsquic_packno_bits packno_bits;

    packno_bits = (flags >> POBIT_SHIFT) & 0x3;

    sz = 1 /* Type */
       + 4 /* Version */
       + 1 /* DCIL/SCIL */
       + lconn->cn_dcid.len
       + lconn->cn_scid.len
       + 2 /* Always use two bytes to encode payload length */
       + packno_bits2len(packno_bits)
       ;

    return sz;
}


static size_t
id12_packout_header_size_short (const struct lsquic_conn *lconn,
                                            enum packet_out_flags flags)
{
    enum lsquic_packno_bits bits;
    size_t sz;

    bits = (flags >> POBIT_SHIFT) & 0x3;
    sz = 1 /* Type */
       + (flags & PO_CONN_ID ? lconn->cn_dcid.len : 0)
       + packno_bits2len(bits)
       ;

    return sz;
}


static size_t
id12_packout_header_size (const struct lsquic_conn *lconn,
                                enum packet_out_flags flags)
{
    if (lconn->cn_flags & LSCONN_HANDSHAKE_DONE)
        return id12_packout_header_size_short(lconn, flags);
    else
        return id12_packout_header_size_long(lconn, flags);
}


static const unsigned char header_type_to_bin[] = {
    [HETY_NOT_SET]      = 0x00,
    [HETY_INITIAL]      = 0x7F,
    [HETY_RETRY]        = 0x7E,
    [HETY_HANDSHAKE]    = 0x7D,
    [HETY_0RTT]         = 0x7C,
};


static unsigned
write_packno (unsigned char *p, lsquic_packno_t packno,
                                                enum packet_out_flags flags)
{
    enum lsquic_packno_bits bits;

    /*
     * [draft-ietf-quic-transport-12], Section 4.8:
     *
     *    +---------------------+----------------+--------------+
     *    | First octet pattern | Encoded Length | Bits Present |
     *    +---------------------+----------------+--------------+
     *    | 0b0xxxxxxx          | 1 octet        | 7            |
     *    |                     |                |              |
     *    | 0b10xxxxxx          | 2              | 14           |
     *    |                     |                |              |
     *    | 0b11xxxxxx          | 4              | 30           |
     *    +---------------------+----------------+--------------+
     */

    bits = (flags >> POBIT_SHIFT) & 0x3;
    if (bits == PACKNO_LEN_1)
    {
        p[0] = packno & 0x7F;
        return 1;
    }
    else if (bits == PACKNO_LEN_2)
    {
        p[0] = 0x80 | ((packno >> 8) & 0x3F);
        p[1] = packno;
        return 2;
    }
    else
    {
        assert(bits == PACKNO_LEN_4);
        p[0] = 0xC0 | ((packno >> 24) & 0x3F);
        p[1] = packno >> 16;
        p[2] = packno >> 8;
        p[3] = packno;
        return 4;
    }
}


static int
gen_long_pkt_header (const struct lsquic_conn *lconn,
            const struct lsquic_packet_out *packet_out, unsigned char *buf,
                                                                size_t bufsz)
{
    unsigned payload_len, bits;
    lsquic_ver_tag_t ver_tag;
    uint8_t dlen, slen;
    unsigned char *p;
    size_t need;

    need = id12_packout_header_size_long(lconn, packet_out->po_flags);
    if (need > bufsz)
    {
        errno = EINVAL;
        return -1;
    }

    p = buf;
    *p++ = 0x80 | header_type_to_bin[ packet_out->po_header_type ];
    ver_tag = lsquic_ver2tag(lconn->cn_version);
    memcpy(p, &ver_tag, sizeof(ver_tag));
    p += sizeof(ver_tag);

    dlen = lconn->cn_dcid.len;
    if (dlen)
        dlen -= 3;
    slen = lconn->cn_scid.len;
    if (slen)
        slen -= 3;
    *p++ = (dlen << 4) | slen;

    memcpy(p, lconn->cn_dcid.idbuf, lconn->cn_dcid.len);
    p += lconn->cn_dcid.len;
    memcpy(p, lconn->cn_scid.idbuf, lconn->cn_scid.len);
    p += lconn->cn_scid.len;

    payload_len = packet_out->po_data_sz
                + lconn->cn_esf_c->esf_tag_len
                /* 6th implementation draft is ID-12 plus PR 1389, which
                 * includes packet number length into bytes covered by
                 * the Length field:
                 */
                + packno_bits2len(lsquic_packet_out_packno_bits(packet_out));
    bits = vint_val2bits(payload_len);
    vint_write(p, payload_len, bits, 1 << bits);
    p += 1 << bits;

    p += write_packno(p, packet_out->po_packno, packet_out->po_flags);

    return p - buf;
}


static int
gen_short_pkt_header (const struct lsquic_conn *lconn,
            const struct lsquic_packet_out *packet_out, unsigned char *buf,
                                                                size_t bufsz)
{
    unsigned packno_len, cid_len, need;
    enum lsquic_packno_bits bits;
    uint32_t packno;

    bits = (packet_out->po_flags >> POBIT_SHIFT) & 0x3;
    packno_len = packno_bits2len(bits);
    cid_len = packet_out->po_flags & PO_CONN_ID ? lconn->cn_dcid.len : 0;

    need = 1 + cid_len + packno_len;
    if (need > bufsz)
        return -1;

    buf[0] = 0x30 | bits;

    if (cid_len)
        memcpy(buf + 1, lconn->cn_dcid.idbuf, cid_len);

    packno = packet_out->po_packno;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    packno = bswap_32(packno);
#endif
    memcpy(buf + 1 + cid_len, (unsigned char *) &packno + 4 - packno_len,
                                                                packno_len);

    return need;
}


static int
id12_gen_reg_pkt_header (const struct lsquic_conn *lconn,
            const struct lsquic_packet_out *packet_out, unsigned char *buf,
                                                                size_t bufsz)
{
    if (packet_out->po_header_type == HETY_NOT_SET)
        return gen_short_pkt_header(lconn, packet_out, buf, bufsz);
    else
        return gen_long_pkt_header(lconn, packet_out, buf, bufsz);
}


static void
id12_packno_info (const struct lsquic_conn *lconn,
        const struct lsquic_packet_out *packet_out, unsigned *packno_off,
        unsigned *packno_len)
{
    if (packet_out->po_header_type == HETY_NOT_SET)
        *packno_off = 1 +
            (packet_out->po_flags & PO_CONN_ID ? lconn->cn_dcid.len : 0);
    else
        *packno_off = 1
                    + 4
                    + 1
                    + lconn->cn_dcid.len
                    + lconn->cn_scid.len
                    + 2;
    *packno_len = packno_bits2len(
        lsquic_packet_out_packno_bits(packet_out));
}


static size_t
id12_packout_size (const struct lsquic_conn *lconn,
                                const struct lsquic_packet_out *packet_out)
{
    size_t sz;

    if ((lconn->cn_flags & LSCONN_HANDSHAKE_DONE)
                                && packet_out->po_header_type == HETY_NOT_SET)
        sz = id12_packout_header_size_short(lconn, packet_out->po_flags);
    else
        sz = id12_packout_header_size_long(lconn, packet_out->po_flags);

    sz += packet_out->po_data_sz;
    sz += lconn->cn_esf_c->esf_tag_len;

    return sz;
}


static int
id12_gen_stream_frame (unsigned char *buf, size_t buf_len,
        lsquic_stream_id_t stream_id, uint64_t offset, int fin, size_t size,
        gsf_read_f gsf_read, void *stream)
{
    /* 0b00010XXX
     *  0x4     OFF
     *  0x2     LEN
     *  0x1     FIN
     */
    unsigned sbits, obits, dbits;
    unsigned slen, olen, dlen;
    unsigned char *p = buf + 1;

    assert(!!fin ^ !!size);

    /* We do not check that stream_id, offset, and size are smaller
     * than 2^62: this is not necessary, as this code will never generate
     * this many stream IDs, nor will it even transfer this much data.
     * The size is limited by our own code.
     */

    sbits = vint_val2bits(stream_id);
    slen = 1 << sbits;
    if (offset)
    {
        obits = vint_val2bits(offset);
        olen = 1 << obits;
    }
    else
        olen = 0;

    if (!fin)
    {
        unsigned n_avail;
        size_t nr;

        n_avail = buf_len - (p + slen + olen - buf);

        /* If we cannot fill remaining buffer, we need to include data
         * length.
         */
        if (size < n_avail)
        {
            dbits = vint_val2bits(size);
            dlen = 1 << dbits;
        }
        else
            dlen = 0;
        n_avail -= dlen;

        CHECK_SPACE(1 + olen + slen + dlen +
            + 1 /* We need to write at least 1 byte */, buf, buf + buf_len);

        vint_write(p, stream_id, sbits, slen);
        p += slen;

        vint_write(p, offset, obits, olen);
        p += olen;

        /* Read as much as we can */
        nr = gsf_read(stream, p + dlen, n_avail, &fin);
        assert(nr != 0);

        if (dlen)
            vint_write(p, size, dbits, dlen);

        p += dlen + nr;
    }
    else
    {
        dlen = 1 + slen + olen < buf_len;
        CHECK_SPACE(1 + slen + olen + dlen, buf, buf + buf_len);
        vint_write(p, stream_id, sbits, slen);
        p += slen;
        vint_write(p, offset, obits, olen);
        p += olen;
        if (dlen)
            *p++ = 0;
    }

    buf[0] = 0x10
           | (!!olen << 2)
           | (!!dlen << 1)
           | (!!fin  << 0)
           ;
    return p - buf;
}


/* return parsed (used) buffer length */
static int
id12_parse_stream_frame (const unsigned char *buf, size_t rem_packet_sz,
                                        struct stream_frame *stream_frame)
{
    /* 0b00010XXX
     *  0x4     OFF
     *  0x2     LEN
     *  0x1     FIN
     */
    const unsigned char *const pend = buf + rem_packet_sz;
    const unsigned char *p = buf;
    lsquic_stream_id_t stream_id;
    uint64_t offset, data_sz;
    int r;

    CHECK_SPACE(1, p, pend);
    const char type = *p++;

    r = vint_read(p, pend, &stream_id);
    if (r < 0)
        return -1;
    p += r;

    if (type & 0x4)
    {
        r = vint_read(p, pend, &offset);
        if (r < 0)
            return -1;
        p += r;
    }
    else
        offset = 0;

    if (type & 0x2)
    {
        r = vint_read(p, pend, &data_sz);
        if (r < 0)
            return -1;
        p += r;
        CHECK_SPACE(data_sz, p, pend);
    }
    else
        data_sz = pend - p;

    /* [draft-ietf-quic-transport-11]:
     *
     *    A stream frame's Stream Data MUST NOT be empty, unless the
     *    offset is 0 or the FIN bit is set.
     */
    if (data_sz == 0 && !(offset == 0 || (type & 0x1)))
        return -1;

    stream_frame->stream_id             = stream_id;
    stream_frame->data_frame.df_fin     = type & 0x1;
    stream_frame->data_frame.df_offset  = offset;
    stream_frame->data_frame.df_size    = data_sz;
    stream_frame->data_frame.df_data    = p;
    stream_frame->data_frame.df_read_off= 0;
    stream_frame->packet_in             = NULL;

    assert(p <= pend);

    return p + data_sz - (unsigned char *) buf;
}


#if __GNUC__
#   define UNLIKELY(cond) __builtin_expect(cond, 0)
#else
#   define UNLIKELY(cond) cond
#endif


static int
id12_parse_ack_frame (const unsigned char *const buf, size_t buf_len,
                                                        struct ack_info *ack)
{
    const unsigned char *p = buf;
    const unsigned char *const end = buf + buf_len;
    uint64_t block_count, gap, block;
    unsigned i;
    int r;

    ++p;
    r = vint_read(p, end, &ack->ranges[0].high);
    if (UNLIKELY(r < 0))
        return -1;
    p += r;
    r = vint_read(p, end, &ack->lack_delta);
    if (UNLIKELY(r < 0))
        return -1;
    p += r;
    r = vint_read(p, end, &block_count);
    if (UNLIKELY(r < 0))
        return -1;
    p += r;
    r = vint_read(p, end, &block);
    if (UNLIKELY(r < 0))
        return -1;
    ack->ranges[0].low = ack->ranges[0].high - block;
    if (UNLIKELY(ack->ranges[0].high < ack->ranges[0].low))
        return -1;
    p += r;

    for (i = 1; i < block_count; ++i)
    {
        r = vint_read(p, end, &gap);
        if (UNLIKELY(r < 0))
            return -1;
        p += r;
        r = vint_read(p, end, &block);
        if (UNLIKELY(r < 0))
            return -1;
        p += r;
        ack->ranges[i].high = ack->ranges[i - 1].low - gap - 2;
        ack->ranges[i].low  = ack->ranges[i].high - block;
        if (UNLIKELY(ack->ranges[i].high >= ack->ranges[i - 1].low
                     || ack->ranges[i].high < ack->ranges[i].low))
            return -1;
    }

    ack->n_ranges = block_count + 1;
    return p - buf;
}


static int
id12_gen_stop_waiting_frame(unsigned char *buf, size_t buf_len,
                lsquic_packno_t cur_packno, enum lsquic_packno_bits bits,
                lsquic_packno_t least_unacked_packno)
{
    assert(0);  /* Not implemented for ID-11 yet */
    lsquic_packno_t delta;
    unsigned packnum_len = packno_bits2len(bits);

    if (buf_len >= 1 + packnum_len)
    {
        *buf = 0x06;
        delta = cur_packno - least_unacked_packno;
#if __BYTE_ORDER == __LITTLE_ENDIAN
        delta = bswap_64(delta);
#endif
        memcpy(buf + 1, (unsigned char *) &delta + 8 - packnum_len,
                                                            packnum_len);
        return 1 + packnum_len;
    }
    else
        return -1;
}


static int
id12_parse_stop_waiting_frame (const unsigned char *buf, size_t buf_len,
                 lsquic_packno_t cur_packno, enum lsquic_packno_bits bits,
                 lsquic_packno_t *least_unacked)
{
    assert(0);  /* Not implemented for ID-11 yet */
    lsquic_packno_t delta;
    unsigned packnum_len = packno_bits2len(bits);

    if (buf_len >= 1 + packnum_len)
    {
        READ_UINT(delta, 64, buf + 1, packnum_len);
        *least_unacked = cur_packno - delta;
        return 1 + packnum_len;
    }
    else
        return -1;
}


static int
id12_skip_stop_waiting_frame (size_t buf_len, enum lsquic_packno_bits bits)
{
    assert(0);  /* Not implemented for ID-11 yet */
    unsigned packnum_len = packno_bits2len(bits);
    if (buf_len >= 1 + packnum_len)
        return 1 + packnum_len;
    else
        return -1;
}


static int
id12_gen_window_update_frame (unsigned char *buf, int buf_len,
                            lsquic_stream_id_t stream_id64, uint64_t offset)
{
    assert(0);  /* Not implemented for ID-11 yet */
    uint32_t stream_id = stream_id64;

    if (buf_len < GQUIC_WUF_SZ)
        return -1;

    *buf = 0x04;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    stream_id = bswap_32(stream_id);
#endif
    memcpy(buf + 1, (unsigned char *) &stream_id, 4);
#if __BYTE_ORDER == __LITTLE_ENDIAN
    offset = bswap_64(offset);
#endif
    memcpy(buf + 1 + 4, (unsigned char *) &offset, 8);
    return GQUIC_WUF_SZ;
}


static int
id12_parse_window_update_frame (const unsigned char *buf, size_t buf_len,
                      lsquic_stream_id_t *stream_id_p, uint64_t *offset)
{
    assert(0);  /* Not implemented for ID-11 yet */
    uint32_t stream_id;

    if (buf_len < GQUIC_WUF_SZ)
        return -1;

    READ_UINT(stream_id, 32, buf + 1, 4);
    READ_UINT(*offset, 64, buf + 1 + 4, 8);
    *stream_id_p = stream_id;
    return GQUIC_WUF_SZ;
}


static int
id12_gen_blocked_frame (unsigned char *buf, size_t buf_len,
                            lsquic_stream_id_t stream_id64)
{
    assert(0);  /* Not implemented for ID-11 yet */
    uint32_t stream_id = stream_id64;

    if (buf_len < GQUIC_BLOCKED_FRAME_SZ)
        return -1;

    *buf = 0x05;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    stream_id = bswap_32(stream_id);
#endif
    memcpy(buf + 1, &stream_id, 4);
    return GQUIC_BLOCKED_FRAME_SZ;
}


static int
id12_parse_blocked_frame (const unsigned char *buf, size_t buf_len,
                                            lsquic_stream_id_t *stream_id_p)
{
    assert(0);  /* Not implemented for ID-11 yet */
    uint32_t stream_id;
    if (buf_len < GQUIC_BLOCKED_FRAME_SZ)
        return -1;

    READ_UINT(stream_id, 32, buf + 1, 4);
    *stream_id_p = stream_id;
    return GQUIC_BLOCKED_FRAME_SZ;
}


static int
id12_gen_rst_frame (unsigned char *buf, size_t buf_len,
        lsquic_stream_id_t stream_id64, uint64_t offset, uint32_t error_code)
{
    assert(0);  /* Not implemented for ID-11 yet */
    uint32_t stream_id = stream_id64;
    unsigned char *p = buf;
    if (buf_len < GQUIC_RST_STREAM_SZ)
        return -1;

    *p = 0x01;
    ++p;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    stream_id = bswap_32(stream_id);
#endif
    memcpy(p, &stream_id, 4);
    p += 4;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    offset = bswap_64(offset);
#endif
    memcpy(p, &offset, 8);
    p += 8;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    error_code = bswap_32(error_code);
#endif
    memcpy(p, &error_code, 4);
    p += 4;
    return p - buf;
}


static int
id12_parse_rst_frame (const unsigned char *buf, size_t buf_len,
    lsquic_stream_id_t *stream_id_p, uint64_t *offset_p, uint32_t *error_code_p)
{
    const unsigned char *p = buf + 1;
    const unsigned char *const end = buf + buf_len;
    uint64_t stream_id, offset;
    uint16_t error_code;
    int r;

    r = vint_read(p, end, &stream_id);
    if (r < 0)
        return r;
    p += r;

    if (end - p < 2)
        return -1;

    READ_UINT(error_code, 16, p, 2);
    p += 2;

    r = vint_read(p, end, &offset);
    if (r < 0)
        return r;
    p += r;

    *stream_id_p = stream_id;
    *offset_p = offset;
    *error_code_p = error_code;

    return p - buf;
}


static int
id12_gen_ping_frame (unsigned char *buf, int buf_len)
{
    assert(0);  /* Not implemented for ID-11 yet */
    if (buf_len > 0)
    {
        buf[0] = 0x07;
        return 1;
    }
    else
        return -1;
}


static int
id12_gen_connect_close_frame (unsigned char *buf, int buf_len, uint32_t error_code,
                            const char *reason, int reason_len)
{
    assert(0);  /* Not implemented for ID-11 yet */
    unsigned char *p = buf;
    if (buf_len < 7)
        return -1;

    *p = 0x02;
    ++p;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    error_code = bswap_32(error_code);
#endif
    memcpy(p, &error_code, 4);
    p += 4;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    const uint16_t copy = bswap_16(reason_len);
    memcpy(p, &copy, 2);
#else
    memcpy(p, &reason_len, 2);
#endif
    p += 2;
    memcpy(p, reason, reason_len);
    p += reason_len;
    if (buf_len < p - buf)
        return -2;

    return p - buf;
}


static int
id12_parse_connect_close_frame (const unsigned char *buf, size_t buf_len,
        uint32_t *error_code, uint16_t *reason_len, uint8_t *reason_offset)
{
    const unsigned char *const pend = buf + buf_len;
    const unsigned char *p;
    uint64_t len;
    uint16_t code;
    ptrdiff_t off;
    int r;

    if (buf_len < 4)
        return -1;

    p = buf + 1;
    memcpy(&code, p, 2);
    p += 2;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    code = bswap_16(code);
#endif

    r = vint_read(p, pend, &len);
    if (r < 0)
        return -1;
    p += r;

    off = p - buf;
    if (buf_len < off + len)
        return -2;

    *error_code = code;
    *reason_len = len;
    *reason_offset = off;
    return off + len;
}


static int
id12_gen_goaway_frame (unsigned char *buf, size_t buf_len,
        uint32_t error_code, lsquic_stream_id_t last_good_stream_id64,
        const char *reason, size_t reason_len)
{
    assert(0);  /* Not implemented for ID-11 yet */
    uint32_t last_good_stream_id = last_good_stream_id64;
    unsigned char *p = buf;
    if (buf_len < GQUIC_GOAWAY_FRAME_SZ + reason_len)
        return -1;

    *p = 0x03;
    ++p;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    error_code = bswap_32(error_code);
#endif
    memcpy(p, &error_code, 4);
    p += 4;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    last_good_stream_id = bswap_32(last_good_stream_id);
#endif
    memcpy(p, &last_good_stream_id, 4);
    p += 4;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint16_t copy = bswap_16(reason_len);
    memcpy(p, &copy, 2);
#else
    memcpy(p, &reason_len, 2);
#endif
    p += 2;
    if (reason_len)
    {
        memcpy(p, reason, reason_len);
        p += reason_len;
    }

    return p - buf;
}


/* the reason is buf + *reason_offset, length is *reason_length */
static int
id12_parse_goaway_frame (const unsigned char *buf, size_t buf_len,
               uint32_t *error_code, lsquic_stream_id_t *last_good_stream_id,
               uint16_t *reason_length, const char **reason)
{
    assert(0);  /* Not implemented for ID-11 yet */
    uint32_t stream_id;
    if (buf_len < GQUIC_GOAWAY_FRAME_SZ)
        return -1;

    READ_UINT(*error_code,          32, buf + 1,         4);
    READ_UINT(stream_id,            32, buf + 1 + 4,     4);
    READ_UINT(*reason_length,       16, buf + 1 + 4 + 4, 2);
    if (*reason_length)
    {
        if ((int)buf_len < GQUIC_GOAWAY_FRAME_SZ + *reason_length)
            return -2;
        *reason = (const char *) buf + GQUIC_GOAWAY_FRAME_SZ;
    }
    else
        *reason = NULL;

    *last_good_stream_id = stream_id;
    return GQUIC_GOAWAY_FRAME_SZ + *reason_length;
}


/* Returns number of bytes written or -1 on failure */
/* This function makes an assumption that there is at least one range */
static int
id12_gen_ack_frame (unsigned char *outbuf, size_t outbuf_sz,
        gaf_rechist_first_f rechist_first, gaf_rechist_next_f rechist_next,
        gaf_rechist_largest_recv_f rechist_largest_recv,
        void *rechist, lsquic_time_t now, int *has_missing,
        lsquic_packno_t *largest_received)
{
    assert(0);  /* Not implemented for ID-11 yet */
    lsquic_time_t time_diff;
    lsquic_packno_t tmp_packno;
    const struct lsquic_packno_range *const first = rechist_first(rechist);
    if (!first)
    {
        errno = EINVAL;
        return -1;
    }

    /* Copy values from the first range, because the memory the pointer
     * points to may change:
     */
    const lsquic_packno_t first_low = first->low, first_high = first->high;

    unsigned char *p = outbuf;
    unsigned char *const type = p;
    unsigned char *const end = p + outbuf_sz;

#define AVAIL() (end - p)

#define CHECKOUT(sz) do {                                               \
    if ((intptr_t) (sz) > AVAIL()) {                                    \
        errno = ENOBUFS;                                                \
        return -1;                                                      \
    }                                                                   \
} while (0)

    CHECKOUT(1);
    ++p;

    /* 01nullmm */
    *type = 0x40;

    unsigned largest_acked_len, ack_block_len, bits;

    /* Calculate largest ACKed len and set `ll' bits: */
    const lsquic_packno_t maxno = first_high;
    bits = (maxno >= (1ULL <<  8))
         + (maxno >= (1ULL << 16))
         + (maxno >= (1ULL << 32));
    largest_acked_len = (1 << bits) - ((maxno >= (1ULL << 32)) << 1);
    *type |= bits << 2;

    /* Calculate largest ACK block length and set `mm' bits: */
    unsigned n_ranges = 0;
    lsquic_packno_t maxdiff = 0;
    const struct lsquic_packno_range *range;
    for (range = rechist_first(rechist); range; range = rechist_next(rechist))
    {
        ++n_ranges;
        const lsquic_packno_t diff = range->high - range->low + 1;
        if (diff > maxdiff)
            maxdiff = diff;
    }
    bits = (maxdiff >= (1ULL <<  8))
         + (maxdiff >= (1ULL << 16))
         + (maxdiff >= (1ULL << 32));
    ack_block_len = (1 << bits) - ((maxdiff >= (1ULL << 32)) << 1);
    *type |= bits;

    CHECKOUT(largest_acked_len);
    tmp_packno = maxno;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    tmp_packno = bswap_64(maxno);
#endif
    memcpy(p, (unsigned char *) &tmp_packno + 8 - largest_acked_len,
                                                            largest_acked_len);
    p += largest_acked_len;

    CHECKOUT(2);
    time_diff = now - rechist_largest_recv(rechist);
    id12_write_float_time16(time_diff, p);
    LSQ_DEBUG("%s: diff: %"PRIu64"; encoded: 0x%04X", __func__, time_diff,
        *(uint16_t*)p);
    p += 2;

    if (n_ranges > 1)
    {
        *has_missing = 1;
        *type |= 0x20;
        /* We need to write out at least one range */
        CHECKOUT(2 * (1 + ack_block_len));
        unsigned char *const n_ranges_p = p;             /* Set this later */
        lsquic_packno_t diff = maxno - first_low + 1;
#if __BYTE_ORDER == __LITTLE_ENDIAN
        diff = bswap_64(diff);
#endif
        memcpy(p + 1, (unsigned char *) &diff + 8 - ack_block_len,
                                                            ack_block_len);
        p += ack_block_len + 1;
        /* Write out ack blocks until one of the following occurs:
         *  1. We run out of intervals.
         *  2. We run out of room.
         *  3. We run out of highest possible number of ACK blocks (0xFF).
         */
        range = rechist_first(rechist);
        lsquic_packno_t gap = 0;
        n_ranges = 0;
        do {
            if (0 == gap)
            {
                const lsquic_packno_t prev_low = range->low;
                range = rechist_next(rechist);
                if (!range)
                    break;
                gap = prev_low - range->high - 1;
            }
            if (gap >= 0x100)
            {
                *p = 0xFF;
                gap -= 0xFF;
                memset(p + 1, 0, ack_block_len);
            }
            else
            {
                *p = gap;
                gap = 0;
                diff = range->high - range->low + 1;
#if __BYTE_ORDER == __LITTLE_ENDIAN
                diff = bswap_64(diff);
#endif
                memcpy(p + 1, (unsigned char *) &diff + 8 - ack_block_len,
                                                                ack_block_len);
            }
            p += ack_block_len + 1;
            ++n_ranges;
        } while (n_ranges < 0xFF &&
                 AVAIL() >= (intptr_t) ack_block_len + 1 + 1 /* timestamp byte */);
        *n_ranges_p = n_ranges;
    }
    else
    {
        *has_missing = 0;
        CHECKOUT(ack_block_len);
        lsquic_packno_t diff = maxno - first_low + 1;
#if __BYTE_ORDER == __LITTLE_ENDIAN
        diff = bswap_64(diff);
#endif
        memcpy(p, (unsigned char *) &diff + 8 - ack_block_len, ack_block_len);
        p += ack_block_len;
    }

    /* We do not generate timestamp list because the reference implementation
     * does not use them.  When that changes, we will start sending timestamps
     * over.
     */
    CHECKOUT(1);
    *p = 0;
    ++p;

    *largest_received = maxno;
    return p - (unsigned char *) outbuf;

#undef CHECKOUT
}


static size_t
id12_calc_stream_frame_header_sz (lsquic_stream_id_t stream_id,
                                                        uint64_t offset)
{
    if (offset)
        return 1
            + (1 << vint_val2bits(stream_id))
            + (1 << vint_val2bits(offset));
    else
        return 1
            + (1 << vint_val2bits(stream_id));
}


static enum QUIC_FRAME_TYPE
id12_parse_frame_type (unsigned char byte)
{
    return lsquic_iquic_byte2type[byte];
}


static int
id12_parse_path_chal (const unsigned char *buf, size_t len, uint64_t *chal)
{
    if (len > 9)
    {
        memcpy(chal, buf + 1, 8);
        return 9;
    }
    else
        return -1;
}


void
id12_turn_on_fin (unsigned char *stream_frame_header)
{
    assert(0);  /* Not implemented for ID-11 yet */
}


static enum lsquic_packno_bits
id12_calc_packno_bits (lsquic_packno_t packno,
                    lsquic_packno_t least_unacked, uint64_t n_in_flight)
{
    uint64_t delta;
    unsigned bits;

    delta = packno - least_unacked;
    if (n_in_flight > delta)
        delta = n_in_flight;

    /*
     * [draft-ietf-quic-transport-12], Section 4.8:
     *
     *    +---------------------+----------------+--------------+
     *    | First octet pattern | Encoded Length | Bits Present |
     *    +---------------------+----------------+--------------+
     *    | 0b0xxxxxxx          | 1 octet        | 7            |
     *    |                     |                |              |
     *    | 0b10xxxxxx          | 2              | 14           |
     *    |                     |                |              |
     *    | 0b11xxxxxx          | 4              | 30           |
     *    +---------------------+----------------+--------------+
     */
    delta *= 4;
    bits = (delta >= (1ULL <<  7))
         + (delta >= (1ULL << 14));

    return bits;
}


const struct parse_funcs lsquic_parse_funcs_id12 =
{
    .pf_gen_reg_pkt_header            =  id12_gen_reg_pkt_header,
    .pf_parse_packet_in_finish        =  id12_parse_packet_in_finish,
    .pf_gen_stream_frame              =  id12_gen_stream_frame,
    .pf_calc_stream_frame_header_sz   =  id12_calc_stream_frame_header_sz,
    .pf_parse_stream_frame            =  id12_parse_stream_frame,
    .pf_parse_ack_frame               =  id12_parse_ack_frame,
    .pf_gen_ack_frame                 =  id12_gen_ack_frame,
    .pf_gen_stop_waiting_frame        =  id12_gen_stop_waiting_frame,
    .pf_parse_stop_waiting_frame      =  id12_parse_stop_waiting_frame,
    .pf_skip_stop_waiting_frame       =  id12_skip_stop_waiting_frame,
    .pf_gen_window_update_frame       =  id12_gen_window_update_frame,
    .pf_parse_window_update_frame     =  id12_parse_window_update_frame,
    .pf_gen_blocked_frame             =  id12_gen_blocked_frame,
    .pf_parse_blocked_frame           =  id12_parse_blocked_frame,
    .pf_gen_rst_frame                 =  id12_gen_rst_frame,
    .pf_parse_rst_frame               =  id12_parse_rst_frame,
    .pf_gen_connect_close_frame       =  id12_gen_connect_close_frame,
    .pf_parse_connect_close_frame     =  id12_parse_connect_close_frame,
    .pf_gen_goaway_frame              =  id12_gen_goaway_frame,
    .pf_parse_goaway_frame            =  id12_parse_goaway_frame,
    .pf_gen_ping_frame                =  id12_gen_ping_frame,
#ifndef NDEBUG
    .pf_write_float_time16            =  id12_write_float_time16,
    .pf_read_float_time16             =  id12_read_float_time16,
#endif
    .pf_parse_frame_type              =  id12_parse_frame_type,
    .pf_turn_on_fin                   =  id12_turn_on_fin,
    .pf_packout_size                  =  id12_packout_size,
    .pf_packout_header_size           =  id12_packout_header_size,
    .pf_parse_path_chal_frame         =  id12_parse_path_chal,
    .pf_calc_packno_bits              =  id12_calc_packno_bits,
    .pf_packno_info                   =  id12_packno_info,
};
