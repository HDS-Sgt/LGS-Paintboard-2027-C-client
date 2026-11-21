#include <zstd.h>

#include "common_head.hpp"

using std::runtime_error;

// ikapb：WebSocket 的每条二进制消息都恰好是一个完整的 Zstandard frame（协议 §3.2），
// 解压结果才是应用层载荷。这里只做「一帧进、一段字节出」，不认识消息类型。
// src 里必须恰好一个 frame，且不能有尾随字节，否则按协议算格式错误。
fn zstddec(const vector<u$b> &src, vector<u$b> &out, size_t cap = 1u << 20) {
   if (src.empty())
      throw runtime_error("zstd: empty message");

   size_t frame = ZSTD_findFrameCompressedSize(src.data(), src.size());
   if (ZSTD_isError(frame))
      throw runtime_error(_Str("zstd: bad frame: ") + ZSTD_getErrorName(frame));
   if (frame != src.size())
      throw runtime_error("zstd: trailing bytes after frame");

   unsigned long long siz = ZSTD_getFrameContentSize(src.data(), src.size());
   if (siz == ZSTD_CONTENTSIZE_ERROR)
      throw runtime_error("zstd: not a zstd frame");
   if (siz == ZSTD_CONTENTSIZE_UNKNOWN)
      siz = cap;
   if (siz > cap)
      throw runtime_error("zstd: payload exceeds cap");

   out.assign(static_cast<size_t>(siz), 0);
   size_t got = ZSTD_decompress(out.data(), out.size(), src.data(), src.size());
   if (ZSTD_isError(got))
      throw runtime_error(_Str("zstd: ") + ZSTD_getErrorName(got));
   out.resize(got);
}

// 反向：把一段应用层载荷压成一个完整 frame。协议不限制级别（≤19 都合法）。
fn zstdenc(const vector<u$b> &src, vector<u$b> &out, $d level = 1) {
   out.assign(ZSTD_compressBound(src.size()), 0);
   size_t got = ZSTD_compress(out.data(), out.size(), src.data(), src.size(), level);
   if (ZSTD_isError(got))
      throw runtime_error(_Str("zstd: ") + ZSTD_getErrorName(got));
   out.resize(got);
}
