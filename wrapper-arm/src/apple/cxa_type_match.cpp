// ARM EHABI exception matching for the guest.
//
// Apple's 3.6.0 libraries run their own C++ try/catch across the auth flow;
// their objects import the EHABI personality helper __cxa_type_match, which
// the NDK's libc++abi.a for arm does not carry. The EHABI contract:
//
//   int __cxa_type_match(_Unwind_Context* context, void* catch_type_info,
//                        void* thrown_object, void** adjusted_thrown_object);
//
// We match on the type_info NAME: exact equality catches. Apple's exception
// hierarchy in the auth flow is flat (SVError and friends are caught by
// value/reference of their own type), so the derived-class walk the full
// libc++abi can_catch() performs is not needed for the handlers in play.
#include <cstddef>
#include <cstring>
#include <typeinfo>
#include <unwind.h>

namespace {

enum { no_match = 0, catch_match = 1, catch_const_match = 3 };

const std::type_info* type_of_thrown(void* thrown_object) {
    if (!thrown_object) {
        return nullptr;
    }
    // The exception header precedes the thrown object; the first word of the
    // Itanium/EHABI layout is the type_info pointer.
    return *reinterpret_cast<const std::type_info**>(
        reinterpret_cast<uintptr_t>(thrown_object) - sizeof(void*));
}

}  // namespace

extern "C" int __cxa_type_match(_Unwind_Context* /*context*/, void* catch_type_info,
                                void* thrown_object, void** adjusted_thrown_object) {
    if (adjusted_thrown_object) {
        *adjusted_thrown_object = thrown_object;
    }
    const std::type_info* catch_type =
        reinterpret_cast<const std::type_info*>(catch_type_info);
    const std::type_info* thrown_type = type_of_thrown(thrown_object);
    if (!catch_type || !thrown_type) {
        return no_match;
    }
    // std::type_info::operator== walks the (possibly type-erased) vtables of
    // two runtimes; compare the mangling strings instead.
    const char* cn = catch_type->name();
    const char* tn = thrown_type->name();
    if (cn && tn && strcmp(cn, tn) == 0) {
        return catch_match;
    }
    return no_match;
}
