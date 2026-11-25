// Links real release executables with zodiac::cli::link_release from the kit layout of the build tree (lib/release, modules/static, bin/ld.lld.exe,
// staged by zodiac_kit_stage) and runs them, so the captured link line, the release stub's wWinMain, start_linked's zodiac_payload_end check and the
// console binding of a GUI-subsystem program started with piped output are all exercised on a real executable. Windows only (see tests/CMakeLists.txt).
#include <cli/linker.hpp>
#include <core/coff_object.hpp>
#include <core/diagnostics.hpp>
#include <core/kit_layout.hpp>
#include <core/payload.hpp>
#include <core/project_file.hpp>
#include <core/script_runtime.hpp>
#include <doctest/doctest.h>
#include <platform/console.hpp>
#include <platform/process.hpp>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
namespace fs=std::filesystem;
const int g_dialogs_disabled=(zodiac::platform::disable_error_dialogs(),0);
fs::path from_utf8(std::string_view text)
{
return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),text.size()));
}
std::string to_utf8(const fs::path& path)
{
const std::u8string text=path.generic_u8string();
return std::string(reinterpret_cast<const char*>(text.data()),text.size());
}
// A fresh directory whose name contains a space and a non-ASCII letter, removed on destruction.
class TempDir{
public:
TempDir()
{
static std::atomic<unsigned> counter{0};
dir_=fs::temp_directory_path()/from_utf8("zodiac linker \xC5\x9F "+std::to_string(counter.fetch_add(1))+"_"+std::to_string(reinterpret_cast<std::uintptr_t>(this)));
fs::create_directories(dir_);
}
~TempDir()
{
std::error_code ec;
fs::remove_all(dir_,ec);
}
TempDir(const TempDir&)=delete;
TempDir& operator=(const TempDir&)=delete;
const fs::path& dir() const{return dir_;}
private:
fs::path dir_;
};
void write_file(const fs::path& path,std::string_view text)
{
std::ofstream out(path,std::ios::binary|std::ios::trunc);
out.write(text.data(),static_cast<std::streamsize>(text.size()));
REQUIRE(out.good());
}
class CollectSink final:public zodiac::DiagnosticSink{
public:
explicit CollectSink(std::vector<std::string>& lines):lines_(lines){}
void write(zodiac_log_level,std::string_view line) noexcept override{lines_.emplace_back(line);}
private:
std::vector<std::string>& lines_;
};
std::string join(const std::vector<std::string>& lines)
{
std::string text;
for(const std::string& line:lines)text.append(line).push_back('\n');
return text;
}
zodiac::KitLayout build_kit(){return zodiac::kit_layout(from_utf8(ZODIAC_TEST_KIT_EXE));}
// Builds the project in dir like zodiac build will: requested names in load order, the registered modules with their versions and link modes, the main script's bytecode.
std::string payload_of(const fs::path& dir)
{
auto project=zodiac::load_project(dir);
INFO(project.error_or(""));
REQUIRE(project.has_value());
zodiac::Diagnostics diagnostics;
zodiac::ScriptRuntime runtime(diagnostics,zodiac::ModuleSearchPaths{{from_utf8(ZODIAC_TEST_MODULE_DIR)}});
REQUIRE(runtime.build(*project)==0);
auto bytecode=runtime.save_bytecode(false);
INFO(bytecode.error_or(""));
REQUIRE(bytecode.has_value());
zodiac::GamePayload payload;
payload.kit_version="0.1.0";
payload.game_name=project->name;
payload.requested=project->static_modules;
for(const std::string& name:project->dynamic_modules)if(std::ranges::find(payload.requested,name)==payload.requested.end())payload.requested.push_back(name);
for(const zodiac_module_info* info:runtime.modules()){
const bool is_static=std::ranges::find(project->static_modules,std::string_view(info->name))!=project->static_modules.end();
payload.modules.push_back({info->name,info->version,is_static?zodiac::LinkMode::static_link:zodiac::LinkMode::dynamic_link});
}
payload.core_exclude=project->core_exclude;
payload.scripts.push_back({to_utf8(project->main_script.lexically_relative(project->root)),true,std::move(*bytecode)});
return zodiac::serialize_payload(payload);
}
struct Linked{
std::expected<void,std::string> result;
std::vector<std::string> lines;
};
// Links folder/game.exe from the payload's object, the release stub, the given static modules, the release core and AngelScript, in the order zodiac build uses.
Linked link_game(const fs::path& folder,std::string_view payload,const std::vector<std::string>& static_modules,const std::vector<fs::path>& extra_inputs={})
{
const zodiac::KitLayout kit=build_kit();
const fs::path object=folder/"game.zodpay.obj";
write_file(object,zodiac::write_payload_object(payload));
zodiac::cli::LinkRequest request;
request.output=folder/"game.exe";
request.inputs.push_back(object);
request.inputs.push_back(kit.release_lib/"zodiac_manifest.obj");
request.inputs.push_back(kit.release_lib/"libzodiac_release_stub.a");
for(const std::string& name:static_modules){
request.inputs.push_back(kit.static_modules/("zodiac_mod_"+name+".a"));
request.forced_symbols.push_back("zodiac_static_module_"+name);
}
request.inputs.push_back(kit.release_lib/"libzodiac_core_release.a");
request.inputs.push_back(kit.release_lib/"libangelscript.a");
request.inputs.insert(request.inputs.end(),extra_inputs.begin(),extra_inputs.end());
Linked linked;
zodiac::Diagnostics diagnostics;
diagnostics.add_sink(std::make_unique<CollectSink>(linked.lines));
linked.result=zodiac::cli::link_release(kit,request,diagnostics);
return linked;
}
// The Subsystem field of a PE image's optional header: 2 is the GUI subsystem, 3 the console one; 0 when the file is not a PE image.
unsigned pe_subsystem(const fs::path& exe)
{
std::ifstream in(exe,std::ios::binary);
const std::string image((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
const auto byte=[&image](std::size_t at){return at<image.size()?static_cast<unsigned>(static_cast<unsigned char>(image[at])):0u;};
if(byte(0)!='M'||byte(1)!='Z')return 0;
const std::size_t pe=byte(0x3C)|(byte(0x3D)<<8)|(byte(0x3E)<<16)|(byte(0x3F)<<24);
if(byte(pe)!='P'||byte(pe+1)!='E'||byte(pe+2)!=0||byte(pe+3)!=0)return 0;
// Signature (4 bytes) + file header (20 bytes), then the optional header, whose Subsystem field is at offset 68 in PE32 and PE32+ alike.
const std::size_t subsystem=pe+4+20+68;
return byte(subsystem)|(byte(subsystem+1)<<8);
}
zodiac::platform::ProcessResult run_exe(const fs::path& exe)
{
const std::vector<std::string> argv{to_utf8(exe)};
auto result=zodiac::platform::run_process(argv);
INFO(result.error_or(""));
REQUIRE(result.has_value());
return std::move(*result);
}
}

TEST_CASE("the kit's release folder holds the captured link line and the toolchain identity")
{
const zodiac::KitLayout kit=build_kit();
CHECK(fs::is_regular_file(kit.bin/"ld.lld.exe"));
CHECK(fs::is_regular_file(kit.release_lib/"libzodiac_release_stub.a"));
CHECK(fs::is_regular_file(kit.release_lib/"libzodiac_core_release.a"));
CHECK(fs::is_regular_file(kit.release_lib/"libangelscript.a"));
CHECK(fs::is_regular_file(kit.release_lib/"zodiac_manifest.obj"));
CHECK(fs::is_regular_file(kit.release_lib/"crt"/"crt2u.o"));
CHECK(fs::is_regular_file(kit.static_modules/"zodiac_mod_example.a"));
std::ifstream toolchain(kit.release_lib/"toolchain.txt",std::ios::binary);
const std::string identity((std::istreambuf_iterator<char>(toolchain)),std::istreambuf_iterator<char>());
CHECK(identity=="llvm-mingw 20261104 ucrt x86_64");
std::ifstream stamp(kit.static_modules/"zodiac_mod_example.stamp",std::ios::binary);
CHECK(std::string((std::istreambuf_iterator<char>(stamp)),std::istreambuf_iterator<char>())==identity);
std::ifstream link(kit.release_lib/"link.txt",std::ios::binary);
std::vector<std::string> lines;
for(std::string line;std::getline(link,line);)lines.push_back(line);
INFO(join(lines));
CHECK(std::ranges::count(lines,"{inputs}")==1);
CHECK(std::ranges::count(lines,"{output}")==1);
CHECK(std::ranges::count(lines,"{release_lib}/crt/crt2u.o")==1);
// The release link flags games get (cmake/ZodiacReleaseFlags.cmake): safe ICF, gc, strip and full LTO at -O3.
CHECK(std::ranges::count(lines,"--icf=safe")==1);
CHECK(std::ranges::count(lines,"--gc-sections")==1);
CHECK(std::ranges::count(lines,"-plugin-opt=O3")==1);
CHECK(std::ranges::count(lines,"-plugin-opt=thinlto")==0);
// Relocatable: no absolute path of the toolchain the kit was built with survives.
for(const std::string& line:lines){
CHECK(line.find(':')==std::string::npos);
CHECK(!line.starts_with("-L"));
}
}

TEST_CASE("link_release links a release executable that runs its payload")
{
const TempDir folder;
const std::string payload=payload_of(from_utf8(ZODIAC_TEST_SCRIPT_DIR)/"int_main");
const Linked linked=link_game(folder.dir(),payload,{});
INFO(join(linked.lines));
REQUIRE(linked.result.has_value());
CHECK(linked.lines.empty());
// The response file is removed after the link.
CHECK(!fs::exists(folder.dir()/"game.exe.rsp"));
// -mwindows: a GUI-subsystem executable, so its entry point is the release stub's wWinMain (pulled from the stub archive by the CRT's reference).
CHECK(pe_subsystem(folder.dir()/"game.exe")==2);
// The application manifest is linked in (its resource holds the manifest's bytes as written): the Segment Heap.
{
std::ifstream in(folder.dir()/"game.exe",std::ios::binary);
const std::string image((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
CHECK(image.find("<heapType xmlns=\"http://schemas.microsoft.com/SMI/2020/WindowsSettings\">SegmentHeap</heapType>")!=std::string::npos);
// And the user's own rights, whatever the game is called (installer detection would otherwise look at names such as setup).
CHECK(image.find("<requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\"/>")!=std::string::npos);
}
// wWinMain hands [zodiac_payload, zodiac_payload_end) to start_linked: any other end than just past the data is a size mismatch (exit 2).
const zodiac::platform::ProcessResult ran=run_exe(folder.dir()/"game.exe");
CHECK(ran.output=="");
CHECK(ran.exit_code==7);
}

TEST_CASE("a linked release delivers static module output to a piped parent")
{
const TempDir source;
write_file(source.dir()/"zodiac.toml","[game]\nmain = \"main.zs\"\n[modules]\nstatic = [\"example\"]\n");
write_file(source.dir()/"main.zs","int main(){example_print(\"merhaba kanka \xC5\x9F\");return example_add(20,3);}\n");
const std::string payload=payload_of(source.dir());
const TempDir folder;
const Linked linked=link_game(folder.dir(),payload,{"example"});
INFO(join(linked.lines));
REQUIRE(linked.result.has_value());
// No shared library next to the executable: example is linked in, its record forced with -u zodiac_static_module_example.
const zodiac::platform::ProcessResult ran=run_exe(folder.dir()/"game.exe");
CHECK(ran.output=="merhaba kanka \xC5\x9F\n");
CHECK(ran.exit_code==23);
}

TEST_CASE("link_release reports the linker's output when linking fails")
{
const TempDir folder;
const std::string payload=payload_of(from_utf8(ZODIAC_TEST_SCRIPT_DIR)/"int_main");
const Linked linked=link_game(folder.dir(),payload,{},{folder.dir()/"missing input.a"});
REQUIRE(!linked.result.has_value());
CHECK(linked.result.error()=="error: linking failed");
INFO(join(linked.lines));
REQUIRE(linked.lines.size()>=2);
CHECK(linked.lines[0]=="error: linking failed");
CHECK(linked.lines[1].find("missing input.a")!=std::string::npos);
// The linker's output follows as continuation lines.
for(std::size_t i=1;i<linked.lines.size();++i)CHECK(linked.lines[i].starts_with("  "));
CHECK(!fs::exists(folder.dir()/"game.exe"));
CHECK(!fs::exists(folder.dir()/"game.exe.rsp"));
}
