#include "../mcp_bridge/test_code_knowledge.h"
#include "../mcp_bridge/test_file_version.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using nlohmann::json;

static int errors = 0;
static void Check(bool ok, const char* label) {
    if (!ok) { std::cerr << "FAIL: " << label << '\n'; ++errors; }
}

int main() {
    const std::string digest(64, 'a');
    const json version = {{"status","hashed"},{"sha256",digest},
                          {"file_size",uint64_t{12345}}};
    int hashes = 0;
    const cortex::test::code_knowledge::FileVersion finger =
        [&](const std::string& file) -> json {
            ++hashes;
            return file == "C:/Fixture/Game.exe" ? version :
                json{{"status","unavailable"}};
        };
    const json mod = {{"name","Game.exe"},{"path","C:/Fixture/Game.exe"},
                      {"base","0x140000000"},{"size",uint64_t{65536}},
                      {"rva","0x3010"},{"identity","live_module_at_lookup"}};
    json next = mod;
    next["rva"] = "0x3011";
    const json context = {
        {"ok",true},{"test_id","test_2026_001"},
        {"locations",json::array({
            {{"address","0x140003010"},{"address_role","debugger_instruction_pointer"},
             {"sampled_events",2},{"module",mod}},
            {{"address","0x140003011"},{"address_role","debugger_instruction_pointer"},
             {"sampled_events",1},{"module",next}},
            {{"address","0x5000"},{"address_role","data_watch_address"},
             {"sampled_events",1},{"module",nullptr}}
        })}
    };
    const json found = cortex::test::code_knowledge::Capture(context,finger);
    Check(found.value("ok",false), "observed context captured");
    if (!found.value("ok",false)) {
        std::cerr << found.dump(2); return 1;
    }
    const json refs = found["references"];
    Check(refs.size()==2 && found["skipped"]==1, "ignore data IP and store two RVAs");
    Check(hashes==1, "one file hash per module");
    Check(refs[0]["rva"]=="0x3010" && refs[0]["source"]=="debugger_observation",
          "provenance and module offset");
    Check(refs[0]["file_sha256"]==digest && refs[0]["file_size"]==12345,
          "on-disk SHA and file size persisted");
    Check(!refs[0].contains("address") && !refs[0].contains("base") &&
          !refs[0].contains("path"), "no stale absolute address or path");
    const json current = json::array({{
        {"name","Game.exe"},{"path","C:/Fixture/Game.exe"},
        {"base",uint64_t{0x180000000}},{"size",uint64_t{65536}}
    }});
    hashes = 0;
    const json reused = cortex::test::code_knowledge::Revalidate(refs,current,finger);
    Check(reused.value("ok",false) && reused["matched"]==2,
          "identical module version after restart");
    Check(reused["results"][0]["address"]=="0x180003010",
          "new address from current ASLR base");
    Check(hashes==1, "revalidation fingerprint memoized");
    const cortex::test::code_knowledge::FileVersion changed =
        [](const std::string&)->json {
            return {{"status","hashed"},{"sha256",std::string(64,'b')},
                    {"file_size",uint64_t{12345}}};
        };
    const json different = cortex::test::code_knowledge::Revalidate(refs,current,changed);
    Check(different["matched"]==0 &&
          different["results"][0]["status"]=="binary_changed" &&
          !different["results"][0].contains("address"),"no address if binary changed");
    json duplicates=current;
    duplicates.push_back(current[0]);
    const json ambiguous=cortex::test::code_knowledge::Revalidate(refs,duplicates,finger);
    Check(ambiguous["results"][0]["status"]=="ambiguous_module" &&
          !ambiguous["results"][0].contains("address"),"multiple module names refused");
    json shorter=current;
    shorter[0]["size"]=10;
    const json outside=cortex::test::code_knowledge::Revalidate(refs,shorter,finger);
    Check(outside["results"][0]["status"]=="rva_outside_module" &&
          !outside["results"][0].contains("address"),"offset within current image");
    const json gone=cortex::test::code_knowledge::Revalidate(refs,json::array(),finger);
    Check(gone["results"][0]["status"]=="module_not_loaded", "missing module explicit");
    const cortex::test::code_knowledge::FileVersion unreadable=
        [](const std::string&)->json {return {{"status","unavailable"}};};
    const json noHash=cortex::test::code_knowledge::Revalidate(refs,current,unreadable);
    Check(noHash["results"][0]["status"]=="file_version_unavailable" &&
          !noHash["results"][0].contains("address"),"unreadable binary refused");
    json spoof=refs;spoof[0]["source"]="client_guess";
    Check(!cortex::test::code_knowledge::Revalidate(spoof,current,finger)
        .value("ok",true), "corrupted provenance rejected");

    json claim,updated;
    std::string error;
    const json input={{"id","Player.health"},{"kind","field"},
                      {"statement","Possible code relation"},{"status","hypothesis"}};
    Check(project::knowledge::Prepare(input,nullptr,claim,error), "new knowledge claim");
    Check(project::knowledge::AppendCodeReferences(claim,refs,error),
          "append measured references");
    json change=input;change["expected_revision"]=1;
    Check(project::knowledge::Prepare(change,&claim,updated,error),
          "claim revision preserved");
    Check(updated["code_references"]==refs && updated["status"]=="hypothesis",
          "code reference survives ordinary user revision");
    json forged=input;forged["code_references"]=refs;
    Check(!project::knowledge::Validate(forged,error),
          "client cannot create server-owned code evidence");
    Check(!project::knowledge::AppendCodeReferences(claim,refs,error) &&
          error=="code_reference_already_linked","duplicate rejected");

    const auto path=std::filesystem::temp_directory_path() /
        "cortex_code_knowledge_hash_test.bin";
    {std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<"abc";}
    const json real=cortex::test::file_version::Hash(path.u8string());
    Check(real.value("sha256",std::string())==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" &&
          real.value("file_size",uint64_t{0})==3,
          "real BCrypt SHA-256 hashes whole file");
    Check(cortex::test::file_version::Hash("relative.file")
          .value("status",std::string())=="unavailable",
          "reject nonabsolute file");
    std::error_code ec;std::filesystem::remove(path,ec);
    if(errors) return 1;
    std::cout<<"PASS: observed RVAs persist with full SHA-256 revalidation\n";
}
