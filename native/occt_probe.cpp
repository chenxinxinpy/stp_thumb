// 探针：验证 OCCT 8.0 能否读 STEP 并给出三角网格
#include <STEPControl_Reader.hxx>
#include <DESTEP_Parameters.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopLoc_Location.hxx>
#include <Poly_Triangulation.hxx>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <chrono>
#include <string>

static std::string readAll(const char* p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("probe <file.step>\n"); return 2; }
    auto t0 = std::chrono::steady_clock::now();
    std::string text = readAll(argv[1]);
    std::printf("bytes=%zu\n", text.size());

    STEPControl_Reader reader;
    std::istringstream in(text);
    DESTEP_Parameters prm;
    const bool lean = (argc > 2);
    if (lean) {
        prm.ReadColor = false; prm.ReadName = false; prm.ReadLayer = false;
        prm.ReadProps = false; prm.ReadMetadata = false; prm.ReadProductMetadata = false;
        prm.ReadSameParamMode = false; prm.ReadSubshapeNames = false;
    }
    auto tr0 = std::chrono::steady_clock::now();
    IFSelect_ReturnStatus st = lean ? reader.ReadStream("memory.step", prm, in)
                                    : reader.ReadStream("memory.step", in);
    int nroots = 0; TopoDS_Shape shape;
    if (st == IFSelect_RetDone) { nroots = reader.TransferRoots(); shape = reader.OneShape(); }
    auto tr1 = std::chrono::steady_clock::now();
    std::printf("lean=%d readTotal=%lldms\n", (int)lean,
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(tr1-tr0).count());
    std::printf("ReadStream=%d\n", (int)st);
    if (st != IFSelect_RetDone) return 3;
    std::printf("roots=%d null=%d\n", nroots, (int)shape.IsNull());
    if (shape.IsNull()) return 4;

    Bnd_Box box; BRepBndLib::Add(shape, box);
    double x1,y1,z1,x2,y2,z2; box.Get(x1,y1,z1,x2,y2,z2);
    std::printf("bbox=%.2f x %.2f x %.2f\n", x2-x1, y2-y1, z2-z1);

    double diag = std::sqrt((x2-x1)*(x2-x1)+(y2-y1)*(y2-y1)+(z2-z1)*(z2-z1));
    double defl = diag * 0.0015;
    auto t1 = std::chrono::steady_clock::now();
    BRepMesh_IncrementalMesh mesher(shape, defl, Standard_False, 0.30, Standard_True);
    auto t2 = std::chrono::steady_clock::now();

    int faces = 0, tri3 = 0, noTri = 0, withNormals = 0, verts = 0;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
        ++faces;
        TopoDS_Face f = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tr = BRep_Tool::Triangulation(f, loc);
        if (tr.IsNull()) { ++noTri; continue; }
        tri3 += tr->NbTriangles();
        verts += tr->NbNodes();
        if (tr->HasNormals()) ++withNormals;
    }
    std::printf("faces=%d noTri=%d tris=%d verts=%d normals=%d\n", faces, noTri, tri3, verts, withNormals);
    auto ms = [](auto a, auto b){ return std::chrono::duration_cast<std::chrono::milliseconds>(b-a).count(); };
    std::printf("read=%lldms mesh=%lldms total=%lldms deflection=%.4f\n",
                (long long)ms(t0,t1), (long long)ms(t1,t2), (long long)ms(t0,t2), defl);
    return 0;
}
