//
// Created by Alexander Bychuk on 04.11.2021.
//

#include "Destination.h"
#include "LogTracer.h"
#include <Poco/File.h>
#include "DestinationHash.h"
namespace tiny_mq {
Destination::Destination(destination::Type type, std::string name, Poco::Path path)
    : _type(type),
      _name(std::move(name)),
      _path(std::move(path)),
      _logger(Poco::Logger::get(Poco::format("tiny_mq.destination.%s.%s", destination::TypeName(_type), _name))),
      _uri(Poco::format("%s://%s", destination::TypeName(_type), _name)),
      _hash(destination::hash(_type, _name)) {
  TRACE(_logger);

  if (_type == destination::Queue || _type == destination::TemporaryQueue) {
    _queue = std::make_shared<QueueT>();
    _defaultConsumer = Consumer::Ptr(new Consumer(*this, _queue, _uuidGenerator.createRandom(), _path));
    _consumers.emplace(_defaultConsumer->id().toString(), _defaultConsumer);
  }
}
void Destination::save(const Producer& producer, const Message& message) {
  TRACE(_logger);
  int64_t number = _messageCounter++;

  for (auto& item : _consumers) {
    if (_type == destination::Queue || _type == destination::TemporaryQueue) {
      item.second->push(number, producer.token(), message);
      break;
    } else {
      item.second->push(number, message);
    }
  }
}
Consumer::Ptr Destination::createConsumer() {
  TRACE(_logger);
  auto id = _uuidGenerator.createRandom();
  Consumer::Ptr consumer;
  std::string sid = id.toString();
  switch (_type) {
    case destination::Queue:
    case destination::TemporaryQueue: {
      if (_defaultConsumer) {
        return Consumer::Ptr(std::move(_defaultConsumer));
      }
      consumer = Consumer::Ptr(new Consumer(*this, _queue, id, _path));
    } break;
    case destination::Topic:
    case destination::TemporaryTopic:
      Poco::Path path(_path);
      path.append(sid).makeDirectory();
      consumer = Consumer::Ptr(new Consumer(*this, std::make_shared<QueueT>(), id, path));
      break;
  }
  auto it = _consumers.emplace(sid, std::move(consumer));
  if (it.second) {
    return it.first->second;
  }
  return nullptr;
}
Producer::Ptr Destination::createProducer() {
  TRACE(_logger);
  using TokenType = moodycamel::BlockingConcurrentQueue<Message::Ptr>::producer_token_t;
  std::unique_ptr<TokenType> token;
  if (_type == destination::Queue || _type == destination::TemporaryQueue) {
    token = std::make_unique<TokenType>(_consumers.begin()->second->getProducerToken());
  }
  auto id = _uuidGenerator.createRandom();
  auto producer = Producer::Ptr(new Producer(*this, id, std::move(token)));

  auto it = _producers.emplace(id.toString(), std::move(producer));
  if (it.second) {
    return it.first->second;
  }
  return nullptr;
}
size_t Destination::consumersCount() const {
  TRACE(_logger);
  return _consumers.size();
}
destination::Type Destination::type() const {
  TRACE(_logger);
  return _type;
}
const std::string& Destination::name() const {
  TRACE(_logger);
  return _name;
}
std::string Destination::typeName() const {
  TRACE(_logger);
  return destination::TypeName(_type);
}
size_t Destination::producersCount() const {
  TRACE(_logger);
  return _producers.size();
}
const std::string& Destination::uri() const { return _uri; }
Destination::~Destination() {
  TRACE(_logger);
  _consumers.clear();
  _producers.clear();
}
size_t Destination::hash() const { return _hash; }
}  // namespace tiny_mq