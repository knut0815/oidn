// Copyright 2009-2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "device.h"
#include "unet.h"

namespace oidn {

  thread_local Device::ErrorState Device::globalError;

  Device::Device()
  {
    if (!mayiuse(sse41))
      throw Exception(Error::UnsupportedHardware, "SSE4.1 support is required at minimum");
  }

  Device::~Device()
  {
    observer.reset();
  }

  void Device::setError(Device* device, Error code, const std::string& message)
  {
    // Update the stored error only if the previous error was queried
    if (device)
    {
      ErrorState& curError = device->error.get();

      if (curError.code == Error::None)
      {
        curError.code = code;
        curError.message = message;
      }

      // Print the error message in verbose mode
      if (device->isVerbose())
        std::cerr << "Error: " << message << std::endl;

      // Call the error callback function
      ErrorFunction errorFunc;
      void* errorUserPtr;

      {
        std::lock_guard<std::mutex> lock(device->mutex);
        errorFunc = device->errorFunc;
        errorUserPtr = device->errorUserPtr;
      }

      if (errorFunc)
        errorFunc(errorUserPtr, code, (code == Error::None) ? nullptr : message.c_str());
    }
    else
    {
      if (globalError.code == Error::None)
      {
        globalError.code = code;
        globalError.message = message;
      }
    }
  }

  Error Device::getError(Device* device, const char** outMessage)
  {
    // Return and clear the stored error code, but keep the error message so pointers to it will
    // remain valid until the next getError call
    if (device)
    {
      ErrorState& curError = device->error.get();
      const Error code = curError.code;
      if (outMessage)
        *outMessage = (code == Error::None) ? nullptr : curError.message.c_str();
      curError.code = Error::None;
      return code;
    }
    else
    {
      const Error code = globalError.code;
      if (outMessage)
        *outMessage = (code == Error::None) ? nullptr : globalError.message.c_str();
      globalError.code = Error::None;
      return code;
    }
  }

  void Device::setErrorFunction(ErrorFunction func, void* userPtr)
  {
    errorFunc = func;
    errorUserPtr = userPtr;
  }

  int Device::get1i(const std::string& name)
  {
    if (name == "numThreads")
      return numThreads;
    else if (name == "setAffinity")
      return setAffinity;
    else if (name == "verbose")
      return verbose;
    else if (name == "version")
      return OIDN_VERSION;
    else if (name == "versionMajor")
      return OIDN_VERSION_MAJOR;
    else if (name == "versionMinor")
      return OIDN_VERSION_MINOR;
    else if (name == "versionPatch")
      return OIDN_VERSION_PATCH;
    else
      throw Exception(Error::InvalidArgument, "invalid parameter");
  }

  void Device::set1i(const std::string& name, int value)
  {
    if (name == "numThreads")
      numThreads = value;
    else if (name == "setAffinity")
      setAffinity = value;
    else if (name == "verbose")
    {
      verbose = value;
      error.verbose = value;
    }

    dirty = true;
  }

  void Device::commit()
  {
    if (isCommitted())
      throw Exception(Error::InvalidOperation, "device can be committed only once");

    // Get the optimal thread affinities
    if (setAffinity)
    {
      affinity = std::make_shared<ThreadAffinity>(1, verbose); // one thread per core
      if (affinity->getNumThreads() == 0)
        affinity.reset();
    }

    // Create the task arena
    const int maxNumThreads = affinity ? affinity->getNumThreads() : tbb::this_task_arena::max_concurrency();
    numThreads = (numThreads > 0) ? min(numThreads, maxNumThreads) : maxNumThreads;
    arena = std::make_shared<tbb::task_arena>(numThreads);

    // Automatically set the thread affinities
    if (affinity)
      observer = std::make_shared<PinningObserver>(affinity, *arena);

    // Initialize DNNL verbosity (unfortunately this is not per-device but global)
    dnnl_set_verbose(clamp(verbose - 2, 0, 2));

    dirty = false;

    if (isVerbose())
      print();
  }

  void Device::checkCommitted()
  {
    if (dirty)
      throw Exception(Error::InvalidOperation, "changes to the device are not committed");
  }

  Ref<Buffer> Device::newBuffer(size_t byteSize)
  {
    checkCommitted();
    return makeRef<Buffer>(Ref<Device>(this), byteSize);
  }

  Ref<Buffer> Device::newBuffer(void* ptr, size_t byteSize)
  {
    checkCommitted();
    return makeRef<Buffer>(Ref<Device>(this), ptr, byteSize);
  }

  Ref<Filter> Device::newFilter(const std::string& type)
  {
    checkCommitted();

    if (isVerbose())
      std::cout << "Filter: " << type << std::endl;

    Ref<Filter> filter;

    if (type == "RT")
      filter = makeRef<RTFilter>(Ref<Device>(this));
    else if (type == "RTLightmap")
      filter = makeRef<RTLightmapFilter>(Ref<Device>(this));
    else
      throw Exception(Error::InvalidArgument, "unknown filter type");

    return filter;
  }

  void Device::print()
  {
    std::cout << std::endl;

    std::cout << "Intel(R) Open Image Denoise " << OIDN_VERSION_STRING << std::endl;
    std::cout << "  Compiler: " << getCompilerName() << std::endl;
    std::cout << "  Build   : " << getBuildName() << std::endl;
    std::cout << "  Platform: " << getPlatformName() << std::endl;

    std::cout << "  Targets :";
    if (mayiuse(sse41))       std::cout << " SSE4.1";
    if (mayiuse(avx2))        std::cout << " AVX2";
    if (mayiuse(avx512_core)) std::cout << " AVX512SKX";
    std::cout << " (supported)" << std::endl;
    std::cout << "            SSE4.1 AVX2 AVX512SKX (compile time enabled)" << std::endl;

    std::cout << "  Tasking :";
    std::cout << " TBB" << TBB_VERSION_MAJOR << "." << TBB_VERSION_MINOR;
  #if TBB_INTERFACE_VERSION >= 12002
    std::cout << " TBB_header_interface_" << TBB_INTERFACE_VERSION << " TBB_lib_interface_" << TBB_runtime_interface_version();
  #else
    std::cout << " TBB_header_interface_" << TBB_INTERFACE_VERSION << " TBB_lib_interface_" << tbb::TBB_runtime_interface_version();
  #endif

    std::cout << std::endl;

    std::cout << std::endl;
  }

} // namespace oidn
