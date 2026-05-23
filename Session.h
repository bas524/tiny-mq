//
// Created by Alexander Bychuk on 03.11.2022.
//

#ifndef TINY_MQ__SESSION_H_
#define TINY_MQ__SESSION_H_

#include "Exchange.h"
#include "TextMessage.h"
#include "MapMessage.h"
#include "StreamMessage.h"
#include "BytesMessage.h"
#include "ObjectMessage.h"
#include "Selector.h"
#include "HashHelper.h"
#include <Poco/UUIDGenerator.h>

namespace tiny_mq {
class Connection;
class Session {
 public:
  enum AcknowledgeMode {

    /**
     * Messages will be consumed when the transaction commits.
     */
    SESSION_TRANSACTED = 0,

    /**
     * With this acknowledgment mode, the session automatically
     * acknowledges a client's receipt of a message either when
     * the session has successfully returned from a call to receive
     * or when the message listener the session has called to
     * process the message successfully returns.
     */
    AUTO_ACKNOWLEDGE = 1,

    /**
     * With this acknowledgment mode, the client acknowledges a
     * consumed message and all before by calling the message's acknowledge method.
     */
    CLIENT_ACKNOWLEDGE = 2,

    /**
     * With this acknowledgment mode, the client acknowledges a
     * consumed message by calling the message's acknowledge method.
     */
    INDIVIDUAL_ACKNOWLEDGE = 3,

    /**
     * Lazy, batched acknowledgement. The session may acknowledge the
     * delivery of messages with reduced effort; on failure duplicates may be
     * redelivered. Suited to high-throughput consumers that tolerate duplicates.
     */
    DUPS_OK_ACKNOWLEDGE = 4
  };

  ~Session();

  Destination::Ptr createDestination(destination::Type type, const std::string &destinationName);
  Consumer::Ptr createConsumer(const Destination::Ptr &destination);
  Consumer::Ptr createConsumer(const Destination::Ptr &destination, const std::string &selector);
  void deleteConsumer(const Poco::UUID &id);
  Producer::Ptr createProducer(const Destination::Ptr &destination);
  void deleteProducer(const Poco::UUID &id);
  Consumer::Ptr createConsumer(Destination &destination);
  Consumer::Ptr createConsumer(Destination &destination, const std::string &selector);
  Producer::Ptr createProducer(Destination &destination);

  // Durable subscriber API (topic-family destinations only)
  Consumer::Ptr createDurableConsumer(const Destination::Ptr &destination, const std::string &subscriptionName);
  Consumer::Ptr createDurableConsumer(const Destination::Ptr &destination, const std::string &subscriptionName, const std::string &selectorExpr);
  Consumer::Ptr createDurableConsumer(Destination &destination, const std::string &subscriptionName);
  Consumer::Ptr createDurableConsumer(Destination &destination, const std::string &subscriptionName, const std::string &selectorExpr);
  void unsubscribe(Destination &destination, const std::string &subscriptionName);
  void unsubscribe(const Destination::Ptr &destination, const std::string &subscriptionName);
  TextMessage createTextMessage(std::string text = "", Message::Reliability reliability = Message::NOT_PERSISTENT);
  MapMessage createMapMessage(Message::Reliability reliability = Message::NOT_PERSISTENT);
  StreamMessage createStreamMessage(Message::Reliability reliability = Message::NOT_PERSISTENT);
  BytesMessage createBytesMessage(BytesVector bytes, Message::Reliability reliability = Message::NOT_PERSISTENT);
  BytesMessage createBytesMessage(const int8_t *bytes, size_t size, Message::Reliability reliability = Message::NOT_PERSISTENT);
  ObjectMessage createObjectMessage(BytesVector body, std::string className, Message::Reliability reliability = Message::NOT_PERSISTENT);

  Session::AcknowledgeMode acknowledgeMode() const;
  std::string acknowledgeModeName() const;
  static std::string getAcknowledgeModeName(Session::AcknowledgeMode mode);

  void commit();

  void rollback();

  const std::string &transactionId() const;
  Poco::UUID createRandomUUID() const;
  const std::string &id();

  Connection &connection();

 private:
  // Sessions are created only through Connection::createSession.
  Session(Connection &connection, AcknowledgeMode mode = AcknowledgeMode::AUTO_ACKNOWLEDGE);
  friend class Connection;

  AcknowledgeMode _mode;
  std::reference_wrapper<Poco::Logger> _logger;
  std::reference_wrapper<Connection> _connection;
  std::string _suffix;
  mutable Poco::UUIDGenerator _uuidGenerator;
  const std::string _id;
  phmap::parallel_node_hash_map<size_t, Destination::Ptr> _destinations;
  phmap::parallel_node_hash_map<Poco::UUID, Producer::Ptr> _producers;
  phmap::parallel_node_hash_map<Poco::UUID, Consumer::Ptr> _consumers;
};
}  // namespace tiny_mq

#endif  // TINY_MQ__SESSION_H_
