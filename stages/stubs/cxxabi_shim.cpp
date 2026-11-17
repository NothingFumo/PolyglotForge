/* Kotlin 归档引用了 libstdc++ 的 RTTI vtable；本工程不回抛 C++ 异常，故提供占位定义，
   避免把整个 libstdc++/winpthreads 拉进纯静态链接。 */
#include <cstddef>
namespace __cxxabiv1 {
class __class_type_info { public: virtual ~__class_type_info(); };
class __si_class_type_info : public __class_type_info { public: ~__si_class_type_info() override; };
class __vmi_class_type_info : public __class_type_info { public: ~__vmi_class_type_info() override; };
__class_type_info::~__class_type_info() {}
__si_class_type_info::~__si_class_type_info() {}
__vmi_class_type_info::~__vmi_class_type_info() {}
}
