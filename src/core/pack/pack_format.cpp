// The pack layout of pack_format.hpp: little-endian loads and stores, and the structural and semantic checks.
#include <core/pack/pack_format.hpp>
#include <core/payload.hpp>
#include <core/vfs/path.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace zodiac::pack {
namespace {
void put16(std::byte* out,std::uint16_t value) noexcept
{
out[0]=static_cast<std::byte>(value);
out[1]=static_cast<std::byte>(value>>8);
}
void put32(std::byte* out,std::uint32_t value) noexcept
{
for(int i=0;i<4;++i)out[i]=static_cast<std::byte>(value>>(8*i));
}
void put64(std::byte* out,std::uint64_t value) noexcept
{
for(int i=0;i<8;++i)out[i]=static_cast<std::byte>(value>>(8*i));
}
std::uint16_t get16(const std::byte* in) noexcept
{
return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(in[0])|std::to_integer<std::uint16_t>(in[1])<<8);
}
std::uint32_t get32(const std::byte* in) noexcept
{
std::uint32_t value=0;
for(int i=0;i<4;++i)value|=std::to_integer<std::uint32_t>(in[i])<<(8*i);
return value;
}
std::uint64_t get64(const std::byte* in) noexcept
{
std::uint64_t value=0;
for(int i=0;i<8;++i)value|=std::to_integer<std::uint64_t>(in[i])<<(8*i);
return value;
}
std::uint32_t crc_of(const std::byte* bytes,std::size_t size) noexcept
{
return crc32(std::string_view(reinterpret_cast<const char*>(bytes),size));
}
bool all_zero(const std::byte* bytes,std::size_t size) noexcept
{
return std::all_of(bytes,bytes+size,[](std::byte b){return b==std::byte{0};});
}
// Header offsets.
constexpr std::size_t kVersionAt=8;
constexpr std::size_t kFlagsAt=12;
constexpr std::size_t kChunkLog2At=16;
constexpr std::size_t kKdfAt=17;
constexpr std::size_t kReservedAt=18;
constexpr std::size_t kKdfKibAt=20;
constexpr std::size_t kKdfPassesAt=24;
constexpr std::size_t kSaltAt=28;
constexpr std::size_t kZerosAt=44;
// Footer offsets.
constexpr std::size_t kNonceAt=16;
constexpr std::size_t kCrcAt=28;
constexpr std::size_t kFooterMagicAt=kFooterMagicOffset;
}

HeaderBytes encode_header(const Header& header) noexcept
{
HeaderBytes out{};
std::memcpy(out.data(),kHeaderMagic.data(),kHeaderMagic.size());
put32(out.data()+kVersionAt,kFormatVersion);
put32(out.data()+kFlagsAt,header.flags);
out[kChunkLog2At]=static_cast<std::byte>(header.chunk_log2);
out[kKdfAt]=static_cast<std::byte>(header.kdf);
put32(out.data()+kKdfKibAt,header.kdf_kib);
put32(out.data()+kKdfPassesAt,header.kdf_passes);
std::memcpy(out.data()+kSaltAt,header.salt.data(),header.salt.size());
return out;
}

std::expected<Header,HeaderError> decode_header(std::span<const std::byte,kHeaderSize> bytes) noexcept
{
const std::byte* in=bytes.data();
if(std::memcmp(in,kHeaderMagic.data(),kHeaderMagic.size())!=0)return std::unexpected(HeaderError::damaged);
const std::uint32_t version=get32(in+kVersionAt);
if(version>kFormatVersion)return std::unexpected(HeaderError::newer);
if(version!=kFormatVersion)return std::unexpected(HeaderError::damaged);
if(!all_zero(in+kReservedAt,2)||!all_zero(in+kZerosAt,kHeaderSize-kZerosAt))return std::unexpected(HeaderError::damaged);
Header header;
header.flags=get32(in+kFlagsAt);
header.chunk_log2=std::to_integer<std::uint8_t>(in[kChunkLog2At]);
if(header.chunk_log2<kMinChunkLog2||header.chunk_log2>kMaxChunkLog2)return std::unexpected(HeaderError::damaged);
const auto kdf=std::to_integer<std::uint8_t>(in[kKdfAt]);
header.kdf_kib=get32(in+kKdfKibAt);
header.kdf_passes=get32(in+kKdfPassesAt);
if(kdf==static_cast<std::uint8_t>(Kdf::raw_key)){
if(header.kdf_kib!=0||header.kdf_passes!=0)return std::unexpected(HeaderError::damaged);
header.kdf=Kdf::raw_key;
}
else if(kdf==static_cast<std::uint8_t>(Kdf::argon2id)){
if(header.kdf_kib<crypto::kMinKdfKib||header.kdf_kib>crypto::kMaxKdfKib||header.kdf_passes<1||header.kdf_passes>crypto::kMaxKdfPasses)return std::unexpected(HeaderError::damaged);
header.kdf=Kdf::argon2id;
}
else return std::unexpected(HeaderError::damaged);
std::memcpy(header.salt.data(),in+kSaltAt,header.salt.size());
return header;
}

std::optional<std::uint64_t> stored_size(std::uint64_t size,std::uint8_t chunk_log2) noexcept
{
const std::uint64_t chunks=(size>>chunk_log2)+((size&((std::uint64_t{1}<<chunk_log2)-1))!=0);
if(chunks>(std::uint64_t{1}<<32))return std::nullopt;
const std::uint64_t tags=chunks*kTagSize;
if(size>std::numeric_limits<std::uint64_t>::max()-tags)return std::nullopt;
return size+tags;
}

crypto::Nonce chunk_nonce(std::uint32_t chunk_index,std::uint64_t entry_id) noexcept
{
crypto::Nonce nonce;
for(int i=0;i<4;++i)nonce[static_cast<std::size_t>(i)]=static_cast<std::uint8_t>(chunk_index>>(8*i));
for(int i=0;i<8;++i)nonce[static_cast<std::size_t>(4+i)]=static_cast<std::uint8_t>(entry_id>>(8*i));
return nonce;
}

std::byte* write_index_head(std::byte* out,std::uint64_t generation,std::uint64_t wasted_bytes,std::string_view metadata,std::uint32_t count) noexcept
{
put64(out,generation);
put64(out+8,wasted_bytes);
put32(out+16,static_cast<std::uint32_t>(metadata.size()));
out+=20;
if(!metadata.empty())std::memcpy(out,metadata.data(),metadata.size());
out+=metadata.size();
put32(out,count);
return out+4;
}

std::byte* write_entry(std::byte* out,const IndexEntry& entry) noexcept
{
put16(out,static_cast<std::uint16_t>(entry.name.size()));
out+=2;
std::memcpy(out,entry.name.data(),entry.name.size());
out+=entry.name.size();
out[0]=static_cast<std::byte>(entry.deleted?kEntryDeleted:0);
put64(out+1,entry.id);
put64(out+9,entry.size);
put64(out+17,entry.offset);
put64(out+25,static_cast<std::uint64_t>(entry.modified_utc_ms));
return out+33;
}

std::vector<std::byte> encode_index(const Index& index)
{
if(index.metadata.size()>std::numeric_limits<std::uint32_t>::max())throw std::length_error("the pack metadata is too long");
if(index.entries.size()>std::numeric_limits<std::uint32_t>::max())throw std::length_error("the pack has too many entries");
std::size_t size=index_head_size(index.metadata);
for(const IndexEntry& entry:index.entries){
if(entry.name.size()>kMaxNameLength)throw std::length_error("a pack entry name is too long");
size+=entry_size(entry);
}
std::vector<std::byte> out(size);
std::byte* at=write_index_head(out.data(),index.generation,index.wasted_bytes,index.metadata,static_cast<std::uint32_t>(index.entries.size()));
for(const IndexEntry& entry:index.entries)at=write_entry(at,entry);
return out;
}

std::optional<Index> decode_index(std::span<const std::byte> plain)
{
const std::byte* in=plain.data();
std::size_t left=plain.size();
if(left<kIndexFixedSize)return std::nullopt;
Index index;
index.generation=get64(in);
index.wasted_bytes=get64(in+8);
const std::uint32_t meta=get32(in+16);
in+=20;
left-=20;
// The count's 4 bytes follow the metadata.
if(meta>left-4)return std::nullopt;
index.metadata.assign(reinterpret_cast<const char*>(in),meta);
in+=meta;
left-=meta;
const std::uint32_t count=get32(in);
in+=4;
left-=4;
// Every entry takes at least its fixed fields, so a count the bytes cannot hold allocates nothing.
if(count>left/kEntryFixedSize)return std::nullopt;
index.entries.resize(count);
for(IndexEntry& entry:index.entries){
if(left<kEntryFixedSize)return std::nullopt;
const std::size_t name=get16(in);
if(name>left-kEntryFixedSize)return std::nullopt;
entry.name.assign(reinterpret_cast<const char*>(in+2),name);
in+=2+name;
const auto flags=std::to_integer<std::uint8_t>(in[0]);
if((flags&~kEntryDeleted)!=0)return std::nullopt;
entry.deleted=flags!=0;
entry.id=get64(in+1);
entry.size=get64(in+9);
entry.offset=get64(in+17);
entry.modified_utc_ms=static_cast<std::int64_t>(get64(in+25));
in+=33;
left-=kEntryFixedSize+name;
}
if(left!=0)return std::nullopt;
return index;
}

std::optional<std::string> canonical_entry_name(std::string_view name)
{
auto parsed=vfs::parse_path(name,false);
if(!parsed||!parsed->mount.empty()||parsed->folder||parsed->relative.empty()||parsed->relative.size()>kMaxNameLength)return std::nullopt;
return std::move(parsed->relative);
}

bool validate_index(const Index& index,std::uint8_t chunk_log2,std::uint64_t data_end,std::vector<std::string>* folds)
{
std::unordered_set<std::string> seen_folds;
std::unordered_set<std::uint64_t> seen_ids;
seen_folds.reserve(index.entries.size());
seen_ids.reserve(index.entries.size());
if(folds){
folds->clear();
folds->reserve(index.entries.size());
}
for(const IndexEntry& entry:index.entries){
const auto canonical=canonical_entry_name(entry.name);
if(!canonical||*canonical!=entry.name)return false;
std::string fold=vfs::fold_name(entry.name);
if(entry.deleted){
if(entry.id!=0||entry.size!=0||entry.offset!=0)return false;
}
else{
if(entry.id==0||!seen_ids.insert(entry.id).second)return false;
const auto stored=stored_size(entry.size,chunk_log2);
if(!stored||entry.offset<kHeaderSize||entry.offset>data_end||*stored>data_end-entry.offset)return false;
}
if(folds)folds->push_back(fold);
if(!seen_folds.insert(std::move(fold)).second)return false;
}
return true;
}

FooterBytes encode_footer(const Footer& footer) noexcept
{
FooterBytes out{};
put64(out.data(),footer.index_offset);
put64(out.data()+8,footer.index_length);
std::memcpy(out.data()+kNonceAt,footer.nonce.data(),footer.nonce.size());
put32(out.data()+kCrcAt,crc_of(out.data(),kCrcAt));
std::memcpy(out.data()+kFooterMagicAt,kFooterMagic.data(),kFooterMagic.size());
return out;
}

FooterState inspect_footer(std::span<const std::byte,kFooterSize> bytes,std::uint64_t footer_offset,Footer& footer) noexcept
{
const std::byte* in=bytes.data();
if(std::memcmp(in+kFooterMagicAt,kFooterMagic.data(),kFooterMagic.size())!=0)return FooterState::none;
const std::uint64_t offset=get64(in);
const std::uint64_t length=get64(in+8);
const bool placed=offset>=kHeaderSize&&length>=kMinIndexLength&&offset<=footer_offset&&length==footer_offset-offset;
if(crc_of(in,kCrcAt)!=get32(in+kCrcAt)||!placed)return FooterState::marked;
footer.index_offset=offset;
footer.index_length=length;
std::memcpy(footer.nonce.data(),in+kNonceAt,footer.nonce.size());
return FooterState::complete;
}
}
