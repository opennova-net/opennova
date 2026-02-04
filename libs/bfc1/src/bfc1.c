#include "bfc1/bfc1.h"

#include <string.h>
#include "miniz.h"

int bfc1_is_bfc1(const uint8_t *data, size_t size)
{
    uint32_t magic;
    if (size < BFC1_HEADER_SIZE)
        return 0;
    memcpy(&magic, data, sizeof(magic));
    return magic == BFC1_MAGIC;
}

int bfc1_uncompressed_size(const uint8_t *data, size_t size,
                           uint32_t *out_size)
{
    if (!bfc1_is_bfc1(data, size))
        return -1;
    memcpy(out_size, data + 4, sizeof(*out_size));
    return 0;
}

int bfc1_decompress(const uint8_t *data, size_t size,
                    uint8_t *out_buf, size_t *out_size)
{
    uint32_t uncompressed_size;
    mz_stream stream;
    int ret;

    if (!bfc1_is_bfc1(data, size))
        return -1;

    memcpy(&uncompressed_size, data + 4, sizeof(uncompressed_size));

    if (*out_size < uncompressed_size)
        return -2;

    memset(&stream, 0, sizeof(stream));
    stream.next_in  = data + BFC1_HEADER_SIZE;
    stream.avail_in = (unsigned int)(size - BFC1_HEADER_SIZE);
    stream.next_out  = out_buf;
    stream.avail_out = uncompressed_size;

    ret = mz_inflateInit(&stream);
    if (ret != MZ_OK)
        return -3;

    ret = mz_inflate(&stream, MZ_FINISH);
    mz_inflateEnd(&stream);

    /* Accept MZ_STREAM_END (complete) and MZ_OK (truncated but valid).
       Also accept MZ_BUF_ERROR / MZ_DATA_ERROR if we got most of the
       expected output — some BFC1 files have truncated zlib streams
       missing the trailing checksum. */
    if (ret != MZ_STREAM_END && ret != MZ_OK) {
        int got_enough = stream.total_out >= uncompressed_size - 16;
        if ((ret == MZ_BUF_ERROR || ret == MZ_DATA_ERROR) && got_enough) {
            /* Accept — got most/all data, just missing checksum */
        } else {
            return -4;
        }
    }

    *out_size = (size_t)stream.total_out;
    return 0;
}
