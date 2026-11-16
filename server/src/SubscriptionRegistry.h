#ifndef TINY_MQ__SUBSCRIPTION_REGISTRY_H_
#define TINY_MQ__SUBSCRIPTION_REGISTRY_H_

#include <string>
#include <utility>
#include <Poco/UUID.h>

namespace tiny_mq {
using SubscriptionIdentity = std::pair<std::string, std::string>;  // client ID, subscription name
enum class SubscriptionKind { Shared, SharedDurable, UnsharedDurable };
}
#endif
