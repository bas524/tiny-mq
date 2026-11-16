//
// Created by Alexander Bychuk on 15.03.2022.
//

#ifndef TINY_MQ__POCOANYVISITOR_H_
#define TINY_MQ__POCOANYVISITOR_H_

#include <Poco/Any.h>
#include <unordered_map>
#include <functional>
namespace Poco {
struct TypeInfoHash {
  std::size_t operator()(std::type_info const& t) const { return t.hash_code(); }
};

struct EqualRef {
  template <typename T>
  bool operator()(std::reference_wrapper<T> a, std::reference_wrapper<T> b) const {
    return a.get() == b.get();
  }
};

struct AnyVisitor {
  using TypeInfoRef = std::reference_wrapper<std::type_info const>;
  using Function = std::function<void(const Poco::Any&)>;
  std::unordered_map<TypeInfoRef, Function, TypeInfoHash, EqualRef> fs;

  template <typename T>
  void insertVisitor(std::function<void(const T&)> f) {
    // `f` is captured by value: the lambda stored in `fs` outlives this call, so it must
    // own its own copy of the visitor function rather than reference a parameter that is
    // destroyed on return. Capturing `[&f]` here left the stored lambda holding a dangling
    // reference — UB that happened to "work" under libc++ but not libstdc++ (see
    // tasks/linux-port/01-anyvisitor-dangling-capture.md).
    fs.insert(std::make_pair(std::ref(typeid(T)), Function([f = std::move(f)](const Poco::Any& x) { f(Poco::RefAnyCast<T>(x)); })));
  }

  bool operator()(const Poco::Any& x) {
    auto it = fs.find(x.type());
    if (it != fs.end()) {
      it->second(x);
      return true;
    } else {
      return false;
    }
  }
};
}  // namespace Poco
#endif  // TINY_MQ__POCOANYVISITOR_H_
