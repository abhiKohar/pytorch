#pragma once

#include <ATen/core/dispatch/OperatorEntry.h>
#include <ATen/core/dispatch/RegistrationHandleRAII.h>
#include <c10/util/Exception.h>
#include <c10/util/LeftRight.h>
#include <mutex>
#include <list>

namespace c10 {

class CAFFE2_API OperatorHandle;

/**
 * Implement this interface and register your instance with the dispatcher
 * to get notified when operators are registered or deregistered with
 * the dispatcher.
 */
class CAFFE2_API OpRegistrationListener {
public:
  virtual ~OpRegistrationListener();

  virtual void onOperatorRegistered(const OperatorHandle& op) = 0;
  virtual void onOperatorDeregistered(const OperatorHandle& op) = 0;
};

namespace detail {
class RegistrationListenerList;
}
class SchemaRegistrationHandleRAII;

/**
 * Top-level dispatch interface for dispatching via the dynamic dispatcher.
 */
class CAFFE2_API Dispatcher final {
private:
  struct OperatorDef final {
    explicit OperatorDef(FunctionSchema&& schema, OperatorOptions&& options)
    : op(std::move(schema), std::move(options)), refcount(0) {}

    impl::OperatorEntry op;
    size_t refcount;
  };
  friend class OperatorHandle;

public:
  ~Dispatcher();

  // Implementation note: this class abstracts over the fact that we have per-operator
  // dispatch tables.  This could be easily adjusted to have a single global hash
  // table.

  static Dispatcher& singleton();

  /**
   * Register a new operator schema.
   *
   * If a schema with the same operator name and overload name already exists,
   * this function will check that both schemas are exactly identical.
   *
   * @return An OperatorHandle for the registered schema which can be used to
   *         register kernels for the operator and a RegistrationHandleRAII RAII
   *         object that manages the lifetime of the registration. Once that
   *         object is destructed, the kernel will be deregistered.
   */
  std::pair<RegistrationHandleRAII, OperatorHandle> registerSchema(FunctionSchema schema, OperatorOptions options);

  /**
   * Looks for an operator schema with the given name and overload name
   * and returns it if it is registered.
   * Returns nullopt otherwise.
   */
  c10::optional<OperatorHandle> findSchema(const OperatorName& operator_name);

  /**
   * Register a kernel to the dispatch table for an operator.
   * If dispatch_key is nullopt, then this registers a fallback kernel.
   *
   * @return A RAII object that manages the lifetime of the registration.
   *         Once that object is destructed, the kernel will be deregistered.
   */
  RegistrationHandleRAII registerKernel(const OperatorHandle& op, DispatchKey dispatch_key, KernelFunction kernel);

  /**
   * Register a fallback kernel for an operator.
   * After this, when trying to lookup a kernel for an unknown dispatch key,
   * it will not fail anymore, but return the fallback kernel instead.
   *
   * @return A RAII object that manages the lifetime of the registration.
   *         Once that object is destructed, the kernel will be deregistered.
   */
  RegistrationHandleRAII registerCatchallKernel(const OperatorHandle& op, KernelFunction kernel);

  /**
   * Register a fallback kernel for a backend.
   * If an operator is called but there is no concrete kernel for the dispatch
   * key of the given operator arguments, it will check if there is such a
   * fallback kernel for the given dispatch key and, if yes, call that one.
   */
  RegistrationHandleRAII registerBackendFallbackKernel(DispatchKey dispatch_key, KernelFunction kernel);

  template<class Return, class... Args>
  Return callUnboxed(const OperatorHandle& op, Args... args) const;

  void callBoxed(const OperatorHandle& op, Stack* stack) const;

  /**
   * Add a listener that gets called whenever a new op is registered or an existing
   * op is deregistered. Immediately after registering, this listener gets called
   * for all previously registered ops, so it can be used to keep track of ops
   * registered with this dispatcher.
   */
  void addRegistrationListener(std::unique_ptr<OpRegistrationListener> listener);

private:
  Dispatcher();

  OperatorHandle findOrRegisterSchema_(FunctionSchema&& schema, OperatorOptions&& options);

  void deregisterSchema_(const OperatorHandle& op, const OperatorName& op_name);
  void deregisterBackendFallbackKernel_(DispatchKey dispatchKey);

  const KernelFunction& dispatch_(const DispatchTable& dispatchTable, c10::optional<DispatchKey> dispatch_key) const;

  std::list<OperatorDef> operators_;
  LeftRight<ska::flat_hash_map<OperatorName, OperatorHandle>> operatorLookupTable_;
  impl::KernelFunctionTable backendFallbackKernels_;
  std::unique_ptr<detail::RegistrationListenerList> listeners_;
  std::mutex mutex_;
};

/**
 * This is a handle to an operator schema registered with the dispatcher.
 * This handle can be used to register kernels with the dispatcher or
 * to lookup a kernel for a certain set of arguments.
 */
class CAFFE2_API OperatorHandle final {
public:
  OperatorHandle(OperatorHandle&&) noexcept = default;
  OperatorHandle& operator=(OperatorHandle&&) noexcept = default;
  OperatorHandle(const OperatorHandle&) = default;
  OperatorHandle& operator=(const OperatorHandle&) = default;

  const FunctionSchema& schema() const {
    return operatorIterator_->op.schema();
  }

  const OperatorOptions& options() const {
    return operatorIterator_->op.options();
  }

  template<class Return, class... Args>
  Return callUnboxed(Args... args) const {
    return c10::Dispatcher::singleton().callUnboxed<Return, Args...>(*this, std::forward<Args>(args)...);
  }

  void callBoxed(Stack* stack) const {
    c10::Dispatcher::singleton().callBoxed(*this, stack);
  }

private:
  explicit OperatorHandle(std::list<Dispatcher::OperatorDef>::iterator operatorIterator)
  : operatorIterator_(std::move(operatorIterator)) {}
  friend class Dispatcher;

  std::list<Dispatcher::OperatorDef>::iterator operatorIterator_;
};

namespace detail {
template<class... Args> inline void unused_arg_(const Args&...) {}
}

template<class Return, class... Args>
inline Return Dispatcher::callUnboxed(const OperatorHandle& op, Args... args) const {
  detail::unused_arg_(args...);  // workaround for a false-positive warning about unused parameters in gcc 5
  const auto& dispatchTable = op.operatorIterator_->op.dispatch_table();
  c10::optional<DispatchKey> dispatchKey = dispatchTable.dispatchKeyExtractor().getDispatchKeyUnboxed<Args...>(args...);
  const KernelFunction& kernel = dispatch_(dispatchTable, dispatchKey);
  return kernel.template callUnboxed<Return, Args...>(op, std::forward<Args>(args)...);
}

inline void Dispatcher::callBoxed(const OperatorHandle& op, Stack* stack) const {
  // note: this doesn't need the mutex because write operations on the list keep iterators intact.
  const auto& dispatchTable = op.operatorIterator_->op.dispatch_table();
  c10::optional<DispatchKey> dispatchKey = dispatchTable.dispatchKeyExtractor().getDispatchKeyBoxed(stack);
  const KernelFunction& kernel = dispatch_(dispatchTable, dispatchKey);
  kernel.callBoxed(op, stack);
}

inline const KernelFunction& Dispatcher::dispatch_(const DispatchTable& dispatchTable, c10::optional<DispatchKey> dispatchKey) const {
  if (C10_LIKELY(dispatchKey.has_value())) {

    const KernelFunction* backendKernel = dispatchTable.lookup(*dispatchKey);

    if (nullptr != backendKernel) {
      return *backendKernel;
    }

    const auto& backendFallbackKernel = backendFallbackKernels_[*dispatchKey];
    if (backendFallbackKernel.isValid()) {
      return backendFallbackKernel;
    }
  }

  const KernelFunction* catchallKernel = dispatchTable.lookupCatchallKernel();
  if (C10_LIKELY(nullptr != catchallKernel)) {
    return *catchallKernel;
  }

  if (!dispatchKey.has_value() || *dispatchKey == DispatchKey::UndefinedTensorId) {
    TORCH_CHECK(false,
          "There were no tensor arguments to this function (e.g., you passed an "
          "empty list of Tensors), but no fallback function is registered for schema ", dispatchTable.operatorName(),
          ".  This usually means that this function requires a non-empty list of Tensors.  "
          "Available functions are ", dispatchTable.listAllDispatchKeys())
  }

  const std::string dispatchKeyStr = toString(*dispatchKey);
  TORCH_CHECK(false, "Could not run '", dispatchTable.operatorName(), "' with arguments",
          " from the '", dispatchKeyStr, "' backend. '",
          dispatchTable.operatorName(), "' is only available for these backends: ",
          dispatchTable.listAllDispatchKeys(), ".");
}

} // namespace c10
