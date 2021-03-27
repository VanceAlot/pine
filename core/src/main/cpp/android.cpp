//
// Created by canyie on 2020/3/15.
//

#include <unistd.h>
#include <string>
#include <dlfcn.h>
#include <mutex>
#include "android.h"
#include "utils/well_known_classes.h"
#include "art/art_method.h"
#include "art/jit.h"
#include "trampoline/trampoline_installer.h"
#include "utils/memory.h"

using namespace pine;

int Android::version = -1;
JavaVM* Android::jvm = nullptr;

void (*Android::suspend_vm)() = nullptr;
void (*Android::resume_vm)() = nullptr;

void (*Android::suspend_all)(void*, const char*, bool);
void (*Android::resume_all)(void*);

void* Android::class_linker_ = nullptr;
void (*Android::make_visibly_initialized_)(void*, void*, bool) = nullptr;

void Android::Init(JNIEnv* env, int sdk_version, bool disable_hiddenapi_policy, bool disable_hiddenapi_policy_for_platform) {
    Android::version = sdk_version;
    if (UNLIKELY(env->GetJavaVM(&jvm) != JNI_OK)) {
        LOGF("Cannot get java vm");
        env->FatalError("Cannot get java vm");
        abort();
    }

    {
        ElfImg art_lib_handle("libart.so");
        if (Android::version >= Android::kR) {
            suspend_all = reinterpret_cast<void (*)(void*, const char*, bool)>(art_lib_handle.GetSymbolAddress(
                    "_ZN3art16ScopedSuspendAllC1EPKcb"));
            resume_all = reinterpret_cast<void (*)(void*)>(art_lib_handle.GetSymbolAddress(
                    "_ZN3art16ScopedSuspendAllD1Ev"));
            if (UNLIKELY(!suspend_all || !resume_all)) {
                LOGE("SuspendAll API is unavailable.");
                suspend_all = nullptr;
                resume_all = nullptr;
            }
        } else {
            suspend_vm = reinterpret_cast<void (*)()>(art_lib_handle.GetSymbolAddress(
                    "_ZN3art3Dbg9SuspendVMEv")); // art::Dbg::SuspendVM()
            resume_vm = reinterpret_cast<void (*)()>(art_lib_handle.GetSymbolAddress(
                    "_ZN3art3Dbg8ResumeVMEv")); // art::Dbg::ResumeVM()
            if (UNLIKELY(!suspend_vm || !resume_vm)) {
                LOGE("Suspend VM API is unavailable.");
                suspend_vm = nullptr;
                resume_vm = nullptr;
            }
        }

        if (Android::version >= Android::kP)
            DisableHiddenApiPolicy(&art_lib_handle, disable_hiddenapi_policy, disable_hiddenapi_policy_for_platform);

        art::Thread::Init(&art_lib_handle);
        art::ArtMethod::Init(&art_lib_handle);
        if (sdk_version >= kN) {
            ElfImg jit_lib_handle("libart-compiler.so", false);
            art::Jit::Init(&art_lib_handle, &jit_lib_handle);
        }

        if (UNLIKELY(sdk_version >= kR)) {
            InitClassLinker(jvm, &art_lib_handle);
        }
    }

    WellKnownClasses::Init(env);
}

static int FakeHandleHiddenApi() {
    return 0;
}

#pragma clang diagnostic push
#pragma ide diagnostic ignored "cppcoreguidelines-macro-usage"

void Android::DisableHiddenApiPolicy(const ElfImg* handle, bool application, bool platform) {
    TrampolineInstaller* trampoline_installer = TrampolineInstaller::GetDefault();
    void* replace = reinterpret_cast<void*>(FakeHandleHiddenApi);

#define HOOK_SYMBOL(symbol) do { \
void *target = handle->GetSymbolAddress(symbol); \
if (LIKELY(target))  \
    trampoline_installer->NativeHookNoBackup(target, replace); \
else  \
    LOGE("DisableHiddenApiPolicy: symbol %s not found", symbol); \
} while(false)

    if (Android::version >= Android::kQ) {
        if (application) {
            // Android Q, for Domain::kApplication
            HOOK_SYMBOL("_ZN3art9hiddenapi6detail28ShouldDenyAccessToMemberImplINS_8ArtFieldEEEbPT_NS0_7ApiListENS0_12AccessMethodE");
            HOOK_SYMBOL("_ZN3art9hiddenapi6detail28ShouldDenyAccessToMemberImplINS_9ArtMethodEEEbPT_NS0_7ApiListENS0_12AccessMethodE");
        }

        if (platform) {
            // For Domain::kPlatform
            HOOK_SYMBOL("_ZN3art9hiddenapi6detail30HandleCorePlatformApiViolationINS_8ArtFieldEEEbPT_RKNS0_13AccessContextENS0_12AccessMethodENS0_17EnforcementPolicyE");
            HOOK_SYMBOL("_ZN3art9hiddenapi6detail30HandleCorePlatformApiViolationINS_9ArtMethodEEEbPT_RKNS0_13AccessContextENS0_12AccessMethodENS0_17EnforcementPolicyE");
        }
    } else {
        // Android P, all accesses from platform domain will be allowed
        if (application) {
            HOOK_SYMBOL("_ZN3art9hiddenapi6detail19GetMemberActionImplINS_8ArtFieldEEENS0_6ActionEPT_NS_20HiddenApiAccessFlags7ApiListES4_NS0_12AccessMethodE");
            HOOK_SYMBOL("_ZN3art9hiddenapi6detail19GetMemberActionImplINS_9ArtMethodEEENS0_6ActionEPT_NS_20HiddenApiAccessFlags7ApiListES4_NS0_12AccessMethodE");
        }
    }

#undef HOOK_SYMBOL
}

#pragma clang diagnostic pop

static bool FakeProcessProfilingInfo() {
    LOGI("Skipped ProcessProfilingInfo.");
    return true;
}

bool Android::DisableProfileSaver() {
    // If the user needs this feature very much,
    // we may find these symbols during initialization in the future to reduce time consumption.
    void* process_profiling_info;
    {
        ElfImg handle("libart.so");
        const char* symbol = version < kO ? "_ZN3art12ProfileSaver20ProcessProfilingInfoEPt"
                                          : "_ZN3art12ProfileSaver20ProcessProfilingInfoEbPt";
        process_profiling_info = handle.GetSymbolAddress(symbol);
    }

    if (UNLIKELY(!process_profiling_info)) {
        LOGE("Failed to disable ProfileSaver: art::ProfileSaver::ProcessProfilingInfo not found");
        return false;
    }
    TrampolineInstaller::GetDefault()->NativeHookNoBackup(process_profiling_info,
            reinterpret_cast<void*>(FakeProcessProfilingInfo));
    return true;
}

void Android::InitClassLinker(JavaVM* jvm, const ElfImg* handle) {
    make_visibly_initialized_ = reinterpret_cast<void (*)(void*, void*, bool)>(handle->GetSymbolAddress(
            "_ZN3art11ClassLinker40MakeInitializedClassesVisiblyInitializedEPNS_6ThreadEb"));
    if (UNLIKELY(!make_visibly_initialized_)) {
        LOGE("ClassLinker::MakeInitializedClassesVisiblyInitialized not found");
        return;
    }

    void** instance_ptr = static_cast<void**>(handle->GetSymbolAddress("_ZN3art7Runtime9instance_E"));
    void* runtime;
    if (UNLIKELY(!instance_ptr || !(runtime = *instance_ptr))) {
        LOGE("Unable to get Runtime.");
        return;
    }

    constexpr size_t kDifference = sizeof(std::unique_ptr<void>) + sizeof(void*) + sizeof(void*);
#ifdef __LP64__
    constexpr size_t kDefaultClassLinkerOffset = 472;
#else
    constexpr size_t kDefaultClassLinkerOffset = 276;
#endif
    constexpr size_t kDefaultJavaVMOffset = kDefaultClassLinkerOffset + kDifference;

    auto jvm_ptr = reinterpret_cast<std::unique_ptr<JavaVM>*>(reinterpret_cast<uintptr_t>(runtime) + kDefaultJavaVMOffset);
    size_t class_linker_offset;
    if (LIKELY(jvm_ptr->get() == jvm)) {
        LOGD("JavaVM offset matches the default offset");
        class_linker_offset = kDefaultClassLinkerOffset;
    } else {
        LOGW("JavaVM offset mismatches the default offset, try search the memory of Runtime");
        int jvm_offset = Memory::FindOffset(runtime, jvm, 1024, 4);
        if (UNLIKELY(jvm_offset == -1)) {
            LOGE("Failed to find java vm from Runtime");
            return;
        }
        class_linker_offset = jvm_offset - kDifference;
        LOGW("New java_vm_offset: %d, class_linker_offset: %u", jvm_offset, class_linker_offset);
    }
    void* class_linker = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(runtime) + class_linker_offset);
    SetClassLinker(class_linker);
}
