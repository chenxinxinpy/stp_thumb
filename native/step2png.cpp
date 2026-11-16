// step2png.cpp - 独立进程验证工具：STEP -> PNG
// 用于在把 DLL 注册进 Explorer 之前，先隔离验证解析与渲染不会崩溃。
#include "step_core.h"
#ifdef USE_OCCT
#include "step_occt.h"
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdio.h>
#include <wchar.h>

// ------------------------------------------------------- 最小 PNG 编码器（stored deflate）
static uint32_t crcTable[256];
static void initCrc() {
    for (uint32_t i=0;i<256;++i) {
        uint32_t c=i;
        for (int k=0;k<8;++k) c = (c&1) ? (0xEDB88320u ^ (c>>1)) : (c>>1);
        crcTable[i]=c;
    }
}
static uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c=0xFFFFFFFFu;
    for (size_t i=0;i<n;++i) c = crcTable[(c ^ p[i]) & 0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
static uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t a=1,b=0;
    const size_t chunk=5552;
    size_t i=0;
    while (i<n) {
        size_t m=std::min(chunk,n-i);
        for (size_t k=0;k<m;++k) { a+=p[i+k]; b+=a; }
        a%=65521; b%=65521; i+=m;
    }
    return (b<<16)|a;
}
static void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x>>24)); v.push_back((uint8_t)(x>>16));
    v.push_back((uint8_t)(x>>8));  v.push_back((uint8_t)x);
}
static void chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
    put32(png,(uint32_t)data.size());
    const uint8_t* t=(const uint8_t*)type;
    png.insert(png.end(),t,t+4);
    png.insert(png.end(),data.begin(),data.end());
    std::vector<uint8_t> c(t,t+4);
    c.insert(c.end(),data.begin(),data.end());
    put32(png,crc32(c.data(),c.size()));
}
static bool writePNG(const std::wstring& path, const step::Image& img) {
    if (img.empty()) return false;
    initCrc();
    // RGB8 scanlines，每行首字节为 filter 0
    std::vector<uint8_t> raw;
    raw.reserve((size_t)img.h*(img.w*3+1));
    for (int y=0;y<img.h;++y) {
        raw.push_back(0);
        const uint8_t* row=img.bgra.data()+(size_t)y*img.w*4;
        for (int x=0;x<img.w;++x) {
            raw.push_back(row[x*4+2]);  // R
            raw.push_back(row[x*4+1]);  // G
            raw.push_back(row[x*4+0]);  // B
        }
    }
    // zlib: 0x78 0x01 + stored deflate blocks + adler32
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    size_t pos=0;
    while (pos<raw.size()) {
        size_t m=std::min((size_t)65535, raw.size()-pos);
        bool last=(pos+m>=raw.size());
        z.push_back(last?1:0);
        z.push_back((uint8_t)(m&0xFF)); z.push_back((uint8_t)(m>>8));
        uint16_t nn=(uint16_t)~m;
        z.push_back((uint8_t)(nn&0xFF)); z.push_back((uint8_t)(nn>>8));
        z.insert(z.end(),raw.begin()+pos,raw.begin()+pos+m);
        pos+=m;
    }
    uint32_t ad=adler32(raw.data(),raw.size());
    put32(z,ad);

    std::vector<uint8_t> ihdr;
    put32(ihdr,(uint32_t)img.w); put32(ihdr,(uint32_t)img.h);
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);

    std::vector<uint8_t> png={0x89,'P','N','G','\r','\n',0x1A,'\n'};
    chunk(png,"IHDR",ihdr);
    chunk(png,"IDAT",z);
    chunk(png,"IEND",std::vector<uint8_t>());
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
    fclose(f);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("step2png <input.step> <output.png> [size=512] [wireframe|solid|solid+edges]\n");
        return 2;
    }
    int size = (argc>=4) ? (int)wcstol(argv[3], nullptr, 10) : 512;
    bool solid = false, edges = true;
    if (argc>=5) {
        if (std::wcscmp(argv[4],L"solid")==0)            { solid=true;  edges=false; }
        else if (std::wcscmp(argv[4],L"solid+edges")==0) { solid=true;  edges=true;  }
    }
    auto t0=std::chrono::steady_clock::now();
    std::string text;
    if (!step::readFile(argv[1],text)) { std::printf("FAIL: cannot read input\n"); return 3; }
    step::Options opt;
    // 与 StepThumb.dll 保持一致的按尺寸分档，便于用 CLI 复现缩略图/预览效果
    opt.maxTris = (size <= 300) ? 30000 : (size <= 700 ? 70000 : 150000);
    opt.angle   = (size <= 300) ? 0.45 : (size <= 700 ? 0.30 : 0.22);
    if (const wchar_t* e = _wgetenv(L"STEPTHUMB_TRIS"))  opt.maxTris = (int)wcstol(e, nullptr, 10);
    if (const wchar_t* e = _wgetenv(L"STEPTHUMB_ANGLE")) opt.angle   = wcstod(e, nullptr);
    opt.solid=solid;
    step::Mesh mesh;
    const wchar_t* engine = _wgetenv(L"STEPTHUMB_ENGINE");
    bool wantNative = (engine && std::wcscmp(engine, L"native")==0);
    bool usedOCCT = false;
#ifdef USE_OCCT
    if (!wantNative) usedOCCT = step::buildMeshOCCT(text, opt, mesh);
#endif
    if (!usedOCCT) mesh = step::buildMesh(text,opt);
    if (mesh.empty()) { std::printf("FAIL: no geometry extracted\n"); return 4; }
    auto tMesh=std::chrono::steady_clock::now();
    mesh.normalize();
    step::Image img=step::render(mesh,size,0x00FAFBFD,edges,usedOCCT);
    auto tRender=std::chrono::steady_clock::now();
    if (!writePNG(argv[2],img)) { std::printf("FAIL: cannot write output\n"); return 5; }
    auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
    std::printf("PHASE mesh=%lldms render=%lldms\n",
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(tMesh-t0).count(),
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(tRender-tMesh).count());
    std::printf("OK engine=%s entities=%zu faces=%zu loops=%zu tris=%zu lines=%zu bbox=%.2fx%.2fx%.2f mm size=%d time=%lldms\n",
        usedOCCT?"occt":"native",
        mesh.stats.entities,mesh.stats.faces,mesh.stats.loops,mesh.tris.size(),mesh.lines.size(),
        mesh.stats.sx,mesh.stats.sy,mesh.stats.sz,size,(long long)ms);
    return 0;
}
