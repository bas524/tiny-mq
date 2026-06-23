//
// ConnectionMetaData — JMS 2.0 § 6.5.
//
// Static description of the provider, exposed via Connection::metadata().
// The same values are reported for in-process and (later) network connections,
// so it documents the broker regardless of transport.
//

#ifndef TINY_MQ__CONNECTION_META_DATA_H_
#define TINY_MQ__CONNECTION_META_DATA_H_

#include <string>
#include <vector>

namespace tiny_mq {

struct ConnectionMetaData {
  // Provider identity.
  std::string providerName{"tiny-mq"};
  std::string providerVersion{"0.1"};
  int providerMajorVersion{0};
  int providerMinorVersion{1};

  // Messaging semantics targeted by the broker.
  std::string jmsVersion{"2.0"};
  int jmsMajorVersion{2};
  int jmsMinorVersion{0};

  // JMSX-defined properties the broker understands.  Empty until a spec that
  // introduces one (e.g. JMSXDeliveryCount in spec 24) lands.
  std::vector<std::string> jmsxPropertyNames{};
};

}  // namespace tiny_mq

#endif  // TINY_MQ__CONNECTION_META_DATA_H_
