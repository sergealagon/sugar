# Sugar

## I'm 100% certain that I am the only one who would use this and write iOS tweaks like this, but I am still leaving a documentation in case one *might* want to use it. Who knows? Either way idgaf.

## Tf is this curse?
Some library for Objective-C++ tweak development. A single header with typed method hooks, an objc_msgSend wrapper that gets the ABI right, and a few small helpers (ownership, ivars, associated objects, cached selectors).

*Kinda* like an alternative to Theos' logos I guess? 

Why? WHY NOT!

## Overview

```cpp
#define SUGAR_SHORT_NAMES
#include "sugar.hpp"
 
OBJC_HOOK(SetAlpha, "UIView", "setAlpha:", false, void, CGFloat a) {
    ORIG(SetAlpha, a * 0.5);
} OBJC_HOOK_END;
```
 
- **Typed hooks.** `Name.orig(...)` takes the real parameter types, so no `int` passed where `NSInteger` is expected.
- **Safe installs.** The class and selector are checked before hooking. A hook whose replacement is already installed is refused instead of recursing forever.
- **Calls that are correct by default.** `invoke` widens integer arguments for you, `invoke_as` handles floating point, and `invoke_owned` / `new_instance` hand `+1` results to ARC.
- **Works with or without Substrate.** Uses `<substrate.h>` (Substrate, ElleKit, libhooker) if present, otherwise a built-in hooker.
- **arm64 / arm64e.** Pointer authentication is handled, including not signing null IMPs.
- **Compile-time checks** for common mistakes, such as a selector/argument count mismatch.
- **Header only.** Copy `include/sugar.hpp` into your project.
## Requirements
 
- clang, **Objective-C++** (`.mm`), **C++17**
- arm64 or arm64e (no `objc_msgSend_stret` / `_fpret` handling, so no x86_64 simulator builds, maybe in the future)
- Written for ARC. `invoke` itself bypasses ARC (see [Ownership](#ownership)).

Theos example:
 
```make
MyTweak_FILES = Tweak.mm
MyTweak_CCFLAGS = -std=c++17
MyTweak_CFLAGS = -fobjc-arc
```
 
## Short names
 
Everything is prefixed `SUGAR_` / lives in `namespace sugar`. For the short aliases, define this **before** including the header (or pass `-DSUGAR_SHORT_NAMES` in your Makefile):
 
```cpp
#define SUGAR_SHORT_NAMES
```
 
| Short | Full |
|---|---|
| `OBJC_HOOK`, `OBJC_HOOK_OPTIONAL`, `OBJC_HOOK_DEFERRED`, `OBJC_HOOK_EX`, `OBJC_HOOK_END` | `SUGAR_HOOK...` |
| `OBJC_ADD_METHOD` | `SUGAR_ADD_METHOD` |
| `ORIG` | `SUGAR_ORIG` |
| `SEL_C`, `CLS` | `SUGAR_SEL`, `SUGAR_CLS` |
| `H_START`, `H_START_SUPER`, `H_END` | `SUGAR_H_START...`, `SUGAR_H_END` |
| `DECL`, `DECL_END` | `SUGAR_DECL`, `SUGAR_DECL_END` |
| `invoke`, `invoke_as`, `invoke_owned`, `new_instance`, `ObjCObject`, `ivar`, `ivar_ptr` | `sugar::...` |
 
They are opt-in because names like `CLS`, `DECL` and `ORIG` are way too generic imo.
 
## Hooks
 
```cpp
OBJC_HOOK(Name, "ClassName", "selector:", isClassMethod, ReturnType, extra args...) {
    // body, call ORIG(Name, args...) here or whatever
} OBJC_HOOK_END;
```
 
- `self` and `_cmd` are always available in the body.
- Extra arguments keep their real types, and `Name.orig(...)` uses the same ones.
- `ORIG(Name, args...)` is wrapper for `Name.orig(self, _cmd, args...)`.
- The body is a normal named function (`Name_impl`), so it shows up with a name in backtraces.
- `OBJC_HOOK_END` is optional and expands to nothing. Pure cosmetic.
```cpp
// instance method
OBJC_HOOK(ViewDidLoad, "UIViewController", "viewDidLoad", false, void) {
    ORIG(ViewDidLoad);
} OBJC_HOOK_END;
 
// class method (isClassMethod = true)
OBJC_HOOK(SharedInst, "SBFoo", "sharedInstance", true, id) {
    return ORIG(SharedInst);
} OBJC_HOOK_END;
```
 
### Variants and flags
 
| Macro | Behavior |
|---|---|
| `OBJC_HOOK` | Logs an error if the class or selector is missing. |
| `OBJC_HOOK_OPTIONAL` | Silently skips a missing class or selector (cross-iOS-version tweaks). |
| `OBJC_HOOK_DEFERRED` | If the class isn't loaded yet, retries every time a new image is loaded. |
| `OBJC_HOOK_EX(flags, ...)` | Custom flags, to combine the above: `sugar::kHookOptional \| sugar::kHookDeferred`. |
 
### Checks done before hooking
 
1. The class exists.
2. The selector exists on it (the metaclass is used for class methods).
3. The method's current IMP is not already your replacement. Installing the same replacement twice would make `orig` point at itself and recurse forever.
The third check detects *your* replacement only. Another tweak hooking the same method is just chaining and is not blocked.
 
### Hook object API
 
Each hook defines a static object named `Name`:
 
| Call | Meaning |
|---|---|
| `Name.installed()` | Whether the hook is active. |
| `Name.restore()` | Puts the original back. **Built-in hooker only**, and it overwrites hooks stacked on top. |
 
### Inherited methods
 
If the method is inherited and you use the built-in hooker, `orig` is resolved from the superclass at call time. A hook added to the superclass later is therefore not skipped.
 
### Timing
 
Hooks are static objects, so they install during static initialization. Frameworks loaded lazily may not be there yet. Use `OBJC_HOOK_DEFERRED` for those.
 
## Calling methods
 
```cpp
invoke<void>(self, "someMethod:", someArg);

// caching selector for reuse using SEL_C
id v = invoke<id>(self, SEL_C("valueForKey:"), @"foo");
```
 
`invoke` is a thin wrapper over `objc_msgSend`.
 
- **Integers are handled for you.** `bool`, `char`, `short`, `int`, `long`, `NSInteger`, enums and so on are widened to 64 bits (sign- or zero-extended by their type). That is always correct on arm64, so `invoke<void>(obj, "setCount:", -1)` works whatever width the method really takes.
- **Everything else is passed exactly as typed.** Pointers, `id`, `SEL` and structs are fine as they are.
- **Floating point is the one thing it can't guess.** `float` vs `double`, or an `int` where the method wants a `CGFloat`, ends up in the wrong register and gives garbage.
For floating point, use `invoke_as` and give it the real signature. The arguments are converted to it:
 
```cpp
invoke_as<void(NSInteger, CGFloat)>(obj, "setCount:alpha:", 5, 1.0);
```
 
Ask for the real return type (`NSInteger`, `BOOL`, `CGFloat`, ...). Variadic ObjC methods aren't supported (arm64 passes variadic arguments on the stack).
 
## Ownership
 
`invoke` **bypasses ARC**.
 
| Source | Ownership |
|---|---|
| `alloc`, `new`, `copy`, `mutableCopy` | +1, you own it |
| `init` | consumes its receiver and returns +1 |
| Everything else (plain getters, ...) | +0, don't release |
 
So `invoke<id>(invoke<id>(cls, "alloc"), "init")` gives you **one** +1 object. Don't release the `alloc` result separately.
 
### Handing a +1 result to ARC
 
```cpp
id a = invoke_owned(CLS("NSObject"), "new"); // outermost call returns +1 -> soooo ARC owns it
id b = new_instance("NSObject"); // alloc + init
id c = new_instance("MyClass", "initWithFoo:bar:", foo, bar); // with init arguments
id d = new_instance(CLS("MyClass"), "initWithFoo:", foo); // Class instead of a name
```
 
- `new_instance` returns `nil` if the class doesn't exist or `init` fails.
- Use `invoke_owned` only on the **outermost** call of a chain. `init` consumes its receiver, so an ARC-owned `alloc` result would be released twice. `new_instance` does `alloc` + `init` in the safe order.
- Under MRC the result is autoreleased instead.
### Other options for handling a +1 object
 
```cpp
// Option 1. RAII wrapper
auto obj = sugar::ObjCObject::adopt(invoke<id>(invoke<id>(CLS("NSObject"), "alloc"), "init"));
obj.call<void>("doThing");
 
// Option 2. hand it to ARC later
id arcOwned = obj.into_arc();
 
// Option 3. bridge it yourself (same operation as into_arc)
id o = CFBridgingRelease((__bridge CFTypeRef)raw);   // or (__bridge_transfer id)raw
 
// Option 4. release manually
CFRelease((__bridge CFTypeRef)raw);
```
 
For a +0 value you want to keep alive, use `ObjCObject::retain(x)`.
 
### `ObjCObject`
 
| Member | Meaning |
|---|---|
| `adopt(id)` | Take ownership of an existing +1. |
| `retain(id)` | Retain a borrowed +0 object. |
| `get()` | The raw `id`. |
| `reset(id = nil)` | Release the current object, optionally adopt another. |
| `detach()` | Give up ownership **without** releasing. You now own the +1. |
| `into_arc()` | Hand the +1 to ARC and return the ARC-managed `id`. |
| `call<Ret>(sel, args...)` | `invoke` on the held object. |
| `call_as<Sig>(sel, args...)` | `invoke_as` on the held object. |
 
It is move-only, and the held pointer is `__unsafe_unretained`, so it is safe under ARC.
 
## Selectors and classes
 
```cpp
SEL s = SEL_C("foo:"); // cached per call site
Class c = CLS("SpringBoard"); // cached once found
```
 
- Each use of `SEL_C` / `CLS` is its own lambda with its own `static`, so a call site only ever caches the one literal written there.
- A `static SEL` inside a function *template* is shared by every call with the same template arguments.
- Only string literals compile. A variable would be cached forever.
- `CLS` doesn't cache a miss, so a class that isn't loaded yet isn't cached as nil.
- `invoke<R>(obj, "foo:")` with a string is deliberately **not** cached. Use `SEL_C` in hot paths.
## Lazy headers
 
Declare just the methods you need for a private class:
 
```cpp
H_START(SpringBoard)
- (void)_simulateHomeButtonPress;
- (void)applicationOpenURL:(id)url publicURLsOnly:(BOOL)only;
H_END
```
 
- Use `[obj method]` syntax on instances. Under ARC the compiler then applies the ownership rules for you, which `invoke` can't.
- These declarations are marked `objc_runtime_visible`, so `[SpringBoard alloc]` and `SpringBoard.class` resolve the class **by name at runtime** instead of through a link-time symbol.
- The trade-offs: such classes can't be subclassed and can't get categories, and a class that doesn't exist at runtime is just `nil` (messages to it do nothing).
For a category on a class whose real `@interface` is already visible (UIKit, Foundation):
 
```cpp
DECL(UIView)
- (void)_privateThing;
DECL_END
```
 
`H_START_SUPER(Cls, Super)` declares a different superclass than `NSObject`. `Super` must be a normal class, not another `H_START` class.
 
## Adding methods
 
Adds a method only if the class doesn't already implement it itself:
 
```cpp
// OBJC_ADD_METHOD(Name, "Class", "selector:", isClassMethod, "type encoding", ReturnType, args...)
OBJC_ADD_METHOD(AddedThing, "SBFoo", "myNewMethod:", false, "v@:@", void, id arg) {
    NSLog(@"hi %@", arg);
}
```
 
`AddedThing_adder.added` tells you whether it was added.
 
## Ivars
 
```cpp
ivar<BOOL>(self, "_isActive") = YES; // logs and traps if the ivar is missing
if (auto *p = ivar_ptr<void *>(self, "_ctx")) { /* ... */ } // nullptr if missing
```
 
`T` must be trivially copyable. With ARC, use `void *` or `__unsafe_unretained id` for object ivars. The header doesn't check that `T` matches the real size of the ivar.
 
## Associated objects
 
```cpp
SUGAR_ASSOC_KEY(kMyKey); // the address is the key
 
sugar::assoc_set(self, kMyKey, someObj); // objects, retain nonatomic by default
id o = sugar::assoc_get(self, kMyKey);
 
sugar::assoc_set_raw<int>(self, kMyKey, 42); // scalars / pointers, up to 8 bytes
int n = sugar::assoc_get_raw<int>(self, kMyKey); // 0 if unset
```
 
The `_raw` variants store the bits with `ASSIGN`, so there is no retain and no ARC involvement.
 
## OS version checks
 
There is no helper for this: use the compiler's own `@available(iOS 17.4, *)`.
 
## Compile-time checks
 
| Check | What it catches |
|---|---|
| `#error` for non-clang, non-Objective-C++, pre-C++17, non-arm64 | Wrong build setup |
| `static_assert` on selector colons vs. hook arguments | `"setFoo:bar:"` hooked with one argument, and so on |
| Hook signature must be `Ret(id, SEL, args...)` | Readable error instead of template noise |
| `SEL_C` / `CLS` take literals only | A variable getting cached forever |
| `ivar<T>` / `assoc_*_raw<T>` `static_assert` on `T` | Non-trivial or oversized types |
 
Hook macros need **string literals** for the selector (and class name), because the selector is checked at compile time.
 
## Backends
 
| Condition | Backend |
|---|---|
| `<substrate.h>` found (default) | `MSHookMessageEx` from Substrate / ElleKit / libhooker |
| Not found, or `#define SUGAR_BUILTIN_HOOKER` | Built-in `class_replaceMethod` hooker |
 
You can force it with `#define SUGAR_BACKEND_SUBSTRATE 0` or `1` before the include.
 
`restore()` and call-time superclass lookup are built-in-hooker features.
 
## Configuration
 
| Macro | Effect |
|---|---|
| `SUGAR_SHORT_NAMES` | Unprefixed aliases (`OBJC_HOOK`, `ORIG`, ...) |
| `SUGAR_BUILTIN_HOOKER` | Never use `<substrate.h>` |
| `SUGAR_BACKEND_SUBSTRATE` | Force the backend (`0` or `1`) |
| `SUGAR_LOG_ENABLED` | `0` silences all logs (default `1`) |
 
Logs go through `os_log` in release builds. Look for the `[sugar]` prefix.
 
## Limitations
 
- arm64 / arm64e only.
- A deferred hook that installs while other threads are already calling the method can miss a call or two (`orig` isn't set yet).
- "Already hooked" detection only knows about your own replacement function.
- Static-init timing: use `OBJC_HOOK_DEFERRED` for lazily loaded classes.
## License
 
MIT. See `LICENSE`.