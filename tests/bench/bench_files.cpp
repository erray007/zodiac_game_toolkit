// bench_files: measurements of the pack and file code (spec 8.2 and 9 of the library-and-packs design). The two read operations are
// gated by tools/check_bench.py in CI (pack against disk, at the limit Task 13 measured); the others are report-only. Not a ctest
// test: CI runs it on the release presets.
// Contract:
// - crypto_chunk: one 64 KiB seal plus open of a pack chunk (32 bytes of aad), with each vectorized ChaCha20 this CPU runs (sse2,
//   and avx2 when the CPU has it) against Monocypher's scalar one (reference), all through crypto::detail::seal_with and open_with,
//   so only the keystream differs. Every side is first checked to give the reference's cipher and tag and to open it (exit 3
//   otherwise). Then rounds (--rounds, 101): in each, every candidate runs A B B A against the reference, A the reference and B the
//   candidate, each a timed window of the same reps (calibrated once so the reference's window takes at least 2 ms), so a drift of
//   the machine's speed during a round weighs on both sides alike. Per candidate: ratio is the median over rounds of
//   (B + B) / (A + A), its 10th and 90th percentiles, and the median nanoseconds of one seal plus open of each side.
// - poly1305_chunk: Poly1305 over 64 KiB, crypto::detail::poly1305_donna (poly1305-donna-64) against Monocypher's
//   crypto::detail::poly1305 (reference), first checked to give the same tag (exit 3 otherwise), then timed as crypto_chunk is
//   (A B B A rounds of equal reps, the reference's window at least 2 ms). It tells how much of a chunk's time Poly1305 is.
// - kdf_open: crypto::derive_password_key with the defaults (kDefaultKdfKib, kDefaultKdfPasses), one untimed run then 11 timed ones:
//   the median, lowest and highest milliseconds. Task 13 records it against the spec's "well under 100 ms" (here at most 50 ms).
// - The read operations (gated): a 32 MiB file of seeded random bytes on disk, in a folder mounted read only at "disk" (a
//   FolderSource, as zodiac run mounts the game's folder), and the same bytes as one entry of a pack made with a raw key and chunks
//   of 2^--chunk-log2 bytes (the default PackOptions::chunk_log2), closed and opened again read only, mounted at "pack" (a release
//   pack's way). Both are read through Vfs::open_read and Reader::read_at, as a script's file object reads, and both are first read
//   whole and at every seek offset and compared with the bytes written (exit 3 on a difference), which also warms them. One rep opens
//   the file and makes the operation's reads:
//   - pack_read_sequential: the whole file in 64 KiB reads (512 reads);
//   - pack_read_random_seek: 2 000 reads of 4 KiB at offsets drawn once from a seeded generator, the same for both sides.
//   Each side is calibrated to its own reps so that its timed window takes at least --window-ms (10) milliseconds (equal-time
//   calibration, bench_types' --calibrate time; a pack rep of the random seeks is longer than that by itself). Then --rounds (31)
//   rounds: each runs every read operation's two sides back to back, the side that runs first alternating with the round. Per
//   operation: new_ns is the median over rounds of the pack's nanoseconds per read, addon_ns the disk's, ratio the median of the
//   per-round ratios pack/disk (the fields tools/check_bench.py gates; new_ns and addon_ns are named as bench_types names them).
// - Output: a line {"machine":{"cpu":brand,"logical_cores":n,"os":build}}, then one JSON line per operation, to stdout and to --out
//   when given; stderr gets a readable summary.
//   {"op":"crypto_chunk","bytes":65536,"rounds":n,"reps":n,"reference_ns":n,"dispatched":name,
//    "candidates":[{"name":name,"ns":n,"ratio":r,"ratio_p10":r,"ratio_p90":r}...]}
//   {"op":"poly1305_chunk","bytes":65536,"rounds":n,"reps":n,"reference_ns":n,"candidates":[{"name":"donna64",...}]}
//   {"op":"kdf_open","kib":n,"passes":n,"runs":11,"ms":m,"min_ms":m,"max_ms":m}
//   {"op":"pack_read_sequential","new_ns":n,"addon_ns":n,"ratio":r,"ratio_p10":r,"ratio_p90":r,"calibrate":"time","window_ms":w,
//    "pack_reps":n,"disk_reps":n,"rounds":n,"chunk_log2":c,"file_bytes":33554432,"read_bytes":65536,"reads":512}
//   (pack_read_random_seek likewise, with read_bytes 4096 and reads 2000).
// - Usage: bench_files [--op NAME]... [--rounds N] [--window-ms N] [--chunk-log2 N] [--out PATH] [--list] [--list-gated]. Without
//   --op every operation runs; --list prints the names, --list-gated the gated ones (the --expected list of check_bench.py).
//   --rounds defaults to 101 for crypto_chunk and poly1305_chunk and to 31 for the reads.
// - Exit codes: 0 measured, 1 bad arguments, 3 a keystream, a tag or a read disagrees with the reference, 4 the read operations'
//   files could not be made, mounted or read.
#include <core/library/random.hpp>
#include <core/pack/crypto.hpp>
#include <core/pack/pack_source.hpp>
#include <core/pack/pack_storage.hpp>
#include <core/vfs/folder_source.hpp>
#include <core/vfs/vfs.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
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
namespace fs=std::filesystem;

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

// ---- poly1305_chunk ----
using MacFunction=void(std::uint8_t tag[16],const std::uint8_t* message,std::size_t size,const std::uint8_t key[32]) noexcept;

// One Poly1305 of the chunk with mac, reps times (the key changes with each rep's tag, so no call can be skipped); returns the
// nanoseconds and folds the tags into sink.
std::int64_t time_mac(MacFunction* mac,const std::uint8_t* message,std::uint8_t key[32],int reps,unsigned& sink)
{
std::uint8_t tag[16];
const Clock::time_point start=Clock::now();
for(int rep=0;rep<reps;++rep){
mac(tag,message,kChunk,key);
key[rep&31]^=tag[0];
sink+=tag[15];
}
return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
}

std::string poly1305_chunk(int rounds)
{
Chunk chunk;
const auto* message=reinterpret_cast<const std::uint8_t*>(chunk.plain.data());
std::uint8_t key[32];
std::memcpy(key,chunk.key.data(),32);
std::uint8_t expected[16];
std::uint8_t tag[16];
crypto::detail::poly1305(expected,message,kChunk,key);
crypto::detail::poly1305_donna(tag,message,kChunk,key);
if(std::memcmp(expected,tag,16)!=0)fail(3,"poly1305_chunk: donna64 disagrees with the reference");
unsigned sink=0;
int reps=1;
while(time_mac(&crypto::detail::poly1305,message,key,reps,sink)<2'000'000&&reps<(1<<20))reps*=2;
std::vector<double> ratios;
std::vector<double> donna_ns;
std::vector<double> reference_ns;
for(int round=0;round<rounds;++round){
const std::int64_t a1=time_mac(&crypto::detail::poly1305,message,key,reps,sink);
const std::int64_t b1=time_mac(&crypto::detail::poly1305_donna,message,key,reps,sink);
const std::int64_t b2=time_mac(&crypto::detail::poly1305_donna,message,key,reps,sink);
const std::int64_t a2=time_mac(&crypto::detail::poly1305,message,key,reps,sink);
ratios.push_back(static_cast<double>(b1+b2)/static_cast<double>(a1+a2));
donna_ns.push_back(static_cast<double>(b1+b2)/(2.0*reps));
reference_ns.push_back(static_cast<double>(a1+a2)/(2.0*reps));
}
const double reference=quantile(reference_ns,0.5);
const double ns=quantile(donna_ns,0.5);
const double ratio=quantile(ratios,0.5);
const double p10=quantile(ratios,0.1);
const double p90=quantile(ratios,0.9);
std::fprintf(stderr,"bench_files: poly1305_chunk: one 64 KiB Poly1305, %d rounds of A B B A, %d reps per window; reference (Monocypher) %.0f ns (%.0f MB/s), donna64 %.0f ns (%.0f MB/s), paired ratio %.4f (p10 %.4f, p90 %.4f)\n",rounds,reps,reference,kChunk/reference*1000.0,ns,kChunk/ns*1000.0,ratio,p10,p90);
crypto::wipe(key,sizeof key);
if(sink==0xFFFFFFFFu)std::fprintf(stderr,"bench_files: sink %u\n",sink);
return "{\"op\":\"poly1305_chunk\",\"bytes\":"+std::to_string(kChunk)+",\"rounds\":"+std::to_string(rounds)+",\"reps\":"+std::to_string(reps)+",\"reference_ns\":"+number(reference,0)+",\"candidates\":[{\"name\":\"donna64\",\"ns\":"+number(ns,0)+",\"ratio\":"+number(ratio,4)+",\"ratio_p10\":"+number(p10,4)+",\"ratio_p90\":"+number(p90,4)+"}]}";
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

// ---- the read operations ----
constexpr std::uint64_t kFileBytes=std::uint64_t{32}<<20;
constexpr std::size_t kSequentialRead=65536;
constexpr std::size_t kSeekRead=4096;
constexpr int kSeekReads=2000;

// The two mounted copies of the file, made in a fresh folder of the system's temporary folder and removed with it.
class ReadFiles{
public:
explicit ReadFiles(std::uint8_t chunk_log2)
{
std::array<std::byte,8> name{};
crypto::random_bytes(name);
std::string suffix;
for(const std::byte b:name){
char hex[3];
std::snprintf(hex,sizeof hex,"%02x",static_cast<unsigned>(b));
suffix+=hex;
}
std::error_code error;
root_=fs::temp_directory_path(error)/("zodiac-bench-files-"+suffix);
if(error||!fs::create_directories(root_/"disk",error)||error)fail(4,"cannot make a folder in the temporary folder: "+error.message());
bytes_.resize(kFileBytes);
zodiac::Xoshiro256 random;
random.seed(0xF11E5u);
for(std::size_t at=0;at<bytes_.size();at+=8){
const std::uint64_t word=random.next();
std::memcpy(bytes_.data()+at,&word,8);
}
{
std::ofstream out(root_/"disk"/"level.bin",std::ios::binary|std::ios::trunc);
out.write(reinterpret_cast<const char*>(bytes_.data()),static_cast<std::streamsize>(bytes_.size()));
out.close();
if(!out)fail(4,"cannot write the disk copy of the file");
}
crypto::Key key{};
for(std::size_t i=0;i<key.size();++i)key[i]=static_cast<std::uint8_t>(0x5A^(i*7));
const fs::path pack_path=root_/"level.zpk";
{
auto made=zodiac::pack::PackStorage::open(pack_path,key,true,zodiac::pack::PackOptions{crypto::kDefaultKdfKib,crypto::kDefaultKdfPasses,chunk_log2});
if(!made)fail(4,"cannot make the pack: "+made.error().failure.message);
if(!(*made)->add("level.bin",bytes_,0)||!(*made)->flush()||!(*made)->close())fail(4,"cannot write the pack");
}
// Opened again, as a release opens its pack: the index read back from the file, nothing of the writing kept.
auto opened=zodiac::pack::PackStorage::open(pack_path,key,false);
crypto::wipe(key.data(),key.size());
if(!opened)fail(4,"cannot open the pack: "+opened.error().failure.message);
vfs_.emplace();
if(!vfs_->mount("disk",std::make_shared<zodiac::vfs::FolderSource>(root_/"disk",true),false,nullptr)||!vfs_->mount("pack",zodiac::pack::make_pack_source(std::move(*opened),false),false,nullptr))fail(4,"cannot mount the two copies");
zodiac::Xoshiro256 offsets;
offsets.seed(0x5EEC5u);
offsets_.reserve(kSeekReads);
for(int i=0;i<kSeekReads;++i)offsets_.push_back(offsets.next()%(kFileBytes-kSeekRead+1));
buffer_.resize(kSequentialRead);
}
~ReadFiles()
{
// The mounts hold the pack file open, which Windows would not delete.
vfs_.reset();
std::error_code error;
fs::remove_all(root_,error);
}
ReadFiles(const ReadFiles&)=delete;
ReadFiles& operator=(const ReadFiles&)=delete;
std::shared_ptr<zodiac::vfs::Reader> open(std::string_view path)
{
auto reader=vfs_->open_read(path);
if(!reader||!*reader)fail(4,std::string("cannot open ")+std::string(path));
return std::move(*reader);
}
// Reads the whole file and every seek offset of path and compares them with the bytes written.
void verify(std::string_view path)
{
const auto reader=open(path);
if(reader->size()!=kFileBytes)fail(3,std::string(path)+" has the wrong size");
for(std::uint64_t at=0;at<kFileBytes;at+=kSequentialRead){
if(reader->read_at(at,buffer_)!=static_cast<std::int64_t>(kSequentialRead)||std::memcmp(buffer_.data(),bytes_.data()+at,kSequentialRead)!=0)fail(3,std::string(path)+" reads other bytes than were written");
}
for(const std::uint64_t at:offsets_){
if(reader->read_at(at,std::span(buffer_).first(kSeekRead))!=static_cast<std::int64_t>(kSeekRead)||std::memcmp(buffer_.data(),bytes_.data()+at,kSeekRead)!=0)fail(3,std::string(path)+" reads other bytes than were written at a seek");
}
}
// reps reps of the sequential reads of path: the nanoseconds, a byte of each read folded into sink.
std::int64_t sequential(std::string_view path,int reps,unsigned& sink)
{
const Clock::time_point start=Clock::now();
for(int rep=0;rep<reps;++rep){
const auto reader=open(path);
for(std::uint64_t at=0;at<kFileBytes;at+=kSequentialRead){
if(reader->read_at(at,buffer_)!=static_cast<std::int64_t>(kSequentialRead))fail(4,std::string("a read of ")+std::string(path)+" failed");
sink+=static_cast<unsigned>(buffer_[static_cast<std::size_t>(at>>16)&(kSequentialRead-1)]);
}
}
return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
}
// reps reps of the 4 KiB reads at the seek offsets of path.
std::int64_t random_seek(std::string_view path,int reps,unsigned& sink)
{
const Clock::time_point start=Clock::now();
for(int rep=0;rep<reps;++rep){
const auto reader=open(path);
for(const std::uint64_t at:offsets_){
if(reader->read_at(at,std::span(buffer_).first(kSeekRead))!=static_cast<std::int64_t>(kSeekRead))fail(4,std::string("a read of ")+std::string(path)+" failed");
sink+=static_cast<unsigned>(buffer_[static_cast<std::size_t>(at)&(kSeekRead-1)]);
}
}
return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
}
private:
fs::path root_;
std::vector<std::byte> bytes_;
std::optional<zodiac::vfs::Vfs> vfs_;
std::vector<std::uint64_t> offsets_;
std::vector<std::byte> buffer_;
};

struct ReadOp{
std::string_view name;
std::int64_t (ReadFiles::*run)(std::string_view path,int reps,unsigned& sink)=nullptr;
std::size_t read_bytes=0;
int reads=0;
int pack_reps=1;
int disk_reps=1;
std::vector<double> pack_ns;
std::vector<double> disk_ns;
std::vector<double> ratios;
};

// Grows reps until one timed run of path takes at least target_ns (each run doubles as a warm-up).
int calibrate_reads(ReadFiles& files,ReadOp& op,std::string_view path,double target_ns,unsigned& sink)
{
int reps=1;
for(;;){
const auto ns=static_cast<double>((files.*op.run)(path,reps,sink));
if(ns>=target_ns||reps>=(1<<20))return reps;
const double scale=ns>0?target_ns/ns*1.1:16;
reps=static_cast<int>(std::min<double>(1<<20,std::max<double>(reps*2.0,reps*scale)));
}
}

std::vector<std::string> read_ops(const std::vector<std::string_view>& names,int rounds,int window_ms,std::uint8_t chunk_log2)
{
ReadFiles files(chunk_log2);
files.verify("disk/level.bin");
files.verify("pack/level.bin");
std::vector<ReadOp> ops;
for(const std::string_view name:names){
ReadOp& op=ops.emplace_back();
op.name=name;
const bool sequential=name=="pack_read_sequential";
op.run=sequential?&ReadFiles::sequential:&ReadFiles::random_seek;
op.read_bytes=sequential?kSequentialRead:kSeekRead;
op.reads=sequential?static_cast<int>(kFileBytes/kSequentialRead):kSeekReads;
}
unsigned sink=0;
const double target_ns=window_ms*1e6;
for(ReadOp& op:ops){
op.pack_reps=calibrate_reads(files,op,"pack/level.bin",target_ns,sink);
op.disk_reps=calibrate_reads(files,op,"disk/level.bin",target_ns,sink);
}
// Every round takes each operation in turn, so each one's rounds spread over the whole run.
for(int round=0;round<rounds;++round){
for(ReadOp& op:ops){
const bool pack_first=round%2==0;
std::int64_t pack=0;
std::int64_t disk=0;
if(pack_first)pack=(files.*op.run)("pack/level.bin",op.pack_reps,sink);
disk=(files.*op.run)("disk/level.bin",op.disk_reps,sink);
if(!pack_first)pack=(files.*op.run)("pack/level.bin",op.pack_reps,sink);
const double pack_read=static_cast<double>(pack)/(static_cast<double>(op.pack_reps)*op.reads);
const double disk_read=static_cast<double>(disk)/(static_cast<double>(op.disk_reps)*op.reads);
op.pack_ns.push_back(pack_read);
op.disk_ns.push_back(disk_read);
op.ratios.push_back(disk_read>0?pack_read/disk_read:0.0);
}
}
std::vector<std::string> lines;
for(const ReadOp& op:ops){
const double pack=quantile(op.pack_ns,0.5);
const double disk=quantile(op.disk_ns,0.5);
const double ratio=quantile(op.ratios,0.5);
const double p10=quantile(op.ratios,0.1);
const double p90=quantile(op.ratios,0.9);
std::fprintf(stderr,"bench_files: %.*s: %d reads of %zu bytes per rep, chunks of %u bytes, %d rounds, reps pack %d disk %d (window %d ms): pack %.0f ns per read (%.0f MB/s), disk %.0f ns (%.0f MB/s), ratio %.3f (p10 %.3f, p90 %.3f)\n",static_cast<int>(op.name.size()),op.name.data(),op.reads,op.read_bytes,1u<<chunk_log2,rounds,op.pack_reps,op.disk_reps,window_ms,pack,op.read_bytes/pack*1000.0,disk,op.read_bytes/disk*1000.0,ratio,p10,p90);
lines.push_back("{\"op\":\""+std::string(op.name)+"\",\"new_ns\":"+number(pack,1)+",\"addon_ns\":"+number(disk,1)+",\"ratio\":"+number(ratio,4)+",\"ratio_p10\":"+number(p10,4)+",\"ratio_p90\":"+number(p90,4)+",\"calibrate\":\"time\",\"window_ms\":"+std::to_string(window_ms)+",\"pack_reps\":"+std::to_string(op.pack_reps)+",\"disk_reps\":"+std::to_string(op.disk_reps)+",\"rounds\":"+std::to_string(rounds)+",\"chunk_log2\":"+std::to_string(chunk_log2)+",\"file_bytes\":"+std::to_string(kFileBytes)+",\"read_bytes\":"+std::to_string(op.read_bytes)+",\"reads\":"+std::to_string(op.reads)+"}");
}
if(sink==0xFFFFFFFFu)std::fprintf(stderr,"bench_files: sink %u\n",sink);
return lines;
}

constexpr std::string_view kOps[]{"crypto_chunk","poly1305_chunk","kdf_open","pack_read_sequential","pack_read_random_seek"};
// The operations tools/check_bench.py gates (CI's --expected list).
constexpr std::string_view kGatedOps[]{"pack_read_sequential","pack_read_random_seek"};

bool parse_whole(std::string_view value,int& out,int low,int high)
{
const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),out);
return error==std::errc()&&end==value.data()+value.size()&&out>=low&&out<=high;
}
}

int main(int argc,char** argv)
{
std::vector<std::string> only;
std::optional<int> rounds;
int window_ms=10;
int chunk_log2=zodiac::pack::kDefaultChunkLog2;
std::string out_path;
bool list=false;
bool list_gated=false;
for(int i=1;i<argc;++i){
const std::string_view arg=argv[i];
const bool has_value=i+1<argc;
if(arg=="--op"&&has_value){
const std::string_view name=argv[++i];
if(std::find(std::begin(kOps),std::end(kOps),name)==std::end(kOps))fail(1,"no operation is named "+std::string(name)+" (--list prints them)");
only.emplace_back(name);
}
else if(arg=="--rounds"&&has_value){
int value=0;
if(!parse_whole(argv[++i],value,1,1'000'000))fail(1,"--rounds needs a whole number of at least 1");
rounds=value;
}
else if(arg=="--window-ms"&&has_value){
if(!parse_whole(argv[++i],window_ms,1,10'000))fail(1,"--window-ms needs a whole number from 1 to 10000");
}
else if(arg=="--chunk-log2"&&has_value){
if(!parse_whole(argv[++i],chunk_log2,zodiac::pack::kMinChunkLog2,zodiac::pack::kMaxChunkLog2))fail(1,"--chunk-log2 needs a whole number from "+std::to_string(zodiac::pack::kMinChunkLog2)+" to "+std::to_string(zodiac::pack::kMaxChunkLog2));
}
else if(arg=="--out"&&has_value)out_path=argv[++i];
else if(arg=="--list")list=true;
else if(arg=="--list-gated")list_gated=true;
else fail(1,"usage: bench_files [--op NAME]... [--rounds N] [--window-ms N] [--chunk-log2 N] [--out PATH] [--list] [--list-gated]");
}
if(list||list_gated){
for(const std::string_view name:list?std::span<const std::string_view>(kOps):std::span<const std::string_view>(kGatedOps))std::printf("%.*s\n",static_cast<int>(name.size()),name.data());
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
if(wanted("crypto_chunk"))lines.push_back(crypto_chunk(rounds.value_or(101)));
if(wanted("poly1305_chunk"))lines.push_back(poly1305_chunk(rounds.value_or(101)));
if(wanted("kdf_open"))lines.push_back(kdf_open());
std::vector<std::string_view> reads;
for(const std::string_view name:kGatedOps)if(wanted(name))reads.push_back(name);
if(!reads.empty()){
std::vector<std::string> read_lines=read_ops(reads,rounds.value_or(31),window_ms,static_cast<std::uint8_t>(chunk_log2));
lines.insert(lines.end(),read_lines.begin(),read_lines.end());
}
std::FILE* out=out_path.empty()?nullptr:std::fopen(out_path.c_str(),"w");
if(!out_path.empty()&&out==nullptr)fail(1,"cannot write "+out_path);
for(const std::string& line:lines){
std::printf("%s\n",line.c_str());
if(out)std::fprintf(out,"%s\n",line.c_str());
}
if(out)std::fclose(out);
return 0;
}
