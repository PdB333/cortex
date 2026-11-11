// Generate metadata-only MSFT type libraries with Microsoft's authoring API.
// This creates a test file; no COM server, target DLL or type-library member
// is activated, registered or executed. Link oleaut32, ole32 and uuid.
#include <windows.h>
#include <oaidl.h>
#include <oleauto.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

static void Need(HRESULT hr, const char* where) {
    if (FAILED(hr)) { std::cerr << where << " HRESULT=" << std::hex << static_cast<unsigned long>(hr) << '\n'; throw std::runtime_error(where); }
}
static GUID Guid(const wchar_t* text) { GUID result{}; Need(CLSIDFromString(text, &result), "CLSIDFromString"); return result; }
static void Build(const std::filesystem::path& path, SYSKIND system) {
    ICreateTypeLib2* lib = nullptr;
    Need(CreateTypeLib2(system, path.wstring().c_str(), &lib), "CreateTypeLib2");
    Need(lib->SetName(L"CortexFixture"), "SetName");
    Need(lib->SetGuid(Guid(L"{12345678-1122-3344-5566-778899AABBCC}")), "SetGuid");
    Need(lib->SetLcid(0x409), "SetLcid"); Need(lib->SetVersion(1, 2), "SetVersion");
    ICreateTypeInfo* type = nullptr;
    Need(lib->CreateTypeInfo(L"ICortex", TKIND_INTERFACE, &type), "CreateTypeInfo interface");
    Need(type->SetGuid(Guid(L"{22345678-1122-3344-5566-778899AABBCC}")), "interface guid");
    Need(type->SetTypeFlags(TYPEFLAG_FOLEAUTOMATION), "interface flags");
    Need(type->SetVersion(2, 3), "interface version");
    ELEMDESC parameter{}; parameter.tdesc.vt = VT_I4; parameter.paramdesc.wParamFlags = PARAMFLAG_FIN;
    FUNCDESC function{}; function.memid = 7; function.funckind = FUNC_PUREVIRTUAL;
    function.invkind = INVOKE_FUNC; function.callconv = CC_STDCALL; function.cParams = 1;
    function.elemdescFunc.tdesc.vt = VT_HRESULT; function.lprgelemdescParam = &parameter;
    Need(type->AddFuncDesc(0, &function), "AddFuncDesc");
    LPOLESTR names[] = {const_cast<LPOLESTR>(L"Ping"), const_cast<LPOLESTR>(L"value")};
    Need(type->SetFuncAndParamNames(0, names, 2), "SetFuncAndParamNames");
    Need(type->LayOut(), "interface LayOut"); type->Release(); type = nullptr;
    Need(lib->CreateTypeInfo(L"Mode", TKIND_ENUM, &type), "CreateTypeInfo enum");
    Need(type->SetGuid(Guid(L"{32345678-1122-3344-5566-778899AABBCC}")), "enum guid");
    VARIANT value; VariantInit(&value); value.vt = VT_I4; value.lVal = 42;
    VARDESC var{}; var.memid = 100; var.varkind = VAR_CONST; var.elemdescVar.tdesc.vt = VT_I4; var.lpvarValue = &value;
    Need(type->AddVarDesc(0, &var), "AddVarDesc"); Need(type->SetVarName(0, L"Answer"), "SetVarName");
    Need(type->LayOut(), "enum LayOut"); type->Release();
    Need(lib->SaveAllChanges(), "SaveAllChanges"); lib->Release();
    std::cout << path.string() << ": library CortexFixture 1.2, ICortex 2.3, Ping(value:I4):HRESULT, Mode.Answer:I4=42, syskind=" << system << '\n';
}
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: make_typelib_fixture OUTPUT_DIRECTORY\n"; return 2; }
    try {
        const auto directory = std::filesystem::u8path(argv[1]); std::filesystem::create_directories(directory);
        Need(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "CoInitializeEx");
        Build(directory / "microsoft-win32.tlb", SYS_WIN32);
        Build(directory / "microsoft-win64.tlb", SYS_WIN64);
        CoUninitialize(); return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; CoUninitialize(); return 1; }
}
