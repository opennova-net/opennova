#include <formats/bfc1/bfc1.h>

#include <string.h>
#include <miniz.h> // third_party/miniz

namespace opennova::bfc1 {

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

    /* The game's read: the header's size is the file's, the stream inflated into a buffer of it
       (zlib's uncompress, inflate to Z_FINISH) and its result never tested; a stream that reaches
       its own end gives its length instead. So a truncated or damaged stream, or one inflate
       refuses outright, keeps the header's size with what inflated at its head; the bytes past
       that are the allocation's, never written, which here are zero (PERMANENT).
       [orig: AudioFile_DecompressBFC_Aligned @ 0x75AFB0 - *outSize = header[1] @0x75B00A,
        ZLib_DecompressBuffer called @0x75B012, its result dropped; ZLib_DecompressBuffer
        @ 0x63E80E - *destSize = total_out only on Z_STREAM_END @0x63E87D] */
    memset(&stream, 0, sizeof(stream));
    stream.next_in  = data + BFC1_HEADER_SIZE;
    stream.avail_in = (unsigned int)(size - BFC1_HEADER_SIZE);
    stream.next_out  = out_buf;
    stream.avail_out = uncompressed_size;

    ret = mz_inflateInit(&stream);
    if (ret == MZ_OK) {
        ret = mz_inflate(&stream, MZ_FINISH);
        mz_inflateEnd(&stream);
    }
    const size_t inflated = (size_t)stream.total_out;
    if (ret == MZ_STREAM_END) {
        *out_size = inflated;
    } else {
        if (uncompressed_size > inflated)
            memset(out_buf + inflated, 0, uncompressed_size - inflated);
        *out_size = uncompressed_size;
    }
    return 0;
}

bool bfc1_unpack(std::vector<uint8_t> &data)
{
    if (!bfc1_is_bfc1(data.data(), data.size()))
        return true; /* not BFC1: no-op */
    uint32_t usize = 0;
    if (bfc1_uncompressed_size(data.data(), data.size(), &usize) != 0)
        return false;
    std::vector<uint8_t> out(usize);
    size_t out_size = out.size();
    uint8_t *out_ptr = out.empty() ? nullptr : out.data();
    if (bfc1_decompress(data.data(), data.size(), out_ptr, &out_size) != 0)
        return false;
    out.resize(out_size);
    data.swap(out);
    return true;
}

} // namespace opennova::bfc1
