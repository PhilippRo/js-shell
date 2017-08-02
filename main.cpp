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
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>


using namespace JS;

// The class of the global object.
static JSClass globalClass = {
    "global",
    JSCLASS_GLOBAL_FLAGS,
    JS_PropertyStub,
    JS_DeletePropertyStub,
    JS_PropertyStub,
    JS_StrictPropertyStub,
    JS_EnumerateStub,
    JS_ResolveStub,
    JS_ConvertStub,
    nullptr, nullptr, nullptr, nullptr,
};

// Function definitions
JSBool log_impl(JSContext *cx, unsigned int argc, JS::Value *vp);
static JSFunctionSpec globalFunctions[] = {
    JS_FS("log", log_impl, 0, 0),
    JS_FS_END,
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
void reportError (JSContext* ctx, const char* message, JSErrorReport* report) {
    std::cout   << "An error occured" << std::endl
                << (report->filename ? report->filename : const_cast<char*>("[no filename] :"))
                << "line : " << report->lineno << " :" << std::endl
                << message << std::endl;
}

class JSRunnerException : public std::runtime_error {
    public:
        JSRunnerException(const std::string& mss)
                : std::runtime_error(mss) {
        }

        using std::runtime_error::what;
};

class JSRunner {

protected:
    JSRuntime *rt;
    JSContext *cx;

    static JSR::JSRemoteDebugger* debugger;
    static std::atomic_int debugger_ref_cnt;

    std::string source;
    std::string path;

public:

    JSRunner(std::string p_path) {
        // ensures, that no jsrunners are created simultainiously, because that will cause a crahs
        static std::mutex on_create_mtx;
        std::lock_guard<std::mutex> lock(on_create_mtx);

        debugger_ref_cnt++;

        path = p_path;
        // load source
        if (!path.empty()) {
            std::ifstream ifstr(path);
            ifstr.seekg(0, std::ios::end);
            source.reserve(ifstr.tellg());
            ifstr.seekg(0, std::ios::beg);

            source.assign((std::istreambuf_iterator<char>(ifstr)), std::istreambuf_iterator<char>());
        }

        // set up spidermonkey
        rt = JS_NewRuntime(8L * 1024L * 1024L, JS_USE_HELPER_THREADS);
        if ( !rt ) {
            throw JSRunnerException("unable to create a runtime");
        }

        cx = JS_NewContext(rt, 8192);
        if ( !cx ) {
            throw JSRunnerException("unable to create a context");
        }
        JS_SetErrorReporter(cx, &reportError);
        
        // set stack size
        const std::size_t max_stack_size = 128 * sizeof(std::size_t) * 1024;
        JS_SetNativeStackQuota(rt, max_stack_size);

        initDbg();
    }

    ~JSRunner() {

        if ( !cx ) {
            debugger->uninstall(cx);
            JS_DestroyContext(cx);
        }

        if ( !rt ) {
            JS_DestroyRuntime(rt);
        }

        debugger_ref_cnt--;
        if( debugger_ref_cnt == 0 && !debugger ) {
            debugger->stop();
            delete debugger;
        }

    }

    void initDbg() {
       static  class script_loader: public JSR::IJSScriptLoader{
            protected:
            std::map<JSContext*, std::string> sources;
            std::mutex mtx;

            public:
            void register_source(JSContext* ctx, std::string source) {
                std::lock_guard<std::mutex> locker(mtx);
                sources[ctx] = source;
            }

            int load( JSContext *ctx, const std::string &path, std::string &script ) {
                std::lock_guard<std::mutex> locker(mtx);
                try {
                    script = sources[ctx];
                    return JSR_ERROR_NO_ERROR;
                } catch (std::out_of_range& e) {
                    return JSR_ERROR_FILE_NOT_FOUND;
                }
            }
        } loader;

        loader.register_source(cx, source);

        bool needs_start = false;
        if ( !debugger ) { 
            JSR::JSRemoteDebuggerCfg dbgCfg;
            dbgCfg.setTcpHost(JSR_DEFAULT_TCP_BINDING_IP);
            dbgCfg.setTcpPort(JSR_DEFAULT_TCP_PORT);
            dbgCfg.setScriptLoader(new script_loader());

            debugger = new JSR::JSRemoteDebugger(dbgCfg);
            needs_start = true;
        }

        JSR::JSDbgEngineOptions dbgOptions;
        dbgOptions.suspended();
        if( debugger->install( cx, path, dbgOptions ) != JSR_ERROR_NO_ERROR ) {
            throw JSRunnerException("Cannot install debugger");
        }

        if(needs_start) {
            if ( debugger->start() != JSR_ERROR_NO_ERROR) {
                debugger->uninstall(cx);
                throw JSRunnerException("Cannot start debugger");
            }
        }
    }

    // runs the script
    void operator()(){
        JSAutoRequest ar(cx);
        RootedObject global(cx);

        JS::CompartmentOptions compartment_options;
        compartment_options.setVersion(JSVERSION_LATEST);
        global = JS_NewGlobalObject(cx, &globalClass, nullptr,  compartment_options);
        if ( !global ) {
            throw JSRunnerException("could not create a global object");
        }

        JSAutoCompartment ac(cx, global);

        if ( !JS_InitStandardClasses(cx, global) ) {
            throw JSRunnerException("unable to init standard classes");
        }

        if ( !JS_DefineFunctions(cx, global, globalFunctions) ) {
            throw JSRunnerException("unable to define functions");
        }

        if ( debugger->addDebuggee( cx, global ) != JSR_ERROR_NO_ERROR ) {
            throw JSRunnerException("unable to add debuggee");
        }

        jsval *some = nullptr;
        if ( !JS_EvaluateScript(cx, global, source.c_str(), source.size(), path.c_str(), 0, some) ) {
            throw JSRunnerException("could not exeute script");
        }
    }


};
JSR::JSRemoteDebugger* JSRunner::debugger;
std::atomic_int JSRunner::debugger_ref_cnt(0);

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "usage: " << argv[0] << " [script]" << std::endl;
    } else {
        std::vector<std::thread> threads;
        for( int i = 1; i < argc; i++ ) {
            threads.push_back(std::thread( [argv, i](){
                try{
                    JSRunner runner(argv[i]);
                    runner();
                    std::cout << "executed script " << i << std::endl;
                } catch (char* c) {
                    std::cout << "an error occured: " << c << std::endl; 
                } 
            }));
            std::cout << "started js instance " << i << std::endl; 
        }
        for(auto& thr : threads) {
            thr.join();
        }
        std::cout << "joined all threads" << std::endl;
    }
}
