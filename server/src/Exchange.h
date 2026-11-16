#ifndef TINY_MQ__EXCHANGE_H_
#define TINY_MQ__EXCHANGE_H_

#include <map>
#include <mutex>
#include "Destination.h"
#include "SubscriptionRegistry.h"

namespace tiny_mq {
class Exchange {
 public:
  explicit Exchange(Poco::Path path);
  virtual ~Exchange();
  Exchange(const Exchange&) = delete;
  Exchange& operator=(const Exchange&) = delete;

 private:
  struct SubscriptionDefinition {
    SubscriptionKind kind;
    std::string topicUri;
    std::string selector;
    bool noLocal;
    Poco::UUID generation;
  };

  struct RegisteredSubscription {
    SubscriptionDefinition definition;
    Destination::Ptr topic;
    std::shared_ptr<Destination::Subscription> state;
  };

  using Registry = std::map<SubscriptionIdentity, RegisteredSubscription>;

  Destination::Ptr create(destination::Type type, const std::string& name);
  Destination::Ptr createPermanent(destination::Type type, const std::string& name);
  TemporaryDestination::Ptr createTemporary(destination::Type type, const Poco::UUID& owner);
  void removeTemporary(Destination& destination, const Poco::UUID& owner);
  void removeTemporary(const Poco::UUID& owner);

  Consumer::Ptr subscribe(Session& session, Destination& topic, SubscriptionKind kind,
                          const std::string& name, const std::string& selector, bool noLocal);
  void forgetShared(const SubscriptionIdentity& key, const Destination::Subscription& state);
  void unsubscribe(const std::string& clientId, const std::string& name);

  void loadRegistry();
  void writeRegistry(const Registry& registry);
  RegisteredSubscription openDurable(const SubscriptionIdentity& key, SubscriptionDefinition definition,
                                     const Destination::Ptr& topic, bool load);
  static bool inUse(const RegisteredSubscription& entry);
  static void removeSubscription(RegisteredSubscription& entry);

  std::map<size_t, Destination::Ptr> _destinations;
  Poco::Path _path;
  Poco::Path _registryPath;
  std::reference_wrapper<Poco::Logger> _logger;
  std::mutex _mutex;
  Registry _durable;
  Registry _shared;

  friend class Connection;
  friend class Destination;
  friend class Session;
  friend class TemporaryDestination;
};
}  // namespace tiny_mq
#endif
