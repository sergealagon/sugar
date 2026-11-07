/*
    SPDX-License-Identifier: MIT
    Copyright (c) 2026 Serge Alagon
*/

/*     
    // example usage:
    #define SUGAR_SHORT_NAMES // optional: unprefixed aliases (OBJC_HOOK, ORIG, CLS, SEL_C, DECL, ...)
    #include "sugar.hpp" // needs: Objective-C++ (.mm), C++17, arm64 / arm64e
 
    
    // OBJC_HOOK(Name, "Class", "selector:", isClassMethod, ReturnType, extra args...)
    // `self` and `_cmd` are always available. Extra args have their REAL types,
    // and Name.orig(...) takes the same types (so no int/NSInteger or float/CGFloat mixups).
 
    OBJC_HOOK(ViewDidLoad, "UIViewController", "viewDidLoad", false, void) {
        ORIG(ViewDidLoad); // same as ViewDidLoad.orig(self, _cmd);
    } OBJC_HOOK_END;
 
    OBJC_HOOK(SetAlpha, "UIView", "setAlpha:", false, void, CGFloat a) {
        ORIG(SetAlpha, a  0.5);
    } OBJC_HOOK_END;
 
    // class method -> isClassMethod = true
    OBJC_HOOK(SharedInst, "SBFoo", "sharedInstance", true, id) {
        return ORIG(SharedInst);
    } OBJC_HOOK_END;
 
    // method might not exist on every iOS version -> no error log if missing
    OBJC_HOOK_OPTIONAL(NewThing, "SBBar", "newThing", false, void) {
        ORIG(NewThing);
    } OBJC_HOOK_END;
 
    // class lives in a framework that loads later -> retried whenever a new image is loaded
    OBJC_HOOK_DEFERRED(LateHook, "SomeLazyClass", "doStuff", false, void) {
        ORIG(LateHook);
    } OBJC_HOOK_END;
 
    // custom flags: OBJC_HOOK_EX(sugar::kHookOptional | sugar::kHookDeferred, Name, ...)
    
    // calling methods / instance methods with arbitrary args and return types:
    invoke<void>(self, "someMethod:", someArg);
    id v = invoke<id>(self, SEL_C("valueForKey:"), @"foo"); // SEL_C = selector cached per call site
 
    // Integers of any width are widened automatically, so literals are fine:
    invoke<void>(self, "setCount:", -1);
    // Floating point can't be guessed (float vs double, int vs CGFloat): say the REAL parameter
    // types and the args get converted to them.
    invoke_as<void(NSInteger, CGFloat)>(self, "setCount:alpha:", 5, 1.0);
 
    // ownership rules:
    // alloc / new / copy / mutableCopy return +1, and so does init (it consumes the
    // alloc'd receiver and returns +1). You own those, ARC does not know about them.
    // Everything else (plain getters etc.) is +0, don't release it.
    // Either release manually, wrap in ObjCObject, or hand it back to noah's arc :p
    // id a = invoke_owned(CLS("NSObject"), "new"); // +1 result handed to ARC
    // id b = new_instance("NSObject"); // alloc + init, ARC owns it
    // id c = new_instance("MyClass", "initWithFoo:", someFoo);
    // by hand: CFBridgingRelease(ptr) / (__bridge_transfer id)ptr on a +1 pointer does the same
 
    auto obj = sugar::ObjCObject::adopt(invoke<id>(invoke<id>(CLS("NSObject"), "alloc"), "init")); // owns +1
    obj.call<void>("doThing");
    id arcOwned = obj.into_arc(); // give ownership back to ARC (obj becomes empty)
 
    auto borrowed = sugar::ObjCObject::retain(someGetterResult); // +0 -> retains, releases on scope exit
 
    // lazy @interface for a class that may not exist at runtime. Use H_START / H_END to declare a REAL @interface
    H_START(SpringBoard)
    - (void)_simulateHomeButtonPress;
    - (void)applicationOpenURL:(id)url publicURLsOnly:(BOOL)only;
    H_END
 
    // category on a class whose real @interface is already visible (UIKit/Foundation):
    DECL(UIView)
    - (void)_privateThing;
    DECL_END
    
    // ivar helpers
    ivar<BOOL>(self, "_isActive") = YES; // traps (and logs) if the ivar doesn't exist
    if (auto p = ivar_ptr<void >(self, "_ctx")) { ... } // nullptr if missing
    // with ARC use ivar<__unsafe_unretained id>(...) / ivar<void >(...) for object ivars
 
    SUGAR_ASSOC_KEY(kMyKey);
    sugar::assoc_set_raw<int>(self, kMyKey, 42); // scalars/pointers (<= 8 bytes)
    int n = sugar::assoc_get_raw<int>(self, kMyKey); // 0 if unset
    sugar::assoc_set(self, kMyKey2, someObj); // objects (retain, nonatomic)
 
    // add a method to a class only if that class doesn't already implement it
    // OBJC_ADD_METHOD(Name, "Class", "selector:", isClassMethod, "v@:@", ReturnType, args...)
    OBJC_ADD_METHOD(AddedThing, "SBFoo", "myNewMethod:", false, "v@:@", void, id arg) {
        NSLog(@"hi %@", arg);
    }
 
    // debugging (built-in hooker only, not with substrate): Name.installed(), Name.restore()
 
    // compile time checks 
    #error unless: clang, Objective-C++, C++17, arm64/arm64e.
    OBJC_HOOK / OBJC_ADD_METHOD: static_assert that the number of ':' in the selector equals
    the number of extra arguments you declared, e.g. "setFoo:bar:" needs exactly 2.
    The selector must be a string literal for this (that is also what makes it checkable).
    A hook implementation must be RetType(id self, SEL _cmd, args...), otherwise a readable error.
    SEL_C / CLS only accept string literals (a variable would be cached forever).
    ivar<T> / assoc__raw<T>: static_assert on T (trivially copyable, pointer-sized for assoc).
    
    // backend stuff
    - If <substrate.h> is found (Substrate / ElleKit / libhooker compatible) it is used for MSHookMessageEx.
    - Otherwise a built-in class_replaceMethod based hooker is used.
    - Force the built-in one with #define SUGAR_BUILTIN_HOOKER before including.
    - Both paths check, before hooking, that the class and selector exist, and refuse to
      install if the method's current IMP already IS your replacement (that is what would
      make `orig` point at itself and recurse forever).
    - Silence logs with #define SUGAR_LOG_ENABLED 0
 */

#pragma once

#if __cplusplus < 201703L
#error "sugar.hpp requires C++17 (-std=c++17 or newer)"
#endif

#if !defined(__arm64__) && !defined(__aarch64__)
#error "sugar.hpp is arm64/arm64e only (no objc_msgSend_stret / objc_msgSend_fpret handling)"
#endif

#ifndef __clang__
#error "sugar.hpp requires clang"
#endif

#ifndef __OBJC__
#error "sugar.hpp must be compiled as Objective-C++ (.mm, or -x objective-c++)"
#endif

#include <objc/runtime.h>
#include <objc/message.h>
#include <objc/NSObject.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <os/log.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif

// backend stuff
#ifndef SUGAR_BACKEND_SUBSTRATE
    #if !defined(SUGAR_BUILTIN_HOOKER) && __has_include(<substrate.h>)
        #define SUGAR_BACKEND_SUBSTRATE 1
    #else
        #define SUGAR_BACKEND_SUBSTRATE 0
    #endif
#endif

#if SUGAR_BACKEND_SUBSTRATE
    #include <substrate.h>
#endif

// logging
#ifndef SUGAR_LOG_ENABLED
#define SUGAR_LOG_ENABLED 1
#endif

#if SUGAR_LOG_ENABLED
    #define SUGAR_LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "[sugar] " fmt, ##__VA_ARGS__)
#else
    #define SUGAR_LOG(...) ((void)0)
#endif

// ARC bridging helpers (the header works with and without ARC)
#if __has_feature(objc_arc)
    #define SUGAR_TO_PTR(x)   ((__bridge void *)(x))
    #define SUGAR_FROM_PTR(p) ((__bridge id)(p))
#else
    #define SUGAR_TO_PTR(x)   ((void *)(x))
    #define SUGAR_FROM_PTR(p) ((id)(p))
#endif


// same symbols as the runtime's objc_retain/objc_release, declared under other 
// names to avoid conflicts with the SDK's objc_retain/objc_release (which are not always available).
extern "C" {
    void *sugar_retain_raw(void *obj) __asm__("_objc_retain");
    void sugar_release_raw(void *obj) __asm__("_objc_release");
    void sugar_set_assoc_raw(void *obj, const void *key, void *value, uintptr_t policy) __asm__("_objc_setAssociatedObject");
    void *sugar_get_assoc_raw(void *obj, const void *key) __asm__("_objc_getAssociatedObject");
}

namespace sugar {

    // hook flags (bitmask) for OBJC_HOOK_EX
    enum : unsigned {
        kHookDefault = 0,
        kHookOptional = 1u << 0, // don't log an error if the class or selector is missing
        kHookDeferred = 1u << 1, // class not loaded yet? retry whenever a new image loads
    };


    // underlying integer type of an enum, or the type itself. Used to widen integral types to 64 bits for objc_msgSend.
    namespace detail {
        // This is used to widen integral types to 64 bits for objc_msgSend.
        template <typename T, bool IsEnum = std::is_enum_v<T>>
        struct IntBase { using type = T; };
        template <typename T>
        struct IntBase<T, true> { using type = std::underlying_type_t<T>; };

        /* 
            this is necessary because objc_msgSend expects integer arguments to be passed in 64-bit registers, even if they are smaller types. 
            this ensures that the callee receives the correct value regardless of the original type size.
        */
        template <typename T>
        using widen_t = std::conditional_t<
            std::is_integral_v<typename IntBase<T>::type>,
            std::conditional_t<std::is_signed_v<typename IntBase<T>::type>, long long, unsigned long long>,
            T>;
    }
    /* 
        invoke a method on an Objective-C object, bypassing ARC. 
        the return type is whatever you ask for, so ask for the real one (NSInteger, BOOL, CGFloat, ...). 
        variadic ObjC methods are not supported (arm64 passes variadics on the stack). 
        use invoke_as<> for floating point or other types that can't be guessed.
    */
    template <typename ReturnType, typename... Args>
    __attribute__((always_inline)) inline ReturnType invoke(id receiver, SEL sel, Args... args) {
        using MethodType = ReturnType (*)(id, SEL, detail::widen_t<Args>...);
        return reinterpret_cast<MethodType>(objc_msgSend)(receiver, sel, static_cast<detail::widen_t<Args>>(args)...);
    }

    /* 
        string selector overload. intentionally NOT cached (a static here would be shared by
        every call with the same <R, Args...>, regardless of the selector). for hot paths use SEL_C("foo:").
    */
    template <typename ReturnType, typename... Args>
    __attribute__((always_inline)) inline ReturnType invoke(id receiver, const char *selName, Args... args) {
        SEL sel = sel_registerName(selName);
        return invoke<ReturnType>(receiver, sel, args...);
    }

    namespace detail {
        template <typename Sig> struct InvokeAs;
        template <typename R, typename... A>
        struct InvokeAs<R(A...)> {
            template <typename... Given>
            __attribute__((always_inline)) static R call(id receiver, SEL sel, Given &&...given) {
                using MethodType = R (*)(id, SEL, A...);
                return reinterpret_cast<MethodType>(objc_msgSend)(receiver, sel, static_cast<A>(std::forward<Given>(given))...);
            }
        };
    }

    /* 
        invoke_as<void(NSInteger, CGFloat)>(obj, "setCount:alpha:", 5, 1.0)
        the signature (without id/SEL) is the REAL one; the arguments are converted to it
        (int -> CGFloat, float -> double, ...).nNeeded for floating point, optional for everything else. 
    */
    template <typename Sig, typename... Given>
    __attribute__((always_inline)) inline auto invoke_as(id receiver, SEL sel, Given &&...given)
        -> decltype(detail::InvokeAs<Sig>::call(receiver, sel, std::forward<Given>(given)...)) {
        return detail::InvokeAs<Sig>::call(receiver, sel, std::forward<Given>(given)...);
    }

    template <typename Sig, typename... Given>
    __attribute__((always_inline)) inline auto invoke_as(id receiver, const char *selName, Given &&...given)
        -> decltype(detail::InvokeAs<Sig>::call(receiver, sel_registerName(selName), std::forward<Given>(given)...)) {
        return detail::InvokeAs<Sig>::call(receiver, sel_registerName(selName), std::forward<Given>(given)...);
    }

    // invoke_owned / new_instance: +1 results handed to ARC
    namespace detail {

        // take a +1 pointer and make ARC the owner (CFBridgingRelease / __bridge_transfer). autoreleased under MRC
        inline id arc_adopt(id raw) {
            return raw ? CFBridgingRelease((CFTypeRef)SUGAR_TO_PTR(raw)) : nullptr;
        }

    }

    /* 
        for methods that return +1 (alloc, new, copy, mutableCopy, init..., or anything marked ns_returns_retained):
        the result is handed to ARC, so `id x = invoke_owned(cls, "new");` does not leak.
        only use it for the OUTERMOST call of a chain: init consumes its receiver, so `alloc` must stay raw
        (see new_instance for alloc + init). 
    */
    template <typename... Args>
    inline id invoke_owned(id receiver, SEL sel, Args... args) {
        __unsafe_unretained id raw = invoke<id>(receiver, sel, args...);
        return detail::arc_adopt(raw);
    }

    template <typename... Args>
    inline id invoke_owned(id receiver, const char *selName, Args... args) {
        return invoke_owned(receiver, sel_registerName(selName), args...);
    }


    /* 
        alloc + init in one call. The result is +1 and owned by ARC (autoreleased under MRC); nil if the class
        doesn't exist or init fails.
        id a = new_instance("NSObject");
        id b = new_instance("MyClass", "initWithFoo:bar:", foo, bar);
        id c = new_instance(CLS("MyClass"), "initWithFoo:", foo);
    */
    template <typename... Args>
    inline id new_instance(Class cls, const char *initSel, Args... args) {
        if (!cls) return nullptr;
        __unsafe_unretained id obj = invoke<id>((id)cls, "alloc"); /* +1 */
        obj = invoke<id>(obj, initSel, args...); /* init consumes the receiver and returns +1 */
        return detail::arc_adopt(obj);
    }

    inline id new_instance(Class cls) { return new_instance(cls, "init"); }

    template <typename... Args>
    inline id new_instance(const char *clsName, const char *initSel, Args... args) {
        Class cls = objc_getClass(clsName);
        if (!cls) {
            SUGAR_LOG("new_instance: class %{public}s not found", clsName);
            return nullptr;
        }
        return new_instance(cls, initSel, args...);
    }

    inline id new_instance(const char *clsName) { return new_instance(clsName, "init"); }

    // ObjCObject: owning RAII wrapper (manual retain/release, ARC-safe)
    struct ObjCObject {
        __unsafe_unretained id instance = nullptr;

        static ObjCObject adopt(id raw)  { return ObjCObject(raw, false); } // +1 (alloc/new/copy/mutableCopy/init)
        static ObjCObject retain(id raw) { return ObjCObject(raw, true);  } // borrowed so need to take ownership

        explicit ObjCObject(id raw = nullptr, bool shouldRetain = false) : instance(raw) {
            if (shouldRetain && instance) sugar_retain_raw(SUGAR_TO_PTR(instance));
        }
        ~ObjCObject() { reset(); }

        ObjCObject(const ObjCObject &) = delete;
        ObjCObject &operator=(const ObjCObject &) = delete;

        ObjCObject(ObjCObject &&other) noexcept : instance(other.instance) { other.instance = nullptr; }
        ObjCObject &operator=(ObjCObject &&other) noexcept {
            if (this != &other) {
                reset();
                instance = other.instance;
                other.instance = nullptr;
            }
            return *this;
        }

        id get() const { return instance; }
        explicit operator bool() const { return instance != nullptr; }

        // give up ownership and release the +1 (if any). 
        // the ObjCObject becomes empty.
        void reset(id raw = nullptr) {
            id old = instance;
            instance = raw;
            if (old) sugar_release_raw(SUGAR_TO_PTR(old));
        }

        // give up ownership WITHOUT releasing; you now own the +1
        id detach() {
            id r = instance;
            instance = nullptr;
            return r;
        }

        // this is useful for returning +1 results from a function to ARC, or for handing ownership back to ARC after using ObjCObject.
        id into_arc() {
            __unsafe_unretained id r = instance;
            instance = nullptr;
            return detail::arc_adopt(r);
        }

        template <typename Ret, typename... Args>
        Ret call(const char *sel, Args... args) const { return invoke<Ret>(instance, sel, args...); }

        template <typename Ret, typename... Args>
        Ret call(SEL sel, Args... args) const { return invoke<Ret>(instance, sel, args...); }

        template <typename Sig, typename... Given>
        auto call_as(const char *sel, Given &&...given) const
            -> decltype(invoke_as<Sig>(instance, sel, std::forward<Given>(given)...)) {
            return invoke_as<Sig>(instance, sel, std::forward<Given>(given)...);
        }
    };

    // hooking internals
    namespace detail {
        inline void *strip_imp(IMP imp) {
        #if __has_feature(ptrauth_calls)
            return ptrauth_strip((void *)imp, ptrauth_key_asia);
        #else
            return (void *)imp;
        #endif
        }

        /* arm64e: signing nullptr yields a garbage non-null pointer, so never sign null. */
        inline IMP sign_imp(IMP imp) {
        #if __has_feature(ptrauth_calls)
            if (!imp) return nullptr;
            return (IMP)ptrauth_sign_unauthenticated(ptrauth_strip((void *)imp, ptrauth_key_asia), ptrauth_key_asia, 0);
        #else
            return imp;
        #endif
        }

        struct Target {
            Class cls = nullptr; /* class (or metaclass for class methods) that was hooked */
            SEL sel = nullptr;
            IMP orig = nullptr;
            bool inherited = false; /* built-in hooker only: method lived on a superclass */
        };

        enum class Result { Ok, ClassMissing, SelectorMissing, AlreadyHooked };

        /* Look up + validate + hook. Used by Hook<>. */
        inline Result install(const char *clsName, const char *selName, bool isMeta, IMP repl, Target &t) {
            Class cls = isMeta ? objc_getMetaClass(clsName) : objc_getClass(clsName);
            if (!cls) return Result::ClassMissing;

            SEL sel = sel_registerName(selName);

            /* For class methods `cls` is the metaclass, so "instance method of the metaclass" == class method. */
            Method method = class_getInstanceMethod(cls, sel);
            if (!method) return Result::SelectorMissing;

            /* If the current IMP already is our replacement, hooking again would make `orig` point to
            * ourselves (infinite recursion). We can only detect OUR replacement here; another tweak's hook
            * on the same method is fine, that is just chaining. */
            if (strip_imp(method_getImplementation(method)) == strip_imp(repl)) return Result::AlreadyHooked;

            t.cls = cls;
            t.sel = sel;

        #if SUGAR_BACKEND_SUBSTRATE
            IMP old = nullptr;
            MSHookMessageEx(cls, sel, repl, &old);
            t.orig = old;
            t.inherited = false;
        #else
            IMP old = class_replaceMethod(cls, sel, repl, method_getTypeEncoding(method));
            if (old) {
                t.orig = sign_imp(old);
                t.inherited = false;
            } else {
                /* method was inherited: class_replaceMethod added a new one. `orig` is resolved from the
                * superclass at call time (see Hook::resolve) so hooks added on the superclass later are not skipped. */
                t.inherited = true;
                Class sup = class_getSuperclass(cls);
                t.orig = sup ? class_getMethodImplementation(sup, sel) : nullptr;
            }
        #endif
            return Result::Ok;
        }

        /* ---- deferred installation (class not loaded yet) ----------------------- */
        struct Pending {
            bool (*fn)(void *);
            void *ctx;
        };

        inline std::mutex &pending_mutex() { static std::mutex m; return m; }
        inline std::vector<Pending> &pending_list() { static std::vector<Pending> v; return v; }

        inline void on_image_added(const struct mach_header *, intptr_t) {
            std::vector<Pending> snapshot;
            {
                std::lock_guard<std::mutex> g(pending_mutex());
                snapshot = pending_list();
            }
            if (snapshot.empty()) return;

            std::vector<void *> done;
            for (const Pending &p : snapshot) {
                if (p.fn(p.ctx)) done.push_back(p.ctx); /* runs outside the lock (class loading may re-enter) */
            }
            if (!done.empty()) {
                std::lock_guard<std::mutex> g(pending_mutex());
                auto &v = pending_list();
                v.erase(std::remove_if(v.begin(), v.end(), [&](const Pending &p) {
                    return std::find(done.begin(), done.end(), p.ctx) != done.end();
                }), v.end());
            }
        }

        inline void defer(bool (*fn)(void *), void *ctx) {
            static std::once_flag once;
            {
                std::lock_guard<std::mutex> g(pending_mutex());
                pending_list().push_back({fn, ctx});
            }
            /* also calls back immediately for already-loaded images, which is what we want */
            std::call_once(once, [] { _dyld_register_func_for_add_image(&on_image_added); });
        }

    }

    // hook<R(id, SEL, Args...)>: typed hook object (created by OBJC_HOOK)
    namespace detail {

        template <typename> inline constexpr bool dependent_false_v = false;

        /* number of ':' in a selector == number of arguments the method takes */
        constexpr std::size_t count_colons(const char *s) {
            std::size_t n = 0;
            for (; *s; ++s) {
                if (*s == ':') ++n;
            }
            return n;
        }

        /* number of extra arguments (everything after self and _cmd) of a hook implementation */
        template <typename Fn>
        struct HookArity {
            static_assert(dependent_false_v<Fn>,
                        "sugar: hook implementation must have the form RetType(id self, SEL _cmd, args...)");
        };
        template <typename R, typename... A>
        struct HookArity<R(id, SEL, A...)> {
            static constexpr std::size_t value = sizeof...(A);
        };

        } // namespace detail

        template <typename Fn>
        struct Hook {
            static_assert(detail::dependent_false_v<Fn>,
                        "sugar: Hook<> needs a signature of the form RetType(id, SEL, args...)");
        };

        template <typename R, typename... A>
        struct Hook<R(id, SEL, A...)> {
            Hook(const char *cls, const char *sel, bool isMeta, IMP replacement, unsigned flags = kHookDefault)
                : cls_(cls), sel_(sel), isMeta_(isMeta), repl_(replacement), flags_(flags) {
                if (!attempt() && result_ == detail::Result::ClassMissing && (flags_ & kHookDeferred)) {
                    detail::defer(&Hook::retry, this);
                }
            }

            Hook(const Hook &) = delete;
            Hook &operator=(const Hook &) = delete;

            /* Call the original implementation. A... are the real parameter types. */
            R orig(id self, SEL _cmd, A... args) const {
                IMP fn = resolve();
                if (!fn) {
                    if constexpr (std::is_void_v<R>) return;
                    else return R();
                }
                return reinterpret_cast<R (*)(id, SEL, A...)>(fn)(self, _cmd, args...);
            }

            bool installed() const { return installed_.load(); }

            /* Built-in hooker only: put the original back. Overwrites any hook another tweak stacked on top! */
            bool restore() {
        #if SUGAR_BACKEND_SUBSTRATE
                return false;
        #else
                if (!installed_.load()) return false;
                Method m = class_getInstanceMethod(t_.cls, t_.sel);
                IMP orig = resolve();
                if (!m || !orig) return false;
                method_setImplementation(m, orig);
                installed_.store(false);
                return true;
        #endif
            }

        private:
            static bool retry(void *ctx) { return static_cast<Hook *>(ctx)->attempt(); }

            IMP resolve() const {
        #if !SUGAR_BACKEND_SUBSTRATE
                if (t_.inherited) {
                    Class sup = class_getSuperclass(t_.cls);
                    return sup ? class_getMethodImplementation(sup, t_.sel) : nullptr;
                }
        #endif
                return t_.orig;
            }

            bool attempt() {
                if (installed_.load()) return true;
                if (busy_.exchange(true)) return false;

                detail::Target t;
                detail::Result r = detail::install(cls_, sel_, isMeta_, repl_, t);
                if (r == detail::Result::Ok) {
                    t_ = t;
                    installed_.store(true);
                }
                result_ = r;
                busy_.store(false);

                if (r != detail::Result::Ok) report(r);
                return r == detail::Result::Ok;
            }

            void report(detail::Result r) const {
                if (flags_ & kHookOptional) return;
                switch (r) {
                    case detail::Result::ClassMissing:
                        if (flags_ & kHookDeferred) return;   /* still waiting for the image */
                        SUGAR_LOG("hook failed: class %{public}s not found", cls_);
                        break;
                    case detail::Result::SelectorMissing:
                        SUGAR_LOG("hook failed: %{public}s has no method %{public}s", cls_, sel_);
                        break;
                    case detail::Result::AlreadyHooked:
                        SUGAR_LOG("hook skipped: %{public}s %{public}s already uses this replacement (would self-recurse)", cls_, sel_);
                        break;
                    default: break;
                }
            }

            const char *cls_;
            const char *sel_;
            bool isMeta_;
            IMP repl_;
            unsigned flags_;
            detail::Target t_{};
            detail::Result result_ = detail::Result::ClassMissing;
            std::atomic<bool> installed_{false};
            std::atomic<bool> busy_{false};
        };

        /* ===========================================================================
        * Add a method if the class doesn't implement it itself
        * ========================================================================= */
        namespace detail {
        struct MethodAdder {
            bool added = false;
            MethodAdder(const char *clsName, const char *selName, bool isMeta, const char *types, IMP imp) {
                Class c = isMeta ? objc_getMetaClass(clsName) : objc_getClass(clsName);
                if (!c) {
                    SUGAR_LOG("add_method failed: class %{public}s not found", clsName);
                    return;
                }
                added = class_addMethod(c, sel_registerName(selName), imp, types);
                if (!added) {
                    SUGAR_LOG("add_method skipped: %{public}s already implements %{public}s", clsName, selName);
                }
            }
        };
    }

    // ivar stuff

    template <typename T>
    inline T *ivar_ptr(id obj, const char *name) {
        static_assert(std::is_trivially_copyable<T>::value, "ivar<T>: T must be trivially copyable (with ARC use void* or __unsafe_unretained id for object ivars)");
        if (!obj) return nullptr;
        Ivar iv = class_getInstanceVariable(object_getClass(obj), name);
        if (!iv) {
            SUGAR_LOG("ivar '%{public}s' not found on %{public}s", name, class_getName(object_getClass(obj)));
            return nullptr;
        }
        return reinterpret_cast<T *>(reinterpret_cast<char *>(SUGAR_TO_PTR(obj)) + ivar_getOffset(iv));
    }

    template <typename T>
    inline T &ivar(id obj, const char *name) {
        T *p = ivar_ptr<T>(obj, name);
        if (!p) __builtin_trap();
        return *p;
    }

    // associated objects
    inline void assoc_set(id obj, const void *key, id value, objc_AssociationPolicy policy = OBJC_ASSOCIATION_RETAIN_NONATOMIC) {
        objc_setAssociatedObject(obj, key, value, policy);
    }

    inline id assoc_get(id obj, const void *key) {
        return objc_getAssociatedObject(obj, key);
    }

    // scalars / raw pointers up to 8 bytes, stored as bits with ASSIGN policy (no retain)
    template <typename T>
    inline void assoc_set_raw(id obj, const void *key, T value) {
        static_assert(std::is_trivially_copyable<T>::value && sizeof(T) <= sizeof(uintptr_t), "assoc_set_raw<T>: T must be trivially copyable and at most pointer-sized");
        uintptr_t bits = 0;
        std::memcpy(&bits, &value, sizeof(T));
        sugar_set_assoc_raw(SUGAR_TO_PTR(obj), key, reinterpret_cast<void *>(bits), OBJC_ASSOCIATION_ASSIGN);
    }

    /* rReturns T{} (zero bits) if nothing was set. */
    template <typename T>
    inline T assoc_get_raw(id obj, const void *key) {
        static_assert(std::is_trivially_copyable<T>::value && sizeof(T) <= sizeof(uintptr_t), "assoc_get_raw<T>: T must be trivially copyable and at most pointer-sized");
        uintptr_t bits = reinterpret_cast<uintptr_t>(sugar_get_assoc_raw(SUGAR_TO_PTR(obj), key));
        T value{};
        std::memcpy(&value, &bits, sizeof(T));
        return value;
    }

} // namespace sugar

/* 
    cached selector: the first time SUGAR_SEL("foo:") is called, it registers the selector and caches it in a static variable.
    subsequent calls return the cached SEL. The selector must be a string literal (the "" concatenation trick forces this at compile time).
*/
#define SUGAR_SEL(s) \
    ([]() -> SEL { static const SEL sugar_sel_ = sel_registerName(s ""); return sugar_sel_; }())

// class lookup, cached only once found (a class that isn't loaded yet must not be cached as nil).
#define SUGAR_CLS(name) \
    ([]() -> Class { \
        static std::atomic<Class> sugar_cls_{nullptr}; \
        Class c_ = sugar_cls_.load(std::memory_order_relaxed); \
        if (!c_) { \
            c_ = objc_getClass(name ""); \
            if (c_) sugar_cls_.store(c_, std::memory_order_relaxed); \
        } \
        return c_; \
    }())

#define SUGAR_ASSOC_KEY(name) \
    static char name##_storage_; \
    static const void *const name = &name##_storage_

 /* 
    hook macros::::
    SUGAR_HOOK(Name, "Class", "sel:", isClassMethod, RetType, extraArgs...) { body } SUGAR_HOOK_END;
    the body is a real named function (Name_impl), so it shows up with a name in backtraces,
    and Name.orig(...) is typed from the same signature.
    (SUGAR_HOOK_END is optional and expands to nothing, cosmetic.)
    compile-time check: number of ':' in the selector == number of extra arguments of the hook.
    also forces the selector to be a string literal (the "" concatenation), so it can be evaluated at compile time
 */
#define SUGAR_CHECK_SEL_ARITY(name, sel) \
    static_assert(::sugar::detail::count_colons(sel "") == ::sugar::detail::HookArity<decltype(name##_impl)>::value, \
                  "sugar: number of ':' in the selector does not match the number of extra arguments")

#define SUGAR_HOOK_EX(flags, name, cls, sel, isMeta, RetType, ...) \
    static RetType name##_impl([[maybe_unused]] id self, [[maybe_unused]] SEL _cmd, ##__VA_ARGS__); \
    static ::sugar::Hook<decltype(name##_impl)> name(cls, sel, isMeta, reinterpret_cast<IMP>(name##_impl), (flags)); SUGAR_CHECK_SEL_ARITY(name, sel);  \
    static RetType name##_impl([[maybe_unused]] id self, [[maybe_unused]] SEL _cmd, ##__VA_ARGS__)

#define SUGAR_HOOK(name, cls, sel, isMeta, RetType, ...) \
    SUGAR_HOOK_EX(::sugar::kHookDefault, name, cls, sel, isMeta, RetType, ##__VA_ARGS__)

#define SUGAR_HOOK_OPTIONAL(name, cls, sel, isMeta, RetType, ...) \
    SUGAR_HOOK_EX(::sugar::kHookOptional, name, cls, sel, isMeta, RetType, ##__VA_ARGS__)

#define SUGAR_HOOK_DEFERRED(name, cls, sel, isMeta, RetType, ...) \
    SUGAR_HOOK_EX(::sugar::kHookDeferred, name, cls, sel, isMeta, RetType, ##__VA_ARGS__)

#define SUGAR_HOOK_END

// orig call wrapper: Name.orig(self, _cmd, args...) calls the original implementation of the hooked method
#define SUGAR_ORIG(name, ...) name.orig(self, _cmd, ##__VA_ARGS__)

// add a method to a class only if that class doesn't already implement it
#define SUGAR_ADD_METHOD(name, cls, sel, isMeta, types, RetType, ...) \
    static RetType name##_impl([[maybe_unused]] id self, [[maybe_unused]] SEL _cmd, ##__VA_ARGS__); \
    static ::sugar::detail::MethodAdder name##_adder(cls, sel, isMeta, types, reinterpret_cast<IMP>(name##_impl)); SUGAR_CHECK_SEL_ARITY(name, sel); \
    static RetType name##_impl([[maybe_unused]] id self, [[maybe_unused]] SEL _cmd, ##__VA_ARGS__)


/*
    lazy @interface for a class that may not exist at runtime.
    btw they are marked objc_runtime_visible so that [Cls alloc] / Cls.class resolve the class by name at runtime 
    instead of emitting a link-time reference to OBJC_CLASS_$_Cls. 
    the price: no subclassing, no categories, and a class that doesn't exist at runtime is just nil. 
    don't use it for classes whose interface is already visible (redefinition) -> use DECL for those.
*/
#define SUGAR_H_START(cls) __attribute__((objc_runtime_visible)) @interface cls : NSObject
#define SUGAR_H_START_SUPER(cls, sup) __attribute__((objc_runtime_visible)) @interface cls : sup
#define SUGAR_H_END @end

// category on a class whose real @interface is already visible (UIKit/Foundation)
#define SUGAR_DECL(cls) @interface cls (cls##_SugarTweak)
#define SUGAR_DECL_END @end

// short names for the macros and types above. define SUGAR_SHORT_NAMES before including to enable them
#ifdef SUGAR_SHORT_NAMES
    #define OBJC_HOOK SUGAR_HOOK
    #define OBJC_HOOK_OPTIONAL SUGAR_HOOK_OPTIONAL
    #define OBJC_HOOK_DEFERRED SUGAR_HOOK_DEFERRED
    #define OBJC_HOOK_EX SUGAR_HOOK_EX
    #define OBJC_HOOK_END SUGAR_HOOK_END
    #define OBJC_ADD_METHOD SUGAR_ADD_METHOD
    #define ORIG SUGAR_ORIG
    #define SEL_C SUGAR_SEL
    #define CLS SUGAR_CLS
    #define H_START SUGAR_H_START
    #define H_START_SUPER SUGAR_H_START_SUPER
    #define H_END SUGAR_H_END
    #define DECL SUGAR_DECL
    #define DECL_END SUGAR_DECL_END

    using sugar::invoke;
    using sugar::invoke_as;
    using sugar::invoke_owned;
    using sugar::new_instance;
    using sugar::ObjCObject;
    using sugar::ivar;
    using sugar::ivar_ptr;
#endif