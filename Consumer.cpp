//
// Created by Alexander Bychuk on 10.11.2021.
//

#include "Consumer.h"
#include "Destination.h"
#include <Poco/File.h>
#include <Poco/FileStream.h>
#include <Poco/Format.h>
#include <Poco/UUIDGenerator.h>
#include "LogTracer.h"
namespace tiny_mq {
Consumer::Consumer(Destination& destination, std::shared_ptr<QueueT> queue, const Poco::UUID& uuid, Poco::Path path)
    : _uuid(uuid),
      _path(std::move(path)),
      _destination(destination),
      _queue(std::move(queue)),
      _token(*_queue),
      _logger(Poco::Logger::get(Poco::format("tiny_mq.consumer.%s", _uuid.toString()))) {
  TRACE(_logger);
  Poco::File dir(_path);
  dir.createDirectories();
  _sent = _path;
  _sent.append("sent").makeDirectory();
  dir = _sent;
  dir.createDirectories();
  poco_information(_logger, Poco::format("create consumer[%s] for %s", _uuid.toString(), _destination.uri()));
}
Consumer::~Consumer() {
  TRACE(_logger);
  _needToStop = true;
  poco_information(_logger, Poco::format("destroy consumer[%s] for %s", _uuid.toString(), _destination.uri()));
  _messages.clear();
}
const Poco::UUID& Consumer::id() const {
  TRACE(_logger);
  return _uuid;
}
Message::Ptr Consumer::recv(int64_t usec_timeout) {
  TRACE(_logger);
  Message::Ptr message;
  do {
    message.reset();
    if (_needToStop) {
      break;
    }
  } while (!_queue->wait_dequeue_timed(_token, message, usec_timeout));
  if (message) {
    if (message->isPersistent()) {
      Poco::File fmsg(message->persistentInfo.fileFromName);
      fmsg.renameTo(message->persistentInfo.fileToName);
    }
    poco_debug(
        _logger,
        Poco::format("recv message[%?d][%s] from %s://%s", message->_number, message->uuid.toString(), _destination.typeName(), _destination.name()));
  }
  return message;
}
Message::Ptr Consumer::preparePush(int64_t number, const Message& message) {
  TRACE(_logger);

  Message::Ptr copyMessage = message.copy();
  if (copyMessage->uuid.isNull()) {
    copyMessage->uuid = _uuidGenerator.createRandom();
  }
  copyMessage->_number = number;
  if (copyMessage->isPersistent()) {
    std::string uuid = copyMessage->uuid.toString();
    copyMessage->persistentInfo.fileFromName = Poco::format("%s%?d.%s.message", _path.toString(), copyMessage->_number, uuid);
    copyMessage->persistentInfo.fileToName = Poco::format("%s%?d.%s.message", _sent.toString(), copyMessage->_number, uuid);
    Poco::FileOutputStream fmsg(copyMessage->persistentInfo.fileFromName);
    const auto& data = Poco::RefAnyCast<std::string>(copyMessage->data);
    fmsg.write(data.c_str(), data.size());
    _messages.emplace(uuid, copyMessage->persistentInfo.fileFromName);
  }
  return copyMessage;
}
void Consumer::push(int64_t number, const QueueT::producer_token_t& token, const Message& message) {
  TRACE(_logger);
  _queue->enqueue(token, preparePush(number, message));
  poco_debug(_logger,
             Poco::format("save message[%?d][%s] to %s://%s", number, message.uuid.toString(), _destination.typeName(), _destination.name()));
}
void Consumer::push(int64_t number, const Message& message) {
  TRACE(_logger);
  _queue->enqueue(preparePush(number, message));
  poco_debug(_logger,
             Poco::format("save message[%?d][%s] to %s://%s", number, message.uuid.toString(), _destination.typeName(), _destination.name()));
}
moodycamel::BlockingConcurrentQueue<Message::Ptr>::producer_token_t Consumer::getProducerToken() {
  TRACE(_logger);
  return QueueT::producer_token_t(*_queue);
}
void Consumer::stop() {
  TRACE(_logger);
  _needToStop = true;
}
void Consumer::acknowledgeOn(const Message& message) {
  TRACE(_logger);
  std::string uuid = message.uuid.toString();
  if (message.isPersistent()) {
    _messages.erase(uuid);
    std::string fname = message.persistentInfo.fileToName;
    if (message.persistentInfo.fileToName.empty()) {
      fname = Poco::format("%s%?d.%s.message", _sent.toString(), message._number, uuid);
    }
    Poco::File fmsg(fname);
    fmsg.remove();
  }
  poco_debug(_logger, Poco::format("ack on message[%?d][%s] to %s://%s", message._number, uuid, _destination.typeName(), _destination.name()));
}
}  // namespace tiny_mq