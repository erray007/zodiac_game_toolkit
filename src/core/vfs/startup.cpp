#include <core/vfs/startup.hpp>
#include <core/path_utf8.hpp>
#include <core/project_file.hpp>
#include <core/text.hpp>
#include <core/vfs/folder_source.hpp>
#include <core/vfs/path.hpp>
#include <platform/file.hpp>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace zodiac::vfs {
namespace {
// The dev mode release pack check of mount_project (startup.hpp): what zodiac build would put into a read-only pack.
// Every pattern and name is folded (fold_name), and so is the path asked about: the check matches as the VFS looks names up, so a
// write spelled "Sounds/a.ogg" counts for "sounds/**" ('*', '?' and '/' fold to themselves).
class ReleasePackCheck{
public:
explicit ReleasePackCheck(const Project& project):zpk_(fold_name(".zpk"))
{
for(const std::string& pattern:project.build.assets)assets_.push_back(fold_name(pattern));
for(const std::string& pattern:project.pack.exclude)exclude_.push_back(fold_name(pattern));
// expand_assets never ships these, whatever the patterns say: zodiac.toml, the project file, release/ and .zodiac/ in the root.
skipped_files_.push_back(fold_name("zodiac.toml"));
if(!project.file.empty())skipped_files_.push_back(fold_name(path_to_utf8(project.file.filename())));
skipped_folders_.push_back(fold_name("release"));
skipped_folders_.push_back(fold_name(".zodiac"));
}
bool operator()(std::string_view relative) const
{
// Folded on the stack for every usual path; a longer one into a string.
char buffer[1024];
std::string long_fold;
std::size_t length=fold_into(relative,buffer,sizeof buffer);
std::string_view fold;
if(length==SIZE_MAX){
long_fold=fold_name(relative);
fold=long_fold;
}
else fold=std::string_view(buffer,length);
const auto matches=[fold](const std::string& pattern){return glob_match(pattern,fold);};
if(std::ranges::none_of(assets_,matches)||std::ranges::any_of(exclude_,matches)||fold.ends_with(zpk_))return false;
const std::size_t cut=fold.find('/');
const std::string_view first=fold.substr(0,cut);
const std::vector<std::string>& skipped=cut==std::string_view::npos?skipped_files_:skipped_folders_;
return std::ranges::find(skipped,first)==skipped.end();
}
private:
std::vector<std::string> assets_;
std::vector<std::string> exclude_;
std::string zpk_;
std::vector<std::string> skipped_files_;
std::vector<std::string> skipped_folders_;
};
}

void mount_game_folders(Vfs& vfs,const std::filesystem::path& root,std::string_view game_name,bool host_paths,bool dev_mode)
{
vfs.set_host_paths(host_paths);
vfs.set_dev_mode(dev_mode);
vfs.mount("/",std::make_shared<FolderSource>(root,false),true,nullptr);
const std::filesystem::path data=platform::user_data_folder();
std::string reason;
if(data.empty())reason="no per-user data folder was found";
else if(!valid_name(game_name))reason="the game name \""+one_line(game_name)+"\" cannot be used as a folder name";
if(!reason.empty()){
if(const auto& log=vfs.log())log(ZODIAC_LOG_WARNING,"warning: data:/ is not available: "+reason);
return;
}
vfs.mount("data:/",std::make_shared<FolderSource>(data/path_from_utf8(game_name),false,true),true,nullptr);
}

void mount_project(Vfs& vfs,const Project& project,bool dev_mode)
{
mount_game_folders(vfs,project.root,project.name,project.files.host_paths,dev_mode);
// Only a release whose pack is read only refuses writes to its names; with no asset, packing off or a writable pack nothing is checked.
if(dev_mode&&project.pack.enabled&&!project.pack.writable&&!project.build.assets.empty())vfs.set_release_pack_check(ReleasePackCheck(project));
}
}
