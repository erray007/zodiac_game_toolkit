#include <doctest/doctest.h>
#include <zodiac/plugin.h>
#include <cstddef>
#include <string>

extern "C" const zodiac_module_info* zodiac_test_get_c_module_info(void);
extern "C" const zodiac_module_info* zodiac_module_get_info(void);
extern "C" int zodiac_test_c_host_has_vfs(const zodiac_host* host);

namespace {
// The ABI v1 layout for 64-bit targets, derived from the member order: two uint32_t fields, then 8-byte pointers. Mirrored in test_plugin_header_c.c.
constexpr std::size_t kPointer=8;
constexpr std::size_t kHeader=8;
constexpr std::size_t host_function(std::size_t index){return kHeader+index*kPointer;}
}

static_assert(sizeof(void*)==kPointer,"the ABI layout checks cover 64-bit targets");
static_assert(sizeof(void (*)(void))==kPointer,"function pointers must be 8 bytes");
static_assert(sizeof(zodiac_log_level)==4);

static_assert(offsetof(zodiac_module_info,abi_version)==0);
static_assert(offsetof(zodiac_module_info,struct_size)==4);
static_assert(offsetof(zodiac_module_info,name)==kHeader);
static_assert(offsetof(zodiac_module_info,version)==kHeader+1*kPointer);
static_assert(offsetof(zodiac_module_info,license)==kHeader+2*kPointer);
static_assert(offsetof(zodiac_module_info,dependencies)==kHeader+3*kPointer);
static_assert(offsetof(zodiac_module_info,dependency_count)==kHeader+4*kPointer);
// dependency_count (4 bytes) is followed by 4 bytes of padding so the next pointer is 8-byte aligned.
static_assert(offsetof(zodiac_module_info,register_module)==kHeader+5*kPointer);
static_assert(offsetof(zodiac_module_info,shutdown_module)==kHeader+6*kPointer);
// Appended after ABI v1: link_modes (4 bytes), then 4 bytes of tail padding so the struct stays 8-byte aligned.
static_assert(offsetof(zodiac_module_info,link_modes)==kHeader+7*kPointer);
static_assert(sizeof(zodiac_module_info)==kHeader+8*kPointer);

static_assert(offsetof(zodiac_host,abi_version)==0);
static_assert(offsetof(zodiac_host,struct_size)==4);
static_assert(offsetof(zodiac_host,register_function)==host_function(0));
static_assert(offsetof(zodiac_host,register_native_function)==host_function(1));
static_assert(offsetof(zodiac_host,register_ref_type)==host_function(2));
static_assert(offsetof(zodiac_host,register_method)==host_function(3));
static_assert(offsetof(zodiac_host,register_factory)==host_function(4));
static_assert(offsetof(zodiac_host,call_user_data)==host_function(5));
static_assert(offsetof(zodiac_host,call_object)==host_function(6));
static_assert(offsetof(zodiac_host,arg_int32)==host_function(7));
static_assert(offsetof(zodiac_host,arg_int64)==host_function(8));
static_assert(offsetof(zodiac_host,arg_float)==host_function(9));
static_assert(offsetof(zodiac_host,arg_double)==host_function(10));
static_assert(offsetof(zodiac_host,arg_bool)==host_function(11));
static_assert(offsetof(zodiac_host,arg_string)==host_function(12));
static_assert(offsetof(zodiac_host,arg_object)==host_function(13));
static_assert(offsetof(zodiac_host,arg_array)==host_function(14));
static_assert(offsetof(zodiac_host,ret_int32)==host_function(15));
static_assert(offsetof(zodiac_host,ret_int64)==host_function(16));
static_assert(offsetof(zodiac_host,ret_float)==host_function(17));
static_assert(offsetof(zodiac_host,ret_double)==host_function(18));
static_assert(offsetof(zodiac_host,ret_bool)==host_function(19));
static_assert(offsetof(zodiac_host,ret_string)==host_function(20));
static_assert(offsetof(zodiac_host,ret_object)==host_function(21));
static_assert(offsetof(zodiac_host,ret_array)==host_function(22));
static_assert(offsetof(zodiac_host,array_length)==host_function(23));
static_assert(offsetof(zodiac_host,array_get_string)==host_function(24));
static_assert(offsetof(zodiac_host,array_get_int32)==host_function(25));
static_assert(offsetof(zodiac_host,array_get_double)==host_function(26));
static_assert(offsetof(zodiac_host,array_push_string)==host_function(27));
static_assert(offsetof(zodiac_host,array_push_int32)==host_function(28));
static_assert(offsetof(zodiac_host,array_push_double)==host_function(29));
static_assert(offsetof(zodiac_host,set_exception)==host_function(30));
static_assert(offsetof(zodiac_host,log)==host_function(31));
// Appended after ABI v1: the VFS entries.
static_assert(offsetof(zodiac_host,vfs_open)==host_function(32));
static_assert(offsetof(zodiac_host,vfs_read)==host_function(33));
static_assert(offsetof(zodiac_host,vfs_seek)==host_function(34));
static_assert(offsetof(zodiac_host,vfs_size)==host_function(35));
static_assert(offsetof(zodiac_host,vfs_close)==host_function(36));
static_assert(offsetof(zodiac_host,vfs_exists)==host_function(37));
static_assert(sizeof(zodiac_host)==host_function(38));

TEST_CASE("abi constants are stable")
{
CHECK(ZODIAC_ABI_VERSION == 1);
CHECK(std::string(ZODIAC_MODULE_ENTRY_NAME) == "zodiac_module_get_info");
CHECK(ZODIAC_LINK_STATIC == 1u);
CHECK(ZODIAC_LINK_DYNAMIC == 2u);
CHECK(ZODIAC_OK == 0);
CHECK(ZODIAC_E_INVALID_ARGUMENT == -1);
CHECK(ZODIAC_E_DECLARATION == -2);
CHECK(ZODIAC_E_NOT_PRIMITIVE == -3);
CHECK(ZODIAC_E_ALREADY_REGISTERED == -4);
CHECK(ZODIAC_E_UNKNOWN_TYPE == -5);
CHECK(ZODIAC_E_NOT_FOUND == -6);
CHECK(ZODIAC_E_IO == -7);
CHECK(ZODIAC_E_DAMAGED == -8);
CHECK(ZODIAC_E_BAD_PATH == -9);
CHECK(ZODIAC_E_UNAVAILABLE == -10);
CHECK(ZODIAC_SEEK_SET == 0);
CHECK(ZODIAC_SEEK_CUR == 1);
CHECK(ZODIAC_SEEK_END == 2);
}

TEST_CASE("ZODIAC_HOST_HAS follows struct_size")
{
zodiac_host host{};
host.struct_size=static_cast<uint32_t>(host_function(32));
CHECK(ZODIAC_HOST_HAS(&host,log));
CHECK_FALSE(ZODIAC_HOST_HAS(&host,vfs_open));
host.struct_size=static_cast<uint32_t>(host_function(37));
CHECK(ZODIAC_HOST_HAS(&host,vfs_close));
CHECK_FALSE(ZODIAC_HOST_HAS(&host,vfs_exists));
CHECK(zodiac_test_c_host_has_vfs(&host)==0);
host.struct_size=sizeof(zodiac_host);
CHECK(ZODIAC_HOST_HAS(&host,vfs_exists));
CHECK(zodiac_test_c_host_has_vfs(&host)==1);
}

TEST_CASE("module info defined in C is exported through the entry point")
{
const zodiac_module_info* info = zodiac_test_get_c_module_info();
REQUIRE(info != nullptr);
CHECK(std::string(info->name) == "plugin_header_c");
CHECK(info->abi_version == ZODIAC_ABI_VERSION);
CHECK(info->struct_size == sizeof(zodiac_module_info));
CHECK(zodiac_module_get_info() == info);
}
