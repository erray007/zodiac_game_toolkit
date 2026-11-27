#include <zodiac/plugin.h>
#include <stddef.h>

/* The ABI v1 layout for 64-bit targets, derived from the member order: two uint32_t fields, then 8-byte pointers. Mirrors test_plugin_header.cpp. */
#define ZT_PTR 8
#define ZT_HEADER 8
#define ZT_HOST_FN(index) (ZT_HEADER+(index)*ZT_PTR)
_Static_assert(sizeof(void*)==ZT_PTR,"the ABI layout checks cover 64-bit targets");
_Static_assert(sizeof(void (*)(void))==ZT_PTR,"function pointers must be 8 bytes");
_Static_assert(sizeof(zodiac_log_level)==4,"zodiac_log_level is 4 bytes");

_Static_assert(offsetof(zodiac_module_info,abi_version)==0,"zodiac_module_info.abi_version");
_Static_assert(offsetof(zodiac_module_info,struct_size)==4,"zodiac_module_info.struct_size");
_Static_assert(offsetof(zodiac_module_info,name)==ZT_HEADER,"zodiac_module_info.name");
_Static_assert(offsetof(zodiac_module_info,version)==ZT_HEADER+1*ZT_PTR,"zodiac_module_info.version");
_Static_assert(offsetof(zodiac_module_info,license)==ZT_HEADER+2*ZT_PTR,"zodiac_module_info.license");
_Static_assert(offsetof(zodiac_module_info,dependencies)==ZT_HEADER+3*ZT_PTR,"zodiac_module_info.dependencies");
_Static_assert(offsetof(zodiac_module_info,dependency_count)==ZT_HEADER+4*ZT_PTR,"zodiac_module_info.dependency_count");
/* dependency_count (4 bytes) is followed by 4 bytes of padding so the next pointer is 8-byte aligned. */
_Static_assert(offsetof(zodiac_module_info,register_module)==ZT_HEADER+5*ZT_PTR,"zodiac_module_info.register_module");
_Static_assert(offsetof(zodiac_module_info,shutdown_module)==ZT_HEADER+6*ZT_PTR,"zodiac_module_info.shutdown_module");
/* Appended after ABI v1: link_modes (4 bytes), then 4 bytes of tail padding so the struct stays 8-byte aligned. */
_Static_assert(offsetof(zodiac_module_info,link_modes)==ZT_HEADER+7*ZT_PTR,"zodiac_module_info.link_modes");
_Static_assert(sizeof(zodiac_module_info)==ZT_HEADER+8*ZT_PTR,"sizeof(zodiac_module_info)");

_Static_assert(offsetof(zodiac_host,abi_version)==0,"zodiac_host.abi_version");
_Static_assert(offsetof(zodiac_host,struct_size)==4,"zodiac_host.struct_size");
_Static_assert(offsetof(zodiac_host,register_function)==ZT_HOST_FN(0),"zodiac_host.register_function");
_Static_assert(offsetof(zodiac_host,register_native_function)==ZT_HOST_FN(1),"zodiac_host.register_native_function");
_Static_assert(offsetof(zodiac_host,register_ref_type)==ZT_HOST_FN(2),"zodiac_host.register_ref_type");
_Static_assert(offsetof(zodiac_host,register_method)==ZT_HOST_FN(3),"zodiac_host.register_method");
_Static_assert(offsetof(zodiac_host,register_factory)==ZT_HOST_FN(4),"zodiac_host.register_factory");
_Static_assert(offsetof(zodiac_host,call_user_data)==ZT_HOST_FN(5),"zodiac_host.call_user_data");
_Static_assert(offsetof(zodiac_host,call_object)==ZT_HOST_FN(6),"zodiac_host.call_object");
_Static_assert(offsetof(zodiac_host,arg_int32)==ZT_HOST_FN(7),"zodiac_host.arg_int32");
_Static_assert(offsetof(zodiac_host,arg_int64)==ZT_HOST_FN(8),"zodiac_host.arg_int64");
_Static_assert(offsetof(zodiac_host,arg_float)==ZT_HOST_FN(9),"zodiac_host.arg_float");
_Static_assert(offsetof(zodiac_host,arg_double)==ZT_HOST_FN(10),"zodiac_host.arg_double");
_Static_assert(offsetof(zodiac_host,arg_bool)==ZT_HOST_FN(11),"zodiac_host.arg_bool");
_Static_assert(offsetof(zodiac_host,arg_string)==ZT_HOST_FN(12),"zodiac_host.arg_string");
_Static_assert(offsetof(zodiac_host,arg_object)==ZT_HOST_FN(13),"zodiac_host.arg_object");
_Static_assert(offsetof(zodiac_host,arg_array)==ZT_HOST_FN(14),"zodiac_host.arg_array");
_Static_assert(offsetof(zodiac_host,ret_int32)==ZT_HOST_FN(15),"zodiac_host.ret_int32");
_Static_assert(offsetof(zodiac_host,ret_int64)==ZT_HOST_FN(16),"zodiac_host.ret_int64");
_Static_assert(offsetof(zodiac_host,ret_float)==ZT_HOST_FN(17),"zodiac_host.ret_float");
_Static_assert(offsetof(zodiac_host,ret_double)==ZT_HOST_FN(18),"zodiac_host.ret_double");
_Static_assert(offsetof(zodiac_host,ret_bool)==ZT_HOST_FN(19),"zodiac_host.ret_bool");
_Static_assert(offsetof(zodiac_host,ret_string)==ZT_HOST_FN(20),"zodiac_host.ret_string");
_Static_assert(offsetof(zodiac_host,ret_object)==ZT_HOST_FN(21),"zodiac_host.ret_object");
_Static_assert(offsetof(zodiac_host,ret_array)==ZT_HOST_FN(22),"zodiac_host.ret_array");
_Static_assert(offsetof(zodiac_host,array_length)==ZT_HOST_FN(23),"zodiac_host.array_length");
_Static_assert(offsetof(zodiac_host,array_get_string)==ZT_HOST_FN(24),"zodiac_host.array_get_string");
_Static_assert(offsetof(zodiac_host,array_get_int32)==ZT_HOST_FN(25),"zodiac_host.array_get_int32");
_Static_assert(offsetof(zodiac_host,array_get_double)==ZT_HOST_FN(26),"zodiac_host.array_get_double");
_Static_assert(offsetof(zodiac_host,array_push_string)==ZT_HOST_FN(27),"zodiac_host.array_push_string");
_Static_assert(offsetof(zodiac_host,array_push_int32)==ZT_HOST_FN(28),"zodiac_host.array_push_int32");
_Static_assert(offsetof(zodiac_host,array_push_double)==ZT_HOST_FN(29),"zodiac_host.array_push_double");
_Static_assert(offsetof(zodiac_host,set_exception)==ZT_HOST_FN(30),"zodiac_host.set_exception");
_Static_assert(offsetof(zodiac_host,log)==ZT_HOST_FN(31),"zodiac_host.log");
/* Appended after ABI v1: the VFS entries. */
_Static_assert(offsetof(zodiac_host,vfs_open)==ZT_HOST_FN(32),"zodiac_host.vfs_open");
_Static_assert(offsetof(zodiac_host,vfs_read)==ZT_HOST_FN(33),"zodiac_host.vfs_read");
_Static_assert(offsetof(zodiac_host,vfs_seek)==ZT_HOST_FN(34),"zodiac_host.vfs_seek");
_Static_assert(offsetof(zodiac_host,vfs_size)==ZT_HOST_FN(35),"zodiac_host.vfs_size");
_Static_assert(offsetof(zodiac_host,vfs_close)==ZT_HOST_FN(36),"zodiac_host.vfs_close");
_Static_assert(offsetof(zodiac_host,vfs_exists)==ZT_HOST_FN(37),"zodiac_host.vfs_exists");
_Static_assert(sizeof(zodiac_host)==ZT_HOST_FN(38),"sizeof(zodiac_host)");
_Static_assert(ZODIAC_E_NOT_FOUND==-6&&ZODIAC_E_IO==-7&&ZODIAC_E_DAMAGED==-8&&ZODIAC_E_BAD_PATH==-9&&ZODIAC_E_UNAVAILABLE==-10,"VFS error codes");
_Static_assert(ZODIAC_SEEK_SET==0&&ZODIAC_SEEK_CUR==1&&ZODIAC_SEEK_END==2,"seek origins");

static const zodiac_module_info zodiac_test_module_info = {
.abi_version = ZODIAC_ABI_VERSION,
.struct_size = sizeof(zodiac_module_info),
.name = "plugin_header_c",
.version = "0.1.0",
.license = "Zlib",
.dependencies = NULL,
.dependency_count = 0,
.register_module = NULL,
.shutdown_module = NULL,
.link_modes = ZODIAC_LINK_STATIC|ZODIAC_LINK_DYNAMIC,
};

ZODIAC_DEFINE_MODULE(zodiac_test_module_info)

const zodiac_module_info* zodiac_test_get_c_module_info(void)
{
return &zodiac_test_module_info;
}

/* ZODIAC_HOST_HAS compiles as C (-pedantic-errors). */
int zodiac_test_c_host_has_vfs(const zodiac_host* host)
{
return ZODIAC_HOST_HAS(host,vfs_exists)?1:0;
}
