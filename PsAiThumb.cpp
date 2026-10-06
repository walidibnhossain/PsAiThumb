// PsAiThumb - File Explorer thumbnail provider for Adobe Photoshop / Illustrator / InDesign files.
//
// Strategy (no Adobe software required):
//   PSD / PSB : 1) embedded JPEG thumbnail (image resource 1036)
//               2) merged composite image data (raw / RLE, RGB / Gray / CMYK, 8/16-bit)
//               3) XMP thumbnail
//   AI / AIT / INDD / INDT / EPS / PDD : XMP thumbnail (xmpGImg:image, base64 JPEG)
//
// Build: x64 only. See build.bat

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <olectl.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")
#pragma comment(linker, "/EXPORT:DllRegisterServer,PRIVATE")
#pragma comment(linker, "/EXPORT:DllUnregisterServer,PRIVATE")

// {5B2D8E61-3C4F-4A7B-9E21-6D0F7A1C8B34}
static const CLSID CLSID_PsAiThumb =
    {0x5b2d8e61, 0x3c4f, 0x4a7b, {0x9e, 0x21, 0x6d, 0x0f, 0x7a, 0x1c, 0x8b, 0x34}};

static HINSTANCE g_hInst = nullptr;
static LONG g_objects = 0;

// ---------------------------------------------------------------- helpers

static bool ReadAt(IStream* s, uint64_t pos, void* buf, ULONG n) {
    if (n == 0) return true;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)pos;
    if (FAILED(s->Seek(li, STREAM_SEEK_SET, nullptr))) return false;
    ULONG got = 0;
    HRESULT hr = s->Read(buf, n, &got);
    return SUCCEEDED(hr) && got == n;
}

static uint64_t StreamSize(IStream* s) {
    STATSTG st{};
    if (FAILED(s->Stat(&st, STATFLAG_NONAME))) return 0;
    return st.cbSize.QuadPart;
}

static uint16_t BE16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t BE32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t BE64(const uint8_t* p) { return ((uint64_t)BE32(p) << 32) | BE32(p + 4); }

// Scale any WIC bitmap to fit cx and return a 32bpp premultiplied-alpha DIB.
static HRESULT SourceToHBitmap(IWICImagingFactory* f, IWICBitmapSource* src, UINT cx, HBITMAP* out) {
    UINT w = 0, h = 0;
    HRESULT hr = src->GetSize(&w, &h);
    if (FAILED(hr) || !w || !h) return E_FAIL;

    double sc = std::min((double)cx / w, (double)cx / h);
    UINT nw = std::max<UINT>(1, (UINT)(w * sc + 0.5));
    UINT nh = std::max<UINT>(1, (UINT)(h * sc + 0.5));

    ComPtr<IWICBitmapScaler> scaler;
    hr = f->CreateBitmapScaler(&scaler);
    if (FAILED(hr)) return hr;
    hr = scaler->Initialize(src, nw, nh, WICBitmapInterpolationModeFant);
    if (FAILED(hr)) return hr;

    ComPtr<IWICFormatConverter> conv;
    hr = f->CreateFormatConverter(&conv);
    if (FAILED(hr)) return hr;
    hr = conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                          nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)nw;
    bi.bmiHeader.biHeight = -(LONG)nh;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb) return E_OUTOFMEMORY;

    hr = conv->CopyPixels(nullptr, nw * 4, nw * 4 * nh, (BYTE*)bits);
    if (FAILED(hr)) {
        DeleteObject(hb);
        return hr;
    }
    *out = hb;
    return S_OK;
}

// Decode JPEG/PNG bytes held in memory and turn them into a thumbnail bitmap.
static HRESULT ImageBytesToHBitmap(IWICImagingFactory* f, const uint8_t* d, size_t n, UINT cx, HBITMAP* out) {
    if (!d || n < 16) return E_FAIL;
    ComPtr<IWICStream> st;
    HRESULT hr = f->CreateStream(&st);
    if (FAILED(hr)) return hr;
    hr = st->InitializeFromMemory((BYTE*)d, (DWORD)n);
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapDecoder> dec;
    hr = f->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapFrameDecode> fr;
    hr = dec->GetFrame(0, &fr);
    if (FAILED(hr)) return hr;
    return SourceToHBitmap(f, fr.Get(), cx, out);
}

// ---------------------------------------------------------------- XMP thumbnail

static bool FindNeedle(IStream* s, uint64_t size, uint64_t from, const char* nd, size_t nl, uint64_t& found) {
    const ULONG CH = 1u << 20;
    std::vector<uint8_t> buf(CH);
    uint64_t pos = from;
    while (pos < size) {
        ULONG got = (ULONG)std::min<uint64_t>(CH, size - pos);
        if (got < nl) break;
        if (!ReadAt(s, pos, buf.data(), got)) return false;
        auto it = std::search(buf.begin(), buf.begin() + got, nd, nd + nl);
        if (it != buf.begin() + got) {
            found = pos + (uint64_t)(it - buf.begin());
            return true;
        }
        if (pos + got >= size) break;
        pos += got - (nl - 1);
    }
    return false;
}

static void Base64Decode(const uint8_t* s, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        int v;
        if (c == '&') {  // XML entity such as &#xA;
            while (i < n && s[i] != ';') i++;
            continue;
        }
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((acc >> bits) & 0xFF));
        }
    }
}

// Finds the next <xmpGImg:image>...</xmpGImg:image> block. Returns false when no more blocks.
static bool NextXmpJpeg(IStream* s, uint64_t size, uint64_t& from, std::vector<uint8_t>& jpeg) {
    static const char open[] = "<xmpGImg:image>";
    static const char close[] = "</xmpGImg:image>";
    jpeg.clear();
    uint64_t p = 0;
    if (!FindNeedle(s, size, from, open, sizeof(open) - 1, p)) return false;
    uint64_t start = p + sizeof(open) - 1;
    uint64_t take = std::min<uint64_t>(size - start, 4u << 20);
    std::vector<uint8_t> buf((size_t)take);
    if (!ReadAt(s, start, buf.data(), (ULONG)take)) {
        from = size;
        return false;
    }
    auto it = std::search(buf.begin(), buf.end(), close, close + sizeof(close) - 1);
    if (it == buf.end()) {
        from = start;
        return true;  // skip this candidate
    }
    from = start + (uint64_t)(it - buf.begin());
    Base64Decode(buf.data(), (size_t)(it - buf.begin()), jpeg);
    return true;
}

static HRESULT TryXmpThumb(IStream* s, uint64_t size, IWICImagingFactory* f, UINT cx, HBITMAP* out) {
    uint64_t from = 0;
    std::vector<uint8_t> jpeg;
    for (int i = 0; i < 8; i++) {
        if (!NextXmpJpeg(s, size, from, jpeg)) break;
        if (jpeg.empty()) continue;
        if (SUCCEEDED(ImageBytesToHBitmap(f, jpeg.data(), jpeg.size(), cx, out))) return S_OK;
    }
    return E_FAIL;
}

// ---------------------------------------------------------------- PSD / PSB

struct PsdInfo {
    uint16_t version = 0, channels = 0, depth = 0, mode = 0;
    uint32_t width = 0, height = 0;
    uint64_t resPos = 0, resLen = 0, imgPos = 0;
};

static bool ParsePsd(IStream* s, uint64_t size, PsdInfo& I) {
    uint8_t h[26];
    if (!ReadAt(s, 0, h, 26)) return false;
    if (memcmp(h, "8BPS", 4) != 0) return false;
    I.version = BE16(h + 4);
    if (I.version != 1 && I.version != 2) return false;
    I.channels = BE16(h + 12);
    I.height = BE32(h + 14);
    I.width = BE32(h + 18);
    I.depth = BE16(h + 22);
    I.mode = BE16(h + 24);

    uint64_t pos = 26;
    uint8_t b4[4], b8[8];
    if (!ReadAt(s, pos, b4, 4)) return false;
    pos += 4 + BE32(b4);  // color mode data

    if (!ReadAt(s, pos, b4, 4)) return false;
    I.resLen = BE32(b4);
    I.resPos = pos + 4;
    pos = I.resPos + I.resLen;

    if (I.version == 1) {
        if (!ReadAt(s, pos, b4, 4)) return false;
        pos += 4 + BE32(b4);
    } else {
        if (!ReadAt(s, pos, b8, 8)) return false;
        pos += 8 + BE64(b8);
    }
    I.imgPos = pos;
    return pos + 2 <= size;
}

// Image resource 1036: JPEG thumbnail
static bool PsdResourceJpeg(IStream* s, const PsdInfo& I, std::vector<uint8_t>& jpeg) {
    if (I.resLen == 0 || I.resLen > (128u << 20)) return false;
    std::vector<uint8_t> d((size_t)I.resLen);
    if (!ReadAt(s, I.resPos, d.data(), (ULONG)I.resLen)) return false;
    size_t n = d.size(), i = 0;
    while (i + 12 <= n) {
        if (memcmp(&d[i], "8BIM", 4) != 0) break;
        uint16_t id = BE16(&d[i + 4]);
        size_t p = i + 6;
        uint8_t nl = d[p];
        p += 1 + nl;
        if (((1 + nl) & 1) != 0) p++;
        if (p + 4 > n) break;
        uint32_t sz = BE32(&d[p]);
        p += 4;
        if (p + sz > n) break;
        if (id == 1036 && sz > 28 && BE32(&d[p]) == 1) {
            jpeg.assign(d.begin() + p + 28, d.begin() + p + sz);
            return true;
        }
        i = p + sz + (sz & 1);
    }
    return false;
}

static bool UnpackBits(const uint8_t* src, size_t sn, uint8_t* dst, size_t dn) {
    size_t i = 0, o = 0;
    while (o < dn && i < sn) {
        int8_t n = (int8_t)src[i++];
        if (n >= 0) {
            size_t c = (size_t)n + 1;
            c = std::min(c, std::min(sn - i, dn - o));
            memcpy(dst + o, src + i, c);
            i += c;
            o += c;
        } else if (n != -128) {
            size_t c = (size_t)(1 - n);
            if (i >= sn) break;
            uint8_t v = src[i++];
            c = std::min(c, dn - o);
            memset(dst + o, v, c);
            o += c;
        }
    }
    return o == dn;
}

// Merged (flattened) image data -> sub-sampled bitmap
static HRESULT PsdComposite(IStream* s, IWICImagingFactory* f, const PsdInfo& I, UINT cx, HBITMAP* out) {
    if (I.depth != 8 && I.depth != 16) return E_FAIL;
    int need = I.mode == 1 ? 1 : (I.mode == 3 ? 3 : (I.mode == 4 ? 4 : 0));
    if (!need || I.channels < need) return E_FAIL;
    uint32_t w = I.width, h = I.height;
    if (!w || !h) return E_FAIL;

    uint8_t c2[2];
    if (!ReadAt(s, I.imgPos, c2, 2)) return E_FAIL;
    uint16_t comp = BE16(c2);
    if (comp != 0 && comp != 1) return E_FAIL;  // ZIP not supported for merged data

    const size_t bps = I.depth / 8;
    const uint64_t rowBytes = (uint64_t)w * bps;
    const uint64_t base = I.imgPos + 2;

    std::vector<uint64_t> off;
    if (comp == 1) {
        size_t cb = (I.version == 2) ? 4 : 2;
        size_t cnt = (size_t)I.channels * h;
        std::vector<uint8_t> tbl(cnt * cb);
        if (!ReadAt(s, base, tbl.data(), (ULONG)tbl.size())) return E_FAIL;
        off.resize(cnt + 1);
        uint64_t cur = base + tbl.size();
        for (size_t i = 0; i < cnt; i++) {
            off[i] = cur;
            cur += (cb == 2) ? BE16(&tbl[i * cb]) : BE32(&tbl[i * cb]);
        }
        off[cnt] = cur;
    }

    uint32_t maxdim = std::max(w, h);
    uint32_t target = std::max<UINT>(cx, 32) * 2;
    uint32_t step = maxdim > target ? (maxdim + target - 1) / target : 1;
    uint32_t ow = (w + step - 1) / step, oh = (h + step - 1) / step;

    std::vector<uint8_t> packed;
    auto readRow = [&](uint32_t c, uint32_t r, uint8_t* dst) -> bool {
        if (comp == 0) return ReadAt(s, base + ((uint64_t)c * h + r) * rowBytes, dst, (ULONG)rowBytes);
        size_t idx = (size_t)c * h + r;
        uint64_t len = off[idx + 1] - off[idx];
        if (len == 0 || len > (64u << 20)) return false;
        packed.resize((size_t)len);
        if (!ReadAt(s, off[idx], packed.data(), (ULONG)len)) return false;
        return UnpackBits(packed.data(), (size_t)len, dst, (size_t)rowBytes);
    };

    std::vector<uint8_t> px((size_t)ow * oh * 4);
    std::vector<std::vector<uint8_t>> rows(need, std::vector<uint8_t>((size_t)rowBytes));
    for (uint32_t oy = 0; oy < oh; oy++) {
        uint32_t r = std::min(oy * step, h - 1);
        for (int c = 0; c < need; c++)
            if (!readRow((uint32_t)c, r, rows[c].data())) return E_FAIL;
        for (uint32_t ox = 0; ox < ow; ox++) {
            uint32_t x = std::min(ox * step, w - 1);
            uint8_t v[4] = {0, 0, 0, 0};
            for (int c = 0; c < need; c++) v[c] = rows[c][(size_t)x * bps];
            uint8_t* p = &px[((size_t)oy * ow + ox) * 4];
            if (I.mode == 1) {
                p[0] = p[1] = p[2] = v[0];
            } else if (I.mode == 3) {
                p[0] = v[2]; p[1] = v[1]; p[2] = v[0];
            } else {  // CMYK, stored inverted in PSD
                p[0] = (uint8_t)(v[2] * v[3] / 255);
                p[1] = (uint8_t)(v[1] * v[3] / 255);
                p[2] = (uint8_t)(v[0] * v[3] / 255);
            }
            p[3] = 255;
        }
    }

    ComPtr<IWICBitmap> bmp;
    HRESULT hr = f->CreateBitmapFromMemory(ow, oh, GUID_WICPixelFormat32bppBGR, ow * 4,
                                           (UINT)px.size(), px.data(), &bmp);
    if (FAILED(hr)) return hr;
    return SourceToHBitmap(f, bmp.Get(), cx, out);
}

// ---------------------------------------------------------------- provider

class ThumbProvider : public IInitializeWithStream, public IThumbnailProvider {
public:
    ThumbProvider() { InterlockedIncrement(&g_objects); }
    ~ThumbProvider() { InterlockedDecrement(&g_objects); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IInitializeWithStream)
            *ppv = static_cast<IInitializeWithStream*>(this);
        else if (riid == IID_IThumbnailProvider)
            *ppv = static_cast<IThumbnailProvider*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP Initialize(IStream* pstm, DWORD) override {
        if (m_stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        m_stream = pstm;
        return S_OK;
    }

    STDMETHODIMP GetThumbnail(UINT cx, HBITMAP* ph, WTS_ALPHATYPE* pa) override {
        if (!ph || !pa) return E_POINTER;
        *ph = nullptr;
        *pa = WTSAT_UNKNOWN;
        if (!m_stream) return E_UNEXPECTED;
        if (cx == 0) cx = 256;

        ComPtr<IWICImagingFactory> f;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        if (FAILED(hr)) return hr;

        IStream* s = m_stream.Get();
        uint64_t size = StreamSize(s);
        if (size < 16) return E_FAIL;

        HBITMAP hb = nullptr;
        PsdInfo I;
        if (ParsePsd(s, size, I)) {
            std::vector<uint8_t> jpeg;
            if (PsdResourceJpeg(s, I, jpeg))
                ImageBytesToHBitmap(f.Get(), jpeg.data(), jpeg.size(), cx, &hb);
            if (!hb) PsdComposite(s, f.Get(), I, cx, &hb);
        }
        if (!hb) TryXmpThumb(s, size, f.Get(), cx, &hb);
        if (!hb) return E_FAIL;

        *ph = hb;
        *pa = WTSAT_ARGB;
        return S_OK;
    }

private:
    LONG m_ref = 1;
    ComPtr<IStream> m_stream;
};

class Factory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        ThumbProvider* p = new (std::nothrow) ThumbProvider();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_objects);
        else InterlockedDecrement(&g_objects);
        return S_OK;
    }

private:
    LONG m_ref = 1;
};

// ---------------------------------------------------------------- DLL exports / registration

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_PsAiThumb) return CLASS_E_CLASSNOTAVAILABLE;
    Factory* f = new (std::nothrow) Factory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return g_objects == 0 ? S_OK : S_FALSE; }

static const wchar_t* kThumbIID = L"{E357FCCD-A995-4576-B01F-234630154E96}";

static LONG SetStr(const std::wstring& key, const wchar_t* name, const std::wstring& val) {
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), name, REG_SZ, val.c_str(),
                           (DWORD)((val.size() + 1) * sizeof(wchar_t)));
}

static std::wstring ClsidStr() {
    wchar_t buf[64];
    StringFromGUID2(CLSID_PsAiThumb, buf, 64);
    return buf;
}

static bool ReadStr(HKEY root, const std::wstring& key, const wchar_t* name, std::wstring& out) {
    wchar_t buf[512];
    DWORD cb = sizeof(buf);
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &cb) != ERROR_SUCCESS) return false;
    out = buf;
    return true;
}

static void SetHandler(const std::wstring& classKey, bool force) {
    std::wstring sub = classKey + L"\\ShellEx\\" + kThumbIID;
    if (!force) {
        wchar_t t[4];
        DWORD cb = sizeof(t);
        LONG r = RegGetValueW(HKEY_CLASSES_ROOT, sub.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, t, &cb);
        if (r == ERROR_SUCCESS || r == ERROR_MORE_DATA) return;  // someone else already handles it
    }
    SetStr(L"Software\\Classes\\" + sub, nullptr, ClsidStr());
}

static void RemoveHandler(const std::wstring& classKey) {
    std::wstring sub = L"Software\\Classes\\" + classKey + L"\\ShellEx\\" + kThumbIID;
    std::wstring cur;
    if (ReadStr(HKEY_LOCAL_MACHINE, sub, nullptr, cur) && _wcsicmp(cur.c_str(), ClsidStr().c_str()) == 0)
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, sub.c_str());
}

struct ExtEntry { const wchar_t* ext; bool force; };
static const ExtEntry kExts[] = {
    {L".psd", true},  {L".psb", true},  {L".pdd", true},  {L".psdt", true},
    {L".ai", true},   {L".ait", true},  {L".indd", true}, {L".indt", true},
    {L".eps", false},  // only if no other handler exists
};

static std::vector<std::wstring> ProgIdsFor(const wchar_t* ext) {
    std::vector<std::wstring> r;
    std::wstring p;
    if (ReadStr(HKEY_CLASSES_ROOT, ext, nullptr, p) && !p.empty()) r.push_back(p);
    std::wstring uc = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\") + ext + L"\\UserChoice";
    if (ReadStr(HKEY_CURRENT_USER, uc, L"ProgId", p) && !p.empty()) r.push_back(p);
    return r;
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_hInst, path, MAX_PATH);
    std::wstring base = L"Software\\Classes\\CLSID\\" + ClsidStr();
    if (SetStr(base, nullptr, L"PSD/AI Thumbnail Provider") != ERROR_SUCCESS) return SELFREG_E_CLASS;
    SetStr(base + L"\\InprocServer32", nullptr, path);
    SetStr(base + L"\\InprocServer32", L"ThreadingModel", L"Apartment");

    for (const auto& e : kExts) {
        SetHandler(e.ext, e.force);
        for (const auto& pid : ProgIdsFor(e.ext)) SetHandler(pid, e.force);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    for (const auto& e : kExts) {
        RemoveHandler(e.ext);
        for (const auto& pid : ProgIdsFor(e.ext)) RemoveHandler(pid);
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (L"Software\\Classes\\CLSID\\" + ClsidStr()).c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
