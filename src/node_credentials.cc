#include "env-inl.h"
#include "node_external_reference.h"
#include "node_internals.h"
#include "util-inl.h"

#ifdef NODE_IMPLEMENTS_POSIX_CREDENTIALS
#include <grp.h>  // getgrnam()
#include <pwd.h>  // getpwnam()
#endif            // NODE_IMPLEMENTS_POSIX_CREDENTIALS

#if !defined(_MSC_VER)
#include <unistd.h>  // setuid, getuid
#endif
#ifdef __linux__
#include <linux/capability.h>
#include <sys/syscall.h>
#endif  // __linux__

namespace node {

using v8::Array;
using v8::Context;
using v8::FunctionCallbackInfo;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Object;
using v8::String;
using v8::TryCatch;
using v8::Uint32;
using v8::Value;

namespace per_process {
bool linux_at_secure = false;
}  // namespace per_process

namespace credentials {

#if defined(__linux__)
// Returns true if the current process only has the passed-in capability.
bool HasOnly(int capability) {
  DCHECK(cap_valid(capability));

  struct __user_cap_data_struct cap_data[2];
  struct __user_cap_header_struct cap_header_data = {
    _LINUX_CAPABILITY_VERSION_3,
    getpid()};


  if (syscall(SYS_capget, &cap_header_data, &cap_data) != 0) {
    return false;
  }
  if (capability < 32) {
    return cap_data[0].permitted ==
        static_cast<unsigned int>(CAP_TO_MASK(capability));
  }
  return cap_data[1].permitted ==
      static_cast<unsigned int>(CAP_TO_MASK(capability));
}
#endif

// Look up the environment variable and allow the lookup if the current
// process only has the capability CAP_NET_BIND_SERVICE set. If the current
// process does not have any capabilities set and the process is running as
// setuid root then lookup will not be allowed.
bool SafeGetenv(const char* key,
                std::string* text,
                std::shared_ptr<KVStore> env_vars,
                v8::Isolate* isolate) {
#if !defined(__CloudABI__) && !defined(_WIN32)
#if defined(__linux__)
  if ((!HasOnly(CAP_NET_BIND_SERVICE) && per_process::linux_at_secure) ||
      getuid() != geteuid() || getgid() != getegid())
#else
  if (per_process::linux_at_secure || getuid() != geteuid() ||
      getgid() != getegid())
#endif
    goto fail;
#endif

  if (env_vars != nullptr) {
    DCHECK_NOT_NULL(isolate);
    HandleScope handle_scope(isolate);
    TryCatch ignore_errors(isolate);
    MaybeLocal<String> maybe_value = env_vars->Get(
        isolate, String::NewFromUtf8(isolate, key).ToLocalChecked());
    Local<String> value;
    if (!maybe_value.ToLocal(&value)) goto fail;
    String::Utf8Value utf8_value(isolate, value);
    if (*utf8_value == nullptr) goto fail;
    *text = std::string(*utf8_value, utf8_value.length());
    return true;
  }

  {
    Mutex::ScopedLock lock(per_process::env_var_mutex);

    size_t init_sz = 256;
    MaybeStackBuffer<char, 256> val;
    int ret = uv_os_getenv(key, *val, &init_sz);

    if (ret == UV_ENOBUFS) {
      // Buffer is not large enough, reallocate to the updated init_sz
      // and fetch env value again.
      val.AllocateSufficientStorage(init_sz);
      ret = uv_os_getenv(key, *val, &init_sz);
    }

    if (ret >= 0) {  // Env key value fetch success.
      *text = *val;
      return true;
    }
  }

fail:
  text->clear();
  return false;
}

static void SafeGetenv(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsString());
  Environment* env = Environment::GetCurrent(args);
  Isolate* isolate = env->isolate();
  Utf8Value strenvtag(isolate, args[0]);
  std::string text;
  if (!SafeGetenv(*strenvtag, &text, env->env_vars(), isolate)) return;
  Local<Value> result =
      ToV8Value(isolate->GetCurrentContext(), text).ToLocalChecked();
  args.GetReturnValue().Set(result);
}

#ifdef NODE_IMPLEMENTS_POSIX_CREDENTIALS

static const uid_t uid_not_found = static_cast<uid_t>(-1);
static const gid_t gid_not_found = static_cast<gid_t>(-1);

static uid_t uid_by_name(const char* name) {
  struct passwd pwd;
  struct passwd* pp;
  char buf[8192];

  errno = 0;
  pp = nullptr;

  if (getpwnam_r(name, &pwd, buf, sizeof(buf), &pp) == 0 && pp != nullptr)
    return pp->pw_uid;

  return uid_not_found;
}

static char* name_by_uid(uid_t uid) {
  struct passwd pwd;
  struct passwd* pp;
  char buf[8192];
  int rc;

  errno = 0;
  pp = nullptr;

  if ((rc = getpwuid_r(uid, &pwd, buf, sizeof(buf), &pp)) == 0 &&
      pp != nullptr) {
    return strdup(pp->pw_name);
  }

  if (rc == 0) errno = ENOENT;

  return nullptr;
}

static gid_t gid_by_name(const char* name) {
  struct group pwd;
  struct group* pp;
  char buf[8192];

  errno = 0;
  pp = nullptr;

  if (getgrnam_r(name, &pwd, buf, sizeof(buf), &pp) == 0 && pp != nullptr)
    return pp->gr_gid;

  return gid_not_found;
}

#if 0  // For future use.
static const char* name_by_gid(gid_t gid) {
  struct group pwd;
  struct group* pp;
  char buf[8192];
  int rc;

  errno = 0;
  pp = nullptr;

  if ((rc = getgrgid_r(gid, &pwd, buf, sizeof(buf), &pp)) == 0 &&
      pp != nullptr) {
    return strdup(pp->gr_name);
  }

  if (rc == 0)
    errno = ENOENT;

  return nullptr;
}
#endif

static uid_t uid_by_name(Isolate* isolate, Local<Value> value) {
  if (value->IsUint32()) {
    return static_cast<uid_t>(value.As<Uint32>()->Value());
  } else {
    Utf8Value name(isolate, value);
    return uid_by_name(*name);
  }
}

static gid_t gid_by_name(Isolate* isolate, Local<Value> value) {
  if (value->IsUint32()) {
    return static_cast<gid_t>(value.As<Uint32>()->Value());
  } else {
    Utf8Value name(isolate, value);
    return gid_by_name(*name);
  }
}

static void GetUid(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(env->has_run_bootstrapping_code());
  // uid_t is an uint32_t on all supported platforms.
  args.GetReturnValue().Set(static_cast<uint32_t>(getuid()));
}

static void GetGid(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(env->has_run_bootstrapping_code());
  // gid_t is an uint32_t on all supported platforms.
  args.GetReturnValue().Set(static_cast<uint32_t>(getgid()));
}

static void GetEUid(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(env->has_run_bootstrapping_code());
  // uid_t is an uint32_t on all supported platforms.
  args.GetReturnValue().Set(static_cast<uint32_t>(geteuid()));
}

static void GetEGid(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(env->has_run_bootstrapping_code());
  // gid_t is an uint32_t on all supported platforms.
  args.GetReturnValue().Set(static_cast<uint32_t>(getegid()));
}

static void SetGid(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(env->owns_process_state());

  CHECK_EQ(args.Length(), 1);
  CHECK(args[0]->IsUint32() || args[0]->IsString());

  gid_t gid = gid_by_name(env->isolate(), args[0]);

  if (gid == gid_not_found) {
    // Tells JS to throw ERR_INVALID_CREDENTIAL
    args.GetReturnValue().Set(1);
  } else if (setgid(gid)) {
    env->ThrowErrnoException(errno, "setgid");
  } else {
    args.GetReturnValue().Set(0);
  }
}

static void SetEGid(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(env->owns_process_state());

  CHECK_EQ(args.Length(), 1);
  CHECK(args[0]->IsUint32() || args[0]->IsString());

  gid_t gid = gid_by_name(env->isolate(), args[0]);

  if (gid == gid_not_found) {
    // Tells JS to throw ERR_INVALID_CREDENTIAL
    args.GetReturnValue().Set(1);
  } else if (setegid(gid)) {
    env->ThrowErrnoException(errno, "setegid");
  } else {
    args.GetReturnValue().Set(0);
  }
}

static void SetUid(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(env->owns_process_state());

  CHECK_EQ(args.Length(), 1);
  CHECK(args[0]->IsUint32() || args[0]->IsString());

  uid_t uid = uid_by_name(env->isolate(), args[0]);

  if (uid == uid_not_found) {
    // Tells JS to throw ERR_INVALID_CREDENTIAL
    args.GetReturnValue().Set(1);
  } else if (setuid(uid)) {
    env->ThrowErrnoException(errno, "setuid");
  } else {
    args.GetReturnValue().Set(0);
  }
}

static void SetEUid(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(env->owns_process_state());

  CHECK_EQ(args.Length(), 1);
  CHECK(args[0]->IsUint32() || args[0]->IsString());

  uid_t uid = uid_by_name(env->isolate(), args[0]);

  if (uid == uid_not_found) {
    // Tells JS to throw ERR_INVALID_CREDENTIAL
    args.GetReturnValue().Set(1);
  } else if (seteuid(uid)) {
    env->ThrowErrnoException(errno, "seteuid");
  } else {
    args.GetReturnValue().Set(0);
  }
}

static void GetGroups(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(env->has_run_bootstrapping_code());

  int ngroups = getgroups(0, nullptr);
  if (ngroups == -1) return env->ThrowErrnoException(errno, "getgroups");

  std::vector<gid_t> groups(ngroups);

  ngroups = getgroups(ngroups, groups.data());
  if (ngroups == -1)
    return env->ThrowErrnoException(errno, "getgroups");

  groups.resize(ngroups);
  gid_t egid = getegid();
  if (std::find(groups.begin(), groups.end(), egid) == groups.end())
    groups.push_back(egid);
  MaybeLocal<Value> array = ToV8Value(env->context(), groups);
  if (!array.IsEmpty())
    args.GetReturnValue().Set(array.ToLocalChecked());
}

static void SetGroups(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK_EQ(args.Length(), 1);
  CHECK(args[0]->IsArray());

  Local<Array> groups_list = args[0].As<Array>();
  size_t size = groups_list->Length();
  MaybeStackBuffer<gid_t, 64> groups(size);

  for (size_t i = 0; i < size; i++) {
    gid_t gid = gid_by_name(
        env->isolate(), groups_list->Get(env->context(), i).ToLocalChecked());

    if (gid == gid_not_found) {
      // Tells JS to throw ERR_INVALID_CREDENTIAL
      args.GetReturnValue().Set(static_cast<uint32_t>(i + 1));
      return;
    }

    groups[i] = gid;
  }

  int rc = setgroups(size, *groups);

  if (rc == -1) return env->ThrowErrnoException(errno, "setgroups");

  args.GetReturnValue().Set(0);
}

static void InitGroups(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK_EQ(args.Length(), 2);
  CHECK(args[0]->IsUint32() || args[0]->IsString());
  CHECK(args[1]->IsUint32() || args[1]->IsString());

  Utf8Value arg0(env->isolate(), args[0]);
  gid_t extra_group;
  bool must_free;
  char* user;

  if (args[0]->IsUint32()) {
    user = name_by_uid(args[0].As<Uint32>()->Value());
    must_free = true;
  } else {
    user = *arg0;
    must_free = false;
  }

  if (user == nullptr) {
    // Tells JS to throw ERR_INVALID_CREDENTIAL
    return args.GetReturnValue().Set(1);
  }

  extra_group = gid_by_name(env->isolate(), args[1]);

  if (extra_group == gid_not_found) {
    if (must_free) free(user);
    // Tells JS to throw ERR_INVALID_CREDENTIAL
    return args.GetReturnValue().Set(2);
  }

  int rc = initgroups(user, extra_group);

  if (must_free) free(user);

  if (rc) return env->ThrowErrnoException(errno, "initgroups");

  args.GetReturnValue().Set(0);
}

#endif  // NODE_IMPLEMENTS_POSIX_CREDENTIALS

void RegisterExternalReferences(ExternalReferenceRegistry* registry) {
  registry->Register(SafeGetenv);

#ifdef NODE_IMPLEMENTS_POSIX_CREDENTIALS
  registry->Register(GetUid);
  registry->Register(GetEUid);
  registry->Register(GetGid);
  registry->Register(GetEGid);
  registry->Register(GetGroups);

  registry->Register(InitGroups);
  registry->Register(SetEGid);
  registry->Register(SetEUid);
  registry->Register(SetGid);
  registry->Register(SetUid);
  registry->Register(SetGroups);
#endif  // NODE_IMPLEMENTS_POSIX_CREDENTIALS
}

static void Initialize(Local<Object> target,
                       Local<Value> unused,
                       Local<Context> context,
                       void* priv) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* isolate = env->isolate();

  SetMethod(context, target, "safeGetenv", SafeGetenv);

#ifdef NODE_IMPLEMENTS_POSIX_CREDENTIALS
  READONLY_TRUE_PROPERTY(target, "implementsPosixCredentials");
  SetMethodNoSideEffect(context, target, "getuid", GetUid);
  SetMethodNoSideEffect(context, target, "geteuid", GetEUid);
  SetMethodNoSideEffect(context, target, "getgid", GetGid);
  SetMethodNoSideEffect(context, target, "getegid", GetEGid);
  SetMethodNoSideEffect(context, target, "getgroups", GetGroups);

  if (env->owns_process_state()) {
    SetMethod(context, target, "initgroups", InitGroups);
    SetMethod(context, target, "setegid", SetEGid);
    SetMethod(context, target, "seteuid", SetEUid);
    SetMethod(context, target, "setgid", SetGid);
    SetMethod(context, target, "setuid", SetUid);
    SetMethod(context, target, "setgroups", SetGroups);
  }
#endif  // NODE_IMPLEMENTS_POSIX_CREDENTIALS
}

}  // namespace credentials
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(credentials, node::credentials::Initialize)
NODE_MODULE_EXTERNAL_REFERENCE(credentials,
                               node::credentials::RegisterExternalReferences)
