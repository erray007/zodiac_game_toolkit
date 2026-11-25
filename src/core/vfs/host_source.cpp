#include <core/vfs/host_source.hpp>
#include <core/path_utf8.hpp>
#include <core/vfs/folder_source.hpp>
#include <core/vfs/path.hpp>
#include <platform/file.hpp>

namespace zodiac::vfs {
namespace fs=std::filesystem;

std::expected<EntryInfo,FileError> HostSource::stat(std::string_view relative)
{
const fs::path path=path_from_utf8(relative);
const auto node=platform::stat_path(path);
if(!node)return std::unexpected(detail::file_error(node.error()));
EntryInfo info;
info.kind=detail::entry_kind(node->kind);
if(info.kind==EntryKind::none)return std::unexpected(FileError::not_found);
info.size=node->size;
info.modified_utc_ms=detail::to_ms(node->modified_utc_ns);
info.name=path_to_utf8(path.filename());
return info;
}

std::expected<std::shared_ptr<Reader>,FileError> HostSource::open_read(std::string_view relative)
{
auto file=platform::File::open(path_from_utf8(relative),platform::FileAccess::read);
if(!file)return std::unexpected(detail::file_error(file.error()));
return detail::make_file_reader(std::move(*file));
}

std::expected<std::unique_ptr<WriteTarget>,FileError> HostSource::open_write(std::string_view relative,WriteMode mode)
{
auto file=platform::File::open(path_from_utf8(relative),detail::access_of(mode));
if(!file)return std::unexpected(detail::file_error(file.error()));
return detail::make_file_writer(std::move(*file));
}

std::expected<void,FileError> HostSource::list(std::string_view relative,std::vector<EntryInfo>& out)
{
std::vector<platform::DirectoryEntry> entries;
if(const std::error_code error=platform::list_directory(path_from_utf8(relative),entries))return std::unexpected(detail::file_error(error));
out.reserve(out.size()+entries.size());
for(platform::DirectoryEntry& entry:entries){
const EntryKind kind=detail::entry_kind(entry.kind);
if(kind==EntryKind::none||!valid_name(entry.name))continue;
EntryInfo& info=out.emplace_back();
info.kind=kind;
info.name=std::move(entry.name);
}
return {};
}

std::expected<void,FileError> HostSource::remove(std::string_view relative,bool)
{
if(const std::error_code error=platform::remove_file(path_from_utf8(relative)))return std::unexpected(detail::file_error(error));
return {};
}

std::expected<void,FileError> HostSource::make_directory(std::string_view relative)
{
const fs::path path=path_from_utf8(relative);
const std::error_code error=platform::make_directory(path);
if(!error)return {};
if(platform::error_kind(error)==platform::ErrorKind::exists){
const auto node=platform::stat_path(path);
if(node&&node->kind==platform::NodeKind::directory)return {};
return std::unexpected(FileError::not_a_directory);
}
return std::unexpected(detail::file_error(error));
}

std::expected<void,FileError> HostSource::make_directories(std::string_view relative)
{
if(const std::error_code error=detail::make_directories(path_from_utf8(relative)))return std::unexpected(detail::file_error(error));
return {};
}

std::expected<bool,MoveError> HostSource::rename(std::string_view from,std::string_view to)
{
const fs::path source=path_from_utf8(from);
const auto node=platform::stat_path(source);
if(!node)return std::unexpected(MoveError{detail::file_error(node.error()),false});
if(node->kind==platform::NodeKind::directory)return std::unexpected(MoveError{FileError::is_directory,false});
if(node->kind!=platform::NodeKind::file)return std::unexpected(MoveError{FileError::not_found,false});
const fs::path target=path_from_utf8(to);
if(const auto existing=platform::stat_path(target);existing&&existing->kind==platform::NodeKind::directory)return std::unexpected(MoveError{FileError::is_directory,true});
std::error_code error;
if(platform::replace_file(source,target,error))return true;
if(platform::error_kind(error)==platform::ErrorKind::cross_device)return false;
return std::unexpected(MoveError{detail::file_error(error),platform::stat_path(source).has_value()});
}

std::expected<std::optional<fs::path>,FileError> HostSource::host_file(std::string_view relative,bool for_write)
{
fs::path path=path_from_utf8(relative);
const auto node=platform::stat_path(path);
if(node){
if(node->kind==platform::NodeKind::directory)return std::unexpected(FileError::is_directory);
if(node->kind!=platform::NodeKind::file)return std::unexpected(for_write?FileError::access_denied:FileError::not_found);
}
else if(!for_write||platform::error_kind(node.error())!=platform::ErrorKind::not_found)return std::unexpected(detail::file_error(node.error()));
return path;
}

std::expected<void,FileError> HostSource::remove_directory(std::string_view relative,bool recursive)
{
const fs::path path=path_from_utf8(relative);
// A drive or share root is never removed.
if(!path.has_relative_path())return std::unexpected(FileError::access_denied);
const auto node=platform::stat_path(path);
if(!node)return std::unexpected(detail::file_error(node.error()));
if(node->kind!=platform::NodeKind::directory)return std::unexpected(FileError::not_a_directory);
if(const std::error_code error=recursive?detail::remove_tree(path):platform::remove_directory(path))return std::unexpected(detail::file_error(error));
return {};
}
}
