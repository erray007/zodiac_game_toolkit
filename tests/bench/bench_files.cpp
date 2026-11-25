// bench_files: report-only measurements of the pack and file code (spec 8.2 of the library-and-packs design); Task 13 adds the read
// operations and gates them. Not a ctest test: CI runs it on the release presets and prints its lines.
// Contract:
// - crypto_chunk: one 64 KiB seal plus open of a pack chunk (32 bytes of aad), with each vectorized ChaCha20 this CPU runs (sse2,
//   and avx2 when the CPU has it) against Monocypher's scalar one (reference), all through crypto::detail::seal_with and open_with,
//   so only the keystream differs. Every side is first checked to give the reference's cipher and tag and to open it (exit 3
//   otherwise). Then rounds (--rounds, 101): in each, every candidate runs A B B A against the reference, A the reference and B the
//   candidate, each a timed window of the same reps (calibrated once so the reference's window takes at least 2 ms), so a drift of
//   the machine's speed during a round weighs on both sides alike. Per candidate: ratio is the median over rounds of
//   (B + B) / (A + A), its 10th and 90th percentiles, and the median nanoseconds of one seal plus open of each side.
// - kdf_open: crypto::derive_password_key with the defaults (kDefaultKdfKib, kDefaultKdfPasses), one untimed run then 11 timed ones:
//   the median, lowest and highest milliseconds. Task 13 records it against the spec's "well under 100 ms" (here at most 50 ms).
// - Output: a line {"machine":{"cpu":brand,"logical_cores":n,"os":build}}, then one JSON line per operation, to stdout and to --out
//   when given; stderr gets a readable summary.
//   {"op":"crypto_chunk","bytes":65536,"rounds":n,"reps":n,"reference_ns":n,"dispatched":name,
//    "candidates":[{"name":name,"ns":n,"ratio":r,"ratio_p10":r,"ratio_p90":r}...]}
//   {"op":"kdf_open","kib":n,"passes":n,"runs":11,"ms":m,"min_ms":m,"max_ms":m}
// - Usage: bench_files [--op NAME]... [--rounds N] [--out PATH] [--list]. Without --op every operation runs; --list prints the names.
// - Exit codes: 0 measured, 1 bad arguments, 3 a keystream disagrees with the reference.
#include <core/pack/crypto.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <sys/utsname.h>
#endif
#if defined(__x86_64__)
#include <cpuid.h>
#endif

namespace crypto=zodiac::crypto;

namespace {
using Clock=std::chrono::steady_clock;

[[noreturn]] void fail(int code,const std::string& message)
{
std::fprintf(stderr,"bench_files: %s\n",message.c_str());
std::exit(code);
}

std::string folded_spaces(std::string_view text)
{
std::string out;
for(const char c:text){
if(c==' '&&(out.empty()||out.back()==' '))continue;
out.push_back(c);
}
while(!out.empty()&&out.back()==' ')out.pop_back();
return out;
}

// The CPU's brand string, as bench_types names it (CPUID leaves 0x80000002-0x80000004 on x86-64, /proc/cpuinfo elsewhere on Linux).
std::string cpu_brand()
{
#if defined(__x86_64__)
unsigned words[12];
unsigned top=0,b=0,c=0,d=0;
if(!__get_cpuid(0x80000000u,&top,&b,&c,&d)||top<0x80000004u)return "unknown";
for(unsigned leaf=0;leaf<3;++leaf)__get_cpuid(0x80000002u+leaf,&words[leaf*4],&words[leaf*4+1],&words[leaf*4+2],&words[leaf*4+3]);
char brand[sizeof words+1]={};
std::memcpy(brand,words,sizeof words);
const std::string text=folded_spaces(brand);
return text.empty()?"unknown":text;
#elif defined(__linux__)
std::FILE* file=std::fopen("/proc/cpuinfo","r");
if(!file)return "unknown";
std::string model;
char line[512];
while(model.empty()&&std::fgets(line,sizeof line,file)){
const std::string_view text(line);
const std::size_t colon=text.find(':');
if(colon==std::string_view::npos)continue;
const std::string key=folded_spaces(text.substr(0,colon));
if(key=="model name"||key=="Model"){
std::string value=folded_spaces(text.substr(colon+1));
while(!value.empty()&&(value.front()==' '||value.front()=='\t'))value.erase(value.begin());
while(!value.empty()&&(value.back()=='\n'||value.back()=='\r'))value.pop_back();
model=value;
}
}
std::fclose(file);
return model.empty()?"unknown":model;
#else
return "unknown";
#endif
}

std::string os_build()
{
#if defined(_WIN32)
using GetVersion=LONG(WINAPI*)(OSVERSIONINFOW*);
OSVERSIONINFOW info{};
info.dwOSVersionInfoSize=sizeof info;
const HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");
const auto get_version=ntdll?reinterpret_cast<GetVersion>(reinterpret_cast<void*>(GetProcAddress(ntdll,"RtlGetVersion"))):nullptr;
if(!get_version||get_version(&info)!=0)return "Windows (unknown version)";
return "Windows "+std::to_string(info.dwMajorVersion)+"."+std::to_string(info.dwMinorVersion)+"."+std::to_string(info.dwBuildNumber);
#else
utsname name{};
if(uname(&name)!=0)return "unknown";
return folded_spaces(std::string(name.sysname)+" "+name.release+" "+name.version);
#endif
}

std::string json_string(std::string_view text)
{
std::string out="\"";
for(const char c:text){
const unsigned char byte=static_cast<unsigned char>(c);
if(c=='"'||c=='\\'){
out.push_back('\\');
out.push_back(c);
}
else if(byte<0x20){
char escape[8];
std::snprintf(escape,sizeof escape,"\\u%04x",byte);
out+=escape;
}
else out.push_back(c);
}
return out+"\"";
}

std::string number(double value,int digits)
{
char text[64];
std::snprintf(text,sizeof text,"%.*f",digits,value);
return text;
}

// The q-quantile (0 to 1) of values, by the nearest rank.
double quantile(std::vector<double> values,double q)
{
std::sort(values.begin(),values.end());
const std::size_t index=static_cast<std::size_t>(q*static_cast<double>(values.size()-1)+0.5);
return values[index];
}

// ---- crypto_chunk ----
constexpr std::size_t kChunk=65536;

struct Chunk {
crypto::Key key{};
crypto::Nonce nonce{};
std::vector<std::byte> aad=std::vector<std::byte>(32);
std::vector<std::byte> plain=std::vector<std::byte>(kChunk);
std::vector<std::byte> cipher=std::vector<std::byte>(kChunk);
std::vector<std::byte> opened=std::vector<std::byte>(kChunk);
Chunk()
{
std::uint64_t state=0x243F6A8885A308D3u;
const auto next=[&state]{
state=state*6364136223846793005u+1442695040888963407u;
return static_cast<std::uint8_t>(state>>56);
};
for(auto& b:key)b=next();
for(auto& b:nonce)b=next();
for(auto& b:aad)b=std::byte{next()};
for(auto& b:plain)b=std::byte{next()};
}
};

// One seal plus open of the chunk on stream, reps times; returns the nanoseconds and folds the opened bytes into sink.
std::int64_t time_chunk(const crypto::detail::Keystream& stream,Chunk& chunk,int reps,unsigned& sink)
{
crypto::Tag tag{};
const Clock::time_point start=Clock::now();
for(int rep=0;rep<reps;++rep){
crypto::detail::seal_with(stream,chunk.key,chunk.nonce,chunk.aad,chunk.plain,chunk.cipher,tag);
if(!crypto::detail::open_with(stream,chunk.key,chunk.nonce,chunk.aad,chunk.cipher,tag,chunk.opened))fail(3,std::string("crypto_chunk: ")+stream.name+" could not open its own chunk");
sink+=static_cast<unsigned>(chunk.opened[static_cast<std::size_t>(rep)&(kChunk-1)]);
}
return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
}

std::string crypto_chunk(int rounds)
{
Chunk chunk;
std::vector<const crypto::detail::Keystream*> candidates;
#if defined(ZODIAC_CRYPTO_SIMD)
candidates.push_back(&crypto::detail::sse2);
if(crypto::detail::cpu_has_avx2())candidates.push_back(&crypto::detail::avx2);
#endif
// Every candidate must give the reference's cipher and tag.
std::vector<std::byte> expected(kChunk);
crypto::Tag expected_tag{};
crypto::detail::seal_with(crypto::detail::reference,chunk.key,chunk.nonce,chunk.aad,chunk.plain,expected,expected_tag);
for(const crypto::detail::Keystream* stream:candidates){
crypto::Tag tag{};
crypto::detail::seal_with(*stream,chunk.key,chunk.nonce,chunk.aad,chunk.plain,chunk.cipher,tag);
if(chunk.cipher!=expected||tag!=expected_tag)fail(3,std::string("crypto_chunk: ")+stream->name+" disagrees with the reference");
}
unsigned sink=0;
// Reps: the reference's window takes at least 2 ms.
int reps=1;
while(time_chunk(crypto::detail::reference,chunk,reps,sink)<2'000'000&&reps<(1<<20))reps*=2;
std::vector<std::vector<double>> ratios(candidates.size());
std::vector<std::vector<double>> candidate_ns(candidates.size());
std::vector<double> reference_ns;
for(int round=0;round<rounds;++round){
for(std::size_t c=0;c<candidates.size();++c){
const std::int64_t a1=time_chunk(crypto::detail::reference,chunk,reps,sink);
const std::int64_t b1=time_chunk(*candidates[c],chunk,reps,sink);
const std::int64_t b2=time_chunk(*candidates[c],chunk,reps,sink);
const std::int64_t a2=time_chunk(crypto::detail::reference,chunk,reps,sink);
ratios[c].push_back(static_cast<double>(b1+b2)/static_cast<double>(a1+a2));
candidate_ns[c].push_back(static_cast<double>(b1+b2)/(2.0*reps));
reference_ns.push_back(static_cast<double>(a1+a2)/(2.0*reps));
}
}
const double reference=reference_ns.empty()?0.0:quantile(reference_ns,0.5);
std::string line="{\"op\":\"crypto_chunk\",\"bytes\":"+std::to_string(kChunk)+",\"rounds\":"+std::to_string(rounds)+",\"reps\":"+std::to_string(reps)+",\"reference_ns\":"+number(reference,0)+",\"dispatched\":"+json_string(crypto::detail::dispatched().name)+",\"candidates\":[";
std::fprintf(stderr,"bench_files: crypto_chunk: one 64 KiB seal plus open, %d rounds of A B B A, %d reps per window; reference %.0f ns (%.0f MB/s each way)\n",rounds,reps,reference,reference>0?2.0*kChunk/reference*1000.0:0.0);
for(std::size_t c=0;c<candidates.size();++c){
const double ns=quantile(candidate_ns[c],0.5);
const double ratio=quantile(ratios[c],0.5);
const double p10=quantile(ratios[c],0.1);
const double p90=quantile(ratios[c],0.9);
if(c!=0)line+=",";
line+="{\"name\":"+json_string(candidates[c]->name)+",\"ns\":"+number(ns,0)+",\"ratio\":"+number(ratio,4)+",\"ratio_p10\":"+number(p10,4)+",\"ratio_p90\":"+number(p90,4)+"}";
std::fprintf(stderr,"bench_files: crypto_chunk: %s %.0f ns, paired ratio to the reference %.4f (p10 %.4f, p90 %.4f): %.1f %% faster\n",candidates[c]->name,ns,ratio,p10,p90,(1.0-ratio)*100.0);
}
line+="]}";
if(sink==0xFFFFFFFFu)std::fprintf(stderr,"bench_files: sink %u\n",sink);
return line;
}

// ---- kdf_open ----
std::string kdf_open()
{
std::array<std::uint8_t,16> salt{};
for(std::size_t i=0;i<salt.size();++i)salt[i]=static_cast<std::uint8_t>(0xA5^i);
constexpr std::string_view password="a script pack password";
unsigned sink=0;
crypto::Key key=crypto::derive_password_key(password,salt,crypto::kDefaultKdfKib,crypto::kDefaultKdfPasses);
sink+=key[0];
constexpr int runs=11;
std::vector<double> ms;
for(int run=0;run<runs;++run){
const Clock::time_point start=Clock::now();
key=crypto::derive_password_key(password,salt,crypto::kDefaultKdfKib,crypto::kDefaultKdfPasses);
ms.push_back(std::chrono::duration<double,std::milli>(Clock::now()-start).count());
sink+=key[run%32];
}
crypto::wipe(key.data(),key.size());
const double median=quantile(ms,0.5);
const double low=*std::min_element(ms.begin(),ms.end());
const double high=*std::max_element(ms.begin(),ms.end());
std::fprintf(stderr,"bench_files: kdf_open: Argon2id %u KiB, %u passes: median %.2f ms (lowest %.2f, highest %.2f, %d runs)\n",crypto::kDefaultKdfKib,crypto::kDefaultKdfPasses,median,low,high,runs);
if(sink==0xFFFFFFFFu)std::fprintf(stderr,"bench_files: sink %u\n",sink);
return "{\"op\":\"kdf_open\",\"kib\":"+std::to_string(crypto::kDefaultKdfKib)+",\"passes\":"+std::to_string(crypto::kDefaultKdfPasses)+",\"runs\":"+std::to_string(runs)+",\"ms\":"+number(median,3)+",\"min_ms\":"+number(low,3)+",\"max_ms\":"+number(high,3)+"}";
}

constexpr std::string_view kOps[]{"crypto_chunk","kdf_open"};
}

int main(int argc,char** argv)
{
std::vector<std::string> only;
int rounds=101;
std::string out_path;
bool list=false;
for(int i=1;i<argc;++i){
const std::string_view arg=argv[i];
const bool has_value=i+1<argc;
if(arg=="--op"&&has_value){
const std::string_view name=argv[++i];
if(std::find(std::begin(kOps),std::end(kOps),name)==std::end(kOps))fail(1,"no operation is named "+std::string(name)+" (--list prints them)");
only.emplace_back(name);
}
else if(arg=="--rounds"&&has_value){
const std::string_view value=argv[++i];
const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),rounds);
if(error!=std::errc()||end!=value.data()+value.size()||rounds<1)fail(1,"--rounds needs a whole number of at least 1");
}
else if(arg=="--out"&&has_value)out_path=argv[++i];
else if(arg=="--list")list=true;
else fail(1,"usage: bench_files [--op NAME]... [--rounds N] [--out PATH] [--list]");
}
if(list){
for(const std::string_view name:kOps)std::printf("%.*s\n",static_cast<int>(name.size()),name.data());
return 0;
}
const std::string cpu=cpu_brand();
const std::string os=os_build();
const unsigned cores=std::thread::hardware_concurrency();
std::fprintf(stderr,"bench_files: machine: %s, %u logical cores, %s\n",cpu.c_str(),cores,os.c_str());
std::vector<std::string> lines{"{\"machine\":{\"cpu\":"+json_string(cpu)+",\"logical_cores\":"+std::to_string(cores)+",\"os\":"+json_string(os)+"}}"};
const auto wanted=[&](std::string_view name){
return only.empty()||std::find(only.begin(),only.end(),name)!=only.end();
};
if(wanted("crypto_chunk"))lines.push_back(crypto_chunk(rounds));
if(wanted("kdf_open"))lines.push_back(kdf_open());
std::FILE* out=out_path.empty()?nullptr:std::fopen(out_path.c_str(),"w");
if(!out_path.empty()&&out==nullptr)fail(1,"cannot write "+out_path);
for(const std::string& line:lines){
std::printf("%s\n",line.c_str());
if(out)std::fprintf(out,"%s\n",line.c_str());
}
if(out)std::fclose(out);
return 0;
}
