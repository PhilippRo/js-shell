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

#include <jsapi.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
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

class JSRunner {

protected:
    JSRuntime *rt;
    JSContext *cx;
    std::string source;
    std::string path;

public:

    JSRunner(std::string p_path) {
        static std::mutex on_create_mtx;
        std::lock_guard<std::mutex> lock(on_create_mtx);

        path = p_path;
        // load source
        if (!path.empty()) {
            std::ifstream ifstr(path);
            ifstr.seekg(0, std::ios::end);
            source.reserve(ifstr.tellg());
            ifstr.seekg(0, std::ios::beg);

            source.assign((std::istreambuf_iterator<char>(ifstr)), std::istreambuf_iterator<char>());
        }

        // init JS engine
        //    JS_Init
        //if ( !JS_Init() ) {
        //    throw "unable to init engine";
        //}
        std::cout << "creating a new runtime" << std::endl;
        rt = JS_NewRuntime(8L * 1024L * 1024L, JS_USE_HELPER_THREADS);
        std::cout << "created a new runtime" << std::endl;
        if ( !rt ) {
            throw "unable to create a runtime";
        }

        cx = JS_NewContext(rt, 8192);
        if ( !cx ) {
            throw "unable to create a context";
        }
        JS_SetErrorReporter(cx, &reportError);
        
    }

    ~JSRunner() {
        if ( !cx ) {
            JS_DestroyContext(cx);
        }

        if ( !rt ) {
            JS_DestroyRuntime(rt);
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
            throw "could not create a global object";
        }

        JSAutoCompartment ac(cx, global);

        if ( !JS_InitStandardClasses(cx, global) ) {
            throw "unable to init standard classes";
        }

        if ( !JS_DefineFunctions(cx, global, globalFunctions) ) {
            throw "unable to define functions";
        }

        jsval *some = nullptr;
        if ( !JS_EvaluateScript(cx, global, source.c_str(), source.size(), path.c_str(), 0, some) ) {
            throw "could not exeute script";
        }
    }


};


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
        }
        for(auto& thr : threads) {
            thr.join();
        }
        std::cout << "joined all threads" << std::endl;
    }
}
