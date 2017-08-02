#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#elif _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4127 4100 4512 4800 4267 4251)
#endif

// spidermonkey
#include <jsapi.h>

// jsrdbg
#include <jsrdbg/jsrdbg.h>

// standard libraries
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace JS;

// The class of the global object.
static JSClass globalClass = {
    "global",         JSCLASS_GLOBAL_FLAGS,
    JS_PropertyStub,  JS_DeletePropertyStub,
    JS_PropertyStub,  JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub,
    JS_ConvertStub,   nullptr,
    nullptr,          nullptr,
    nullptr,
};

// Function definitions
JSBool log_impl(JSContext *cx, unsigned int argc, JS::Value *vp);
static JSFunctionSpec globalFunctions[] = {
    JS_FS("log", log_impl, 0, 0), JS_FS_END,
};

JSBool log_impl(JSContext *cx, unsigned argc, JS::Value *vp) {
  static std::mutex on_log_impl_mtx;
  std::lock_guard<std::mutex> lock(on_log_impl_mtx);

  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  for (unsigned i = 0; i < args.length(); i++) {
    JS::RootedString str(cx);

    if (args[i].isObject() && !args[i].isNull()) {
      str = args[i].toString();
    } else if (args[i].isString()) {
      str = args[i].toString();
    } else {
      return false;
    }

    if (!str) {
      return false;
    }

    char *utf8_encoded = JS_EncodeStringToUTF8(cx, str);
    if (!utf8_encoded) {
      return false;
    }

    fprintf(stdout, "%s%s", i ? " " : "", utf8_encoded);
    JS_free(cx, utf8_encoded);
  }

  fputc('\n', stdout);
  fflush(stdout);

  args.rval().setUndefined();
  return true;
}

// The error reporter CB
void reportError(JSContext *ctx, const char *message, JSErrorReport *report) {
  std::cout << "An error occured" << std::endl
            << (report->filename ? report->filename : const_cast<char *>("[no filename] :"))
            << "line : " << report->lineno << " :" << std::endl
            << message << std::endl;
}

class JSRunnerException : public std::runtime_error {
public:
  JSRunnerException(const std::string &mss) : std::runtime_error(mss) {}

  using std::runtime_error::what;
};


//
// Loads and source files for each context and stores them for future use.
//
class JSSourceFactory {
protected:
  std::map<JSContext *, std::string> sources;
  std::mutex mtx;

public:
  std::string getSource(JSContext *ctx) {
    auto source = sources.at(ctx);
    return source;
  }

  std::string getSource(JSContext *ctx, std::string &path) {
    if (!path.empty()) {
      std::string source;
      std::ifstream ifstr(path);
      ifstr.seekg(0, std::ios::end);
      source.reserve(ifstr.tellg());
      ifstr.seekg(0, std::ios::beg);
      source.assign((std::istreambuf_iterator<char>(ifstr)), std::istreambuf_iterator<char>());
      return source;
    } else {
      throw std::runtime_error("paths must not be empty");
    }
  }
};

//
// Wrapper around JSR::JSRemoteDebugger to simplify the usage of JSRDBG and clarify the ownership.
//
class JSRemoteDebugger {
protected:
  std::unique_ptr<JSR::JSRemoteDebugger> dbg;
  JSR::JSDbgEngineOptions dbgOptions;
  std::shared_ptr<JSSourceFactory> src_fac;
  std::set<JSContext *> set_contexts;
  std::mutex mtx;
  bool started;

public:
  JSRemoteDebugger(std::shared_ptr<JSSourceFactory> p_src_fac)
      : src_fac(p_src_fac), started(false) {
    static class loaderClass : public JSR::IJSScriptLoader {
    protected:
      std::mutex mtx;
      std::shared_ptr<JSSourceFactory> fac;

    public:
      loaderClass(std::shared_ptr<JSSourceFactory> p_fac) : fac(p_fac) {}
      int load(JSContext *ctx, const std::string &path, std::string &script) {
        try {
          auto res = fac->getSource(ctx);
          script = res;
          return JSR_ERROR_NO_ERROR;
        } catch (std::out_of_range &e) {
          return JSR_ERROR_FILE_NOT_FOUND;
        }
      }
    } loader(src_fac);
    // configure dgb
    JSR::JSRemoteDebuggerCfg dbgCfg;
    dbgCfg.setTcpHost(JSR_DEFAULT_TCP_BINDING_IP);
    dbgCfg.setTcpPort(JSR_DEFAULT_TCP_PORT);
    dbgCfg.setScriptLoader(&loader);
    // create dbg
    dbg = std::unique_ptr<JSR::JSRemoteDebugger>(new JSR::JSRemoteDebugger(dbgCfg));
    // set global options
    dbgOptions = JSR::JSDbgEngineOptions();
    dbgOptions.suspended();
  }

  ~JSRemoteDebugger() {
    // remove all contexts
    std::for_each(set_contexts.begin(), set_contexts.end(),
                  [this](JSContext *cx) { this->remCtx(cx); });
    // stop debugger
    dbg->stop();
  }

  void addCtx(JSContext *ctx, RootedObject &global, std::string path) {
    std::lock_guard<std::mutex> lock(mtx);
    if (dbg->install(ctx, path, dbgOptions) != JSR_ERROR_NO_ERROR) {
      throw JSRunnerException("Cannot install debugger.");
    }
    if (!started) {
      if (dbg->start() != JSR_ERROR_NO_ERROR) {
        throw JSRunnerException("Cannot start debugger");
      }
      started = true;
    }
    if (dbg->addDebuggee(ctx, global) != JSR_ERROR_NO_ERROR) {
      throw JSRunnerException("unable to add debuggee");
    }
    set_contexts.insert(ctx);
  }

  void remCtx(JSContext *ctx) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = set_contexts.find(ctx);
    if (it != set_contexts.end()) {
      dbg->uninstall(ctx);
      set_contexts.erase(it);
    }
  }
};

//
// Wrapper around Spidermonkey. Makes it easy to load a script and connect it to JSRDBG via
// the JSRemoteDebugger.
//
class JSRunner {

protected:
  JSRuntime *rt;
  JSContext *cx;

  std::string source;
  std::string path;

  JSRemoteDebugger &dbg;
  std::shared_ptr<JSSourceFactory> src_fac;

public:
  JSRunner(std::string p_path, JSRemoteDebugger &p_dbg, std::shared_ptr<JSSourceFactory> p_src_fac)
      : path(p_path), dbg(p_dbg), src_fac(p_src_fac) {
    // ensures, that no jsrunners are created simultainiously, because that will cause a crahs
    static std::mutex on_create_mtx;
    std::lock_guard<std::mutex> lock(on_create_mtx);
    // set up spidermonkey
    rt = JS_NewRuntime(8L * 1024L * 1024L, JS_USE_HELPER_THREADS);
    if (!rt) {
      throw JSRunnerException("unable to create a runtime");
    }
    cx = JS_NewContext(rt, 8192);
    if (!cx) {
      throw JSRunnerException("unable to create a context");
    }
    JS_SetErrorReporter(cx, &reportError);
    // set stack size
    const std::size_t max_stack_size = 128 * sizeof(std::size_t) * 1024;
    JS_SetNativeStackQuota(rt, max_stack_size);
    // load source
    source = src_fac->getSource(cx, path);
  }

  ~JSRunner() {
    dbg.remCtx(cx);
    if (!cx) {
      JS_DestroyContext(cx);
    }
    if (!rt) {
      JS_DestroyRuntime(rt);
    }
  }

  // runs the script
  void operator()() {
    // generate root object
    JSAutoRequest ar(cx);
    RootedObject global(cx);
    JS::CompartmentOptions compartment_options;
    compartment_options.setVersion(JSVERSION_LATEST);
    global = JS_NewGlobalObject(cx, &globalClass, nullptr, compartment_options);
    if (!global) {
      throw JSRunnerException("could not create a global object");
    }
    // prepare context
    JSAutoCompartment ac(cx, global);
    if (!JS_InitStandardClasses(cx, global)) {
      throw JSRunnerException("unable to init standard classes");
    }
    if (!JS_DefineFunctions(cx, global, globalFunctions)) {
      throw JSRunnerException("unable to define functions");
    }
    // register context in dbg
    dbg.addCtx(cx, global, path);
    // start script
    jsval *some = nullptr;
    if (!JS_EvaluateScript(cx, global, source.c_str(), source.size(), path.c_str(), 0, some)) {
      throw JSRunnerException("could not exeute script");
    }
  }
};

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cout << "usage: " << argv[0] << " [script]" << std::endl;
  } else {
    std::shared_ptr<JSSourceFactory> src_fac(new JSSourceFactory());
    JSRemoteDebugger dbg(src_fac);
    std::vector<std::thread> threads;
    for (int i = 1; i < argc; i++) {
      threads.push_back(std::thread([argv, i, &dbg, src_fac]() {
        try {
          JSRunner runner(argv[i], dbg, src_fac);
          runner();
          std::cout << "executed script " << i << std::endl;
        } catch (char *c) {
          std::cout << "an error occured: " << c << std::endl;
        }
      }));
      std::cout << "started js instance " << i << std::endl;
    }
    for (auto &thr : threads) {
      thr.join();
    }
    std::cout << "joined all threads" << std::endl;
  }
}
