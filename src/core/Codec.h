/*
 * The byte format of every p2p message: little endian integers, length-prefixed byte strings. A Reader never reads past its end:
 * it turns "bad" and returns zeros, so a malformed message from the network is rejected by one check of ok() after decoding.
 */
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace p2p
{
    using Bytes = std::vector<uint8_t>;
    using Id128 = std::array<uint8_t, 16>;
    using Key32 = std::array<uint8_t, 32>;

    inline Bytes ToBytes(std::string const& s) { return Bytes(s.begin(), s.end()); }
    inline std::string ToString(Bytes const& b) { return std::string(b.begin(), b.end()); }
    std::string Hex(uint8_t const* data, size_t size);
    inline std::string Hex(Bytes const& b) { return Hex(b.data(), b.size()); }
    inline std::string Hex(Id128 const& id) { return Hex(id.data(), id.size()); }
    inline std::string Hex(Key32 const& k) { return Hex(k.data(), k.size()); }
    bool FromHex(std::string const& text, Bytes& out);

    class Writer
    {
    public:
        Bytes data;

        Writer& U8(uint8_t v) { data.push_back(v); return *this; }
        Writer& U16(uint16_t v) { return Raw(&v, 2); }
        Writer& U32(uint32_t v) { return Raw(&v, 4); }
        Writer& U64(uint64_t v) { return Raw(&v, 8); }
        Writer& I64(int64_t v) { return Raw(&v, 8); }
        Writer& F32(float v) { return Raw(&v, 4); }
        Writer& Bool(bool v) { return U8(v ? 1 : 0); }
        Writer& Id(Id128 const& id) { return Raw(id.data(), 16); }
        Writer& Key(Key32 const& k) { return Raw(k.data(), 32); }
        Writer& Blob(Bytes const& b) { U32(static_cast<uint32_t>(b.size())); return Raw(b.data(), b.size()); }
        Writer& Blob(uint8_t const* p, size_t n) { U32(static_cast<uint32_t>(n)); return Raw(p, n); }
        Writer& Str(std::string const& s) { U32(static_cast<uint32_t>(s.size())); return Raw(s.data(), s.size()); }
        Writer& Raw(void const* p, size_t n)
        {
            if (n)
            {
                auto b = static_cast<uint8_t const*>(p);
                data.insert(data.end(), b, b + n);   // (little endian host: wasm, x86 and arm64 all are)
            }
            return *this;
        }
        Writer& Raw(Bytes const& b) { return Raw(b.data(), b.size()); }
    };

    class Reader
    {
    public:
        Reader(uint8_t const* p, size_t n) : _p(p), _n(n) { }
        explicit Reader(Bytes const& b) : _p(b.data()), _n(b.size()) { }

        bool ok() const { return !_bad; }
        bool end() const { return _at == _n; }
        size_t remaining() const { return _n - _at; }
        uint8_t const* here() const { return _p + _at; }

        uint8_t U8() { uint8_t v = 0; Take(&v, 1); return v; }
        uint16_t U16() { uint16_t v = 0; Take(&v, 2); return v; }
        uint32_t U32() { uint32_t v = 0; Take(&v, 4); return v; }
        uint64_t U64() { uint64_t v = 0; Take(&v, 8); return v; }
        int64_t I64() { int64_t v = 0; Take(&v, 8); return v; }
        float F32() { float v = 0; Take(&v, 4); return v; }
        bool Bool() { return U8() != 0; }
        Id128 Id() { Id128 v{}; Take(v.data(), 16); return v; }
        Key32 Key() { Key32 v{}; Take(v.data(), 32); return v; }
        Bytes Blob(size_t limit = 1 << 20)
        {
            uint32_t n = U32();
            if (_bad || n > limit || n > remaining()) { _bad = true; return {}; }
            Bytes b(_p + _at, _p + _at + n);
            _at += n;
            return b;
        }
        std::string Str(size_t limit = 4096) { return ToString(Blob(limit)); }
        Bytes Rest() { Bytes b(_p + _at, _p + _n); _at = _n; return b; }   // everything that is left

    private:
        void Take(void* out, size_t n)
        {
            if (_bad || n > remaining()) { _bad = true; return; }
            memcpy(out, _p + _at, n);
            _at += n;
        }
        uint8_t const* _p;
        size_t _n, _at = 0;
        bool _bad = false;
    };
}
